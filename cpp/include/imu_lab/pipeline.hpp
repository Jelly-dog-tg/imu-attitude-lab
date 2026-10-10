// Multithreaded processing pipeline: receiver -> workers -> logger.
//
// Thread topology
// ---------------
//
//   [receiver thread]                     1 x
//        reads the FrameSource, stamps arrival time, pushes RawFrame
//        -> ThreadSafeQueue<RawFrame> raw_queue_        (bounded)
//
//   [worker threads]                      N x
//        parse + validate, measure decode time, push OutputItem
//        -> ThreadSafeQueue<OutputItem> output_queue_   (bounded)
//
//   [logger thread]                       1 x
//        restore sequence order, run jump detection, write CSV,
//        accumulate statistics and sample RSS
//
// Why this split
// --------------
// * Parsing is the only CPU-heavy step, so it is the only thing replicated.
// * Jump detection compares adjacent samples, so it is order dependent and
//   therefore lives on the single logger thread. Doing it on workers would make
//   the result depend on thread scheduling.
// * Shutdown is driven by queue closure, never by a sleep or a timed poll:
//   receiver finishes -> closes raw queue -> workers drain and exit -> main
//   closes output queue -> logger drains and exits. No frame is lost and no
//   thread is ever killed mid-write.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "imu_lab/error.hpp"
#include "imu_lab/frame_source.hpp"
#include "imu_lab/jump_detector.hpp"
#include "imu_lab/metrics.hpp"

namespace imu_lab {

/// Tunables for one run.
struct PipelineConfig {
    /// Number of parsing threads. 0 is treated as 1.
    std::size_t worker_threads = 2;
    /// Max frames waiting to be parsed; 0 means unbounded.
    std::size_t queue_capacity = 4096;
    /// Max completed frames waiting to be logged; 0 means unbounded.
    std::size_t output_queue_capacity = 4096;
    /// Threshold in degrees for `JumpDetector`.
    double jump_threshold_deg = JumpDetector::kDefaultThresholdDeg;
    /// Sample process RSS every N logged frames (0 disables sampling).
    std::size_t rss_sample_every = 4096;
    /// Keep at most this many jump events in the returned vector.
    std::size_t jump_cap = 256;
};

/// Outcome of one pipeline run.
struct PipelineRun {
    /// `ok()` unless something prevented a complete run.
    Status status = success();
    PipelineStats stats;
    std::vector<JumpEvent> jumps;
    /// Path of the written CSV, empty when no output was requested.
    std::string csv_path;
    /// Frames kept for diagnostics (rejected samples), capped.
    std::vector<std::string> sample_rejections;
};

/// Runs the receiver/worker/logger pipeline for a given source.
class AttitudePipeline {
public:
    explicit AttitudePipeline(PipelineConfig config);

    /// Process the whole source.
    ///
    /// @param source      frames to consume; the pipeline does not own it.
    /// @param csv_path    when non-empty, write the normalised log here.
    ///                    A file that cannot be opened is reported through
    ///                    `PipelineRun::status`.
    [[nodiscard]] PipelineRun run(FrameSource& source, const std::string& csv_path = {});

    /// Ask the pipeline to stop early (safe to call from another thread).
    void request_stop() noexcept { stop_requested_.store(true, std::memory_order_relaxed); }

    [[nodiscard]] const PipelineConfig& config() const noexcept { return config_; }

private:
    PipelineConfig config_;
    std::atomic<bool> stop_requested_{false};
};

}  // namespace imu_lab
