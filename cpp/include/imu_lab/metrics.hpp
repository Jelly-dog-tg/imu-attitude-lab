// Performance and stability metrics for the multithreaded pipeline.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "imu_lab/statistics.hpp"

namespace imu_lab {

/// Resident set size of the current process, in bytes.
///
/// Uses `GetProcessMemoryInfo` on Windows and `/proc/self/statm` on Linux.
/// Returns 0 when the platform cannot report it, so callers must never treat 0
/// as "no memory used".
[[nodiscard]] std::size_t current_rss_bytes();

/// Accumulator for a timing series (min / mean / max).
///
/// Deliberately does not retain samples: the pipeline must stay allocation-free
/// on the hot path, so percentiles are out of scope here.
class LatencyStats {
public:
    void add(double milliseconds) noexcept;

    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] double total_ms() const noexcept { return total_ms_; }
    [[nodiscard]] double min_ms() const noexcept { return count_ ? min_ms_ : 0.0; }
    [[nodiscard]] double max_ms() const noexcept { return count_ ? max_ms_ : 0.0; }
    [[nodiscard]] double mean_ms() const noexcept {
        return count_ ? total_ms_ / static_cast<double>(count_) : 0.0;
    }

    void merge(const LatencyStats& other) noexcept;
    void reset() noexcept;

private:
    std::uint64_t count_ = 0;
    double total_ms_ = 0.0;
    double min_ms_ = 0.0;
    double max_ms_ = 0.0;
};

/// Everything measured during one pipeline run.
struct PipelineStats {
    std::size_t workers = 0;
    std::size_t queue_capacity = 0;

    std::uint64_t frames_received = 0;   ///< pushed by the receiver thread
    std::uint64_t frames_parsed = 0;     ///< successfully decoded by workers
    std::uint64_t frames_rejected = 0;   ///< failed to decode ("anomalous frames")
    std::uint64_t frames_logged = 0;     ///< written by the logger thread

    std::size_t queue_high_water = 0;    ///< peak backlog in the parse queue
    std::size_t output_high_water = 0;   ///< peak backlog in the output queue

    LatencyStats parse_latency_ms;       ///< decoder time per frame
    LatencyStats end_to_end_latency_ms;  ///< receive -> logged

    double wall_ms = 0.0;
    std::size_t rss_start_bytes = 0;
    std::size_t rss_end_bytes = 0;
    std::size_t rss_peak_bytes = 0;

    AngleStats pitch;
    AngleStats roll;
    std::size_t jump_count = 0;

    /// Fraction of received frames that could not be decoded.
    [[nodiscard]] double anomaly_ratio() const noexcept;
    /// Frames ingested per second of wall time.
    [[nodiscard]] double throughput_fps() const noexcept;
    /// Peak backlog as a fraction of capacity (0 when unbounded).
    [[nodiscard]] double backpressure_ratio() const noexcept;
    /// Frames that entered the pipeline but were neither parsed nor rejected.
    /// Must be 0: a non-zero value means the queue hand-off dropped frames.
    [[nodiscard]] std::uint64_t lost_frames() const noexcept;

    /// Multi-line human-readable report.
    [[nodiscard]] std::string render() const;
    /// Single-object JSON document (no external dependency, ASCII-safe keys).
    [[nodiscard]] std::string to_json() const;
};

}  // namespace imu_lab
