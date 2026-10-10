#include "imu_lab/pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <thread>
#include <utility>

#include "imu_lab/protocol_parser.hpp"
#include "imu_lab/statistics.hpp"
#include "imu_lab/thread_safe_queue.hpp"

namespace imu_lab {
namespace {

/// One unit of work travelling from a worker thread to the logger thread.
struct OutputItem {
    std::uint64_t sequence = 0;
    double elapsed_s = 0.0;
    double arrival_s = 0.0;
    /// Monotonic time at which the worker finished with this frame.
    double ready_s = 0.0;
    bool decoded = false;
    AttitudeSample sample;
    double parse_ms = 0.0;
    /// The original protocol line, moved (never copied) from the RawFrame so the
    /// CSV keeps the exact bytes the device or file produced.
    std::string line;
    /// Populated only when `decoded` is false.
    std::string reason;
};

/// Per-worker accumulators.
///
/// Each worker owns its own counters and latency series and they are merged
/// after `join()`. That keeps the hot path free of atomics: contention on a
/// shared counter is a classic way to make a "parallel" pipeline end up slower
/// than the single-threaded version.
struct WorkerAccumulator {
    std::uint64_t parsed = 0;
    std::uint64_t rejected = 0;
    LatencyStats parse_latency_ms;
};

[[nodiscard]] double monotonic_seconds() {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

/// ISO-8601 UTC timestamp with millisecond precision.
///
/// Written by hand so the C++ log stays byte-compatible with the Python one
/// (which uses `datetime.now(timezone.utc).isoformat(timespec="milliseconds")`).
[[nodiscard]] std::string format_utc(std::chrono::system_clock::time_point point) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(point);
    const auto since_epoch = point.time_since_epoch();
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch).count() % 1000;

    std::tm broken_down{};
#if defined(_WIN32)
    gmtime_s(&broken_down, &seconds);
#else
    gmtime_r(&seconds, &broken_down);
#endif

    char buffer[40] = {};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  broken_down.tm_year + 1900, broken_down.tm_mon + 1, broken_down.tm_mday,
                  broken_down.tm_hour, broken_down.tm_min, broken_down.tm_sec,
                  static_cast<int>(millis));
    return buffer;
}

/// Quote a CSV field the way Python's csv module does: only when needed, and
/// with any embedded quote doubled.
[[nodiscard]] std::string csv_field(const std::string& value) {
    if (value.find_first_of(",\"\r\n") == std::string::npos) {
        return value;
    }
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('"');
    for (const char character : value) {
        if (character == '"') {
            quoted.push_back('"');
        }
        quoted.push_back(character);
    }
    quoted.push_back('"');
    return quoted;
}

[[nodiscard]] std::string fixed(double value, int precision) {
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, value);
    return buffer;
}

}  // namespace

AttitudePipeline::AttitudePipeline(PipelineConfig config) : config_(config) {
    if (config_.worker_threads == 0) {
        config_.worker_threads = 1;
    }
}

PipelineRun AttitudePipeline::run(FrameSource& source, const std::string& csv_path) {
    PipelineRun result;
    stop_requested_.store(false, std::memory_order_relaxed);

    // Validate the threshold before starting any thread: a bad value is a
    // configuration error, and failing here keeps the shutdown path trivial.
    std::unique_ptr<JumpDetector> detector;
    try {
        detector = std::make_unique<JumpDetector>(config_.jump_threshold_deg);
    } catch (const std::invalid_argument& error) {
        result.status = Error(ErrorCode::internal, error.what());
        return result;
    }

    std::ofstream csv;
    if (!csv_path.empty()) {
        csv.open(csv_path, std::ios::binary | std::ios::trunc);
        if (!csv) {
            result.status =
                Error(ErrorCode::file_not_writable, "cannot open output file: " + csv_path);
            return result;
        }
        csv << "timestamp_utc,elapsed_s,pitch_deg,roll_deg,x,y,raw_line\n";
        result.csv_path = csv_path;
    }

    const std::size_t worker_count = config_.worker_threads;
    ThreadSafeQueue<RawFrame> raw_queue(config_.queue_capacity);
    ThreadSafeQueue<OutputItem> output_queue(config_.output_queue_capacity);

    std::vector<WorkerAccumulator> accumulators(worker_count);
    std::atomic<std::uint64_t> received{0};
    std::atomic<bool> receiver_failed{false};
    std::atomic<std::uint64_t> out_of_order_dropped{0};

    // Owned by the logger thread; read only after `logger.join()`.
    RunningStats pitch_stats;
    RunningStats roll_stats;

    const double started_s = monotonic_seconds();
    const auto started_wall = std::chrono::system_clock::now();
    const std::size_t rss_start = config_.rss_sample_every ? current_rss_bytes() : 0;

    // ---------------------------------------------------------------- logger
    std::thread logger([&] {
        std::map<std::uint64_t, OutputItem> pending;
        std::uint64_t next_sequence = 0;
        std::size_t rss_peak = rss_start;

        auto emit = [&](OutputItem&& item) {
            const double emitted_s = monotonic_seconds();
            result.stats.end_to_end_latency_ms.add((emitted_s - item.arrival_s) * 1000.0);

            if (item.decoded) {
                const AttitudeSample& sample = item.sample;
                pitch_stats.add(sample.pitch_deg);
                roll_stats.add(sample.roll_deg);

                if (const std::optional<JumpEvent> event =
                        detector->observe(item.sequence, item.elapsed_s, sample)) {
                    ++result.stats.jump_count;
                    if (result.jumps.size() < config_.jump_cap) {
                        result.jumps.push_back(*event);
                    }
                }

                if (csv.is_open()) {
                    const auto stamp =
                        started_wall +
                        std::chrono::duration_cast<std::chrono::system_clock::duration>(
                            std::chrono::duration<double>(item.elapsed_s));
                    csv << format_utc(stamp) << ',' << fixed(item.elapsed_s, 6) << ','
                        << fixed(sample.pitch_deg, 6) << ',' << fixed(sample.roll_deg, 6) << ','
                        << (sample.x ? fixed(*sample.x, 6) : std::string()) << ','
                        << (sample.y ? fixed(*sample.y, 6) : std::string()) << ','
                        << csv_field(item.line) << '\n';
                }
            } else if (result.sample_rejections.size() < 8) {
                result.sample_rejections.push_back(item.reason + " | input: " + item.line);
            }

            ++result.stats.frames_logged;
            if (config_.rss_sample_every != 0 &&
                result.stats.frames_logged % config_.rss_sample_every == 0) {
                const std::size_t rss = current_rss_bytes();
                if (rss > rss_peak) {
                    rss_peak = rss;
                }
            }
        };

        // Emit everything that is now contiguous. Working on a map keyed by
        // sequence number is what makes the output independent of which worker
        // happened to finish first.
        auto drain_ready = [&] {
            for (auto iterator = pending.find(next_sequence); iterator != pending.end();
                 iterator = pending.find(next_sequence)) {
                emit(std::move(iterator->second));
                pending.erase(iterator);
                ++next_sequence;
            }
        };

        OutputItem item;
        while (output_queue.pop(item)) {
            if (item.sequence < next_sequence) {
                out_of_order_dropped.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            pending.emplace(item.sequence, std::move(item));
            drain_ready();
        }

        // Queue closed and drained: flush stragglers in sequence order so a slow
        // worker's frame is never silently lost.
        for (auto& entry : pending) {
            emit(std::move(entry.second));
        }

        if (csv.is_open()) {
            csv.flush();
        }
        result.stats.rss_peak_bytes = rss_peak;
    });

    // -------------------------------------------------------------- workers
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        workers.emplace_back([&, index] {
            WorkerAccumulator& accumulator = accumulators[index];
            RawFrame frame;
            while (raw_queue.pop(frame)) {
                OutputItem item;
                item.sequence = frame.sequence;
                item.elapsed_s = frame.elapsed_s;
                item.arrival_s = frame.arrival_s;

                const double begin_s = monotonic_seconds();
                Result<AttitudeSample> parsed =
                    AttitudeLineParser::parse_attitude_line(frame.line);
                const double end_s = monotonic_seconds();

                item.parse_ms = (end_s - begin_s) * 1000.0;
                item.ready_s = end_s;
                accumulator.parse_latency_ms.add(item.parse_ms);

                if (parsed) {
                    item.decoded = true;
                    item.sample = std::move(parsed).value();
                    ++accumulator.parsed;
                } else {
                    item.decoded = false;
                    item.reason = parsed.error().describe();
                    ++accumulator.rejected;
                }
                // Move (not copy) the raw line across the queue boundary.
                item.line = std::move(frame.line);

                if (!output_queue.push(std::move(item))) {
                    break;  // queue closed during shutdown
                }
            }
        });
    }

    // ------------------------------------------------------------- receiver
    std::thread receiver([&] {
        try {
            RawFrame frame;
            for (;;) {
                if (stop_requested_.load(std::memory_order_relaxed)) {
                    break;
                }
                if (!source.next(frame)) {
                    break;
                }
                // Arrival is stamped here rather than by the source, so it
                // measures the pipeline and not the generator's own clock.
                frame.arrival_s = monotonic_seconds();
                if (!raw_queue.push(frame)) {
                    break;
                }
                received.fetch_add(1, std::memory_order_relaxed);
            }
        } catch (...) {
            receiver_failed.store(true, std::memory_order_relaxed);
        }
        raw_queue.close();
    });

    // ------------------------------------------------------------ shutdown
    receiver.join();
    for (std::thread& worker : workers) {
        worker.join();
    }
    output_queue.close();
    logger.join();

    const double finished_s = monotonic_seconds();

    // -------------------------------------------------------- consolidation
    result.stats.workers = worker_count;
    result.stats.queue_capacity = config_.queue_capacity;
    result.stats.frames_received = received.load(std::memory_order_relaxed);
    for (const WorkerAccumulator& accumulator : accumulators) {
        result.stats.frames_parsed += accumulator.parsed;
        result.stats.frames_rejected += accumulator.rejected;
        result.stats.parse_latency_ms.merge(accumulator.parse_latency_ms);
    }
    result.stats.queue_high_water = raw_queue.high_water_mark();
    result.stats.output_high_water = output_queue.high_water_mark();
    result.stats.wall_ms = (finished_s - started_s) * 1000.0;
    result.stats.rss_start_bytes = rss_start;
    result.stats.rss_end_bytes = config_.rss_sample_every ? current_rss_bytes() : 0;
    result.stats.pitch = pitch_stats.snapshot();
    result.stats.roll = roll_stats.snapshot();
    if (result.stats.rss_peak_bytes < result.stats.rss_end_bytes) {
        result.stats.rss_peak_bytes = result.stats.rss_end_bytes;
    }

    if (result.stats.lost_frames() != 0) {
        std::ostringstream message;
        message << "pipeline lost " << result.stats.lost_frames() << " frame(s): received "
                << result.stats.frames_received << " but parsed+rejected "
                << (result.stats.frames_parsed + result.stats.frames_rejected);
        result.status = Error(ErrorCode::internal, message.str());
    } else if (receiver_failed.load(std::memory_order_relaxed)) {
        result.status = Error(ErrorCode::internal, "frame source threw while producing frames");
    } else if (out_of_order_dropped.load(std::memory_order_relaxed) != 0) {
        std::ostringstream message;
        message << out_of_order_dropped.load(std::memory_order_relaxed)
                << " frame(s) arrived after their slot had already been emitted";
        result.status = Error(ErrorCode::internal, message.str());
    }

    return result;
}

}  // namespace imu_lab
