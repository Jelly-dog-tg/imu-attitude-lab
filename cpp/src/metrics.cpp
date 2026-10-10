#include "imu_lab/metrics.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <psapi.h>
#elif defined(__linux__) || defined(__unix__)
#  include <unistd.h>
#  include <cstdio>
#endif

namespace imu_lab {
namespace {

std::string format_double(double value, int precision = 3) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

/// RSS is a size_t but JSON numbers must stay integral; format explicitly.
std::string format_bytes(std::size_t bytes) { return std::to_string(bytes); }

}  // namespace

std::size_t current_rss_bytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) != 0) {
        return static_cast<std::size_t>(counters.WorkingSetSize);
    }
    return 0;
#elif defined(__linux__) || defined(__unix__)
    // statm reports resident pages in the second field.
    std::FILE* file = std::fopen("/proc/self/statm", "r");
    if (file == nullptr) {
        return 0;
    }
    long total_pages = 0;
    long resident_pages = 0;
    const int matched = std::fscanf(file, "%ld %ld", &total_pages, &resident_pages);
    std::fclose(file);
    if (matched != 2 || resident_pages < 0) {
        return 0;
    }
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        return 0;
    }
    return static_cast<std::size_t>(resident_pages) * static_cast<std::size_t>(page_size);
#else
    return 0;
#endif
}

void LatencyStats::add(double milliseconds) noexcept {
    if (count_ == 0) {
        min_ms_ = milliseconds;
        max_ms_ = milliseconds;
    } else {
        min_ms_ = std::min(min_ms_, milliseconds);
        max_ms_ = std::max(max_ms_, milliseconds);
    }
    total_ms_ += milliseconds;
    ++count_;
}

void LatencyStats::merge(const LatencyStats& other) noexcept {
    if (other.count_ == 0) {
        return;
    }
    if (count_ == 0) {
        min_ms_ = other.min_ms_;
        max_ms_ = other.max_ms_;
    } else {
        min_ms_ = std::min(min_ms_, other.min_ms_);
        max_ms_ = std::max(max_ms_, other.max_ms_);
    }
    total_ms_ += other.total_ms_;
    count_ += other.count_;
}

void LatencyStats::reset() noexcept {
    count_ = 0;
    total_ms_ = 0.0;
    min_ms_ = 0.0;
    max_ms_ = 0.0;
}

double PipelineStats::anomaly_ratio() const noexcept {
    return frames_received == 0
               ? 0.0
               : static_cast<double>(frames_rejected) / static_cast<double>(frames_received);
}

double PipelineStats::throughput_fps() const noexcept {
    return wall_ms <= 0.0 ? 0.0 : static_cast<double>(frames_received) * 1000.0 / wall_ms;
}

double PipelineStats::backpressure_ratio() const noexcept {
    return queue_capacity == 0
               ? 0.0
               : static_cast<double>(queue_high_water) / static_cast<double>(queue_capacity);
}

std::uint64_t PipelineStats::lost_frames() const noexcept {
    // Integrity check: every received frame must be either parsed or rejected.
    // Anything else means a frame was dropped between the queues, which is the
    // one thing a data-acquisition pipeline must never do silently.
    const std::uint64_t accounted = frames_parsed + frames_rejected;
    return frames_received > accounted ? frames_received - accounted : 0;
}

std::string PipelineStats::render() const {
    std::ostringstream out;
    out << "Pipeline metrics\n";
    out << "  workers                 : " << workers << "\n";
    out << "  queue capacity          : "
        << (queue_capacity == 0 ? std::string("unbounded") : std::to_string(queue_capacity)) << "\n";
    out << "  frames received         : " << frames_received << "\n";
    out << "  frames parsed           : " << frames_parsed << "\n";
    out << "  frames rejected         : " << frames_rejected << "\n";
    out << "  frames logged           : " << frames_logged << "\n";
    out << "  lost frames             : " << lost_frames() << "\n";
    out << "  anomaly ratio           : " << format_double(anomaly_ratio() * 100.0, 4) << " %\n";
    out << "  queue high-water mark   : " << queue_high_water;
    if (queue_capacity != 0) {
        out << " / " << queue_capacity << " (" << format_double(backpressure_ratio() * 100.0, 2)
            << " %)";
    }
    out << "\n";
    out << "  output high-water mark  : " << output_high_water << "\n";
    out << "  parse latency (ms)      : mean " << format_double(parse_latency_ms.mean_ms(), 5)
        << ", max " << format_double(parse_latency_ms.max_ms(), 5) << "\n";
    out << "  end-to-end latency (ms) : mean "
        << format_double(end_to_end_latency_ms.mean_ms(), 5) << ", max "
        << format_double(end_to_end_latency_ms.max_ms(), 5) << "\n";
    out << "  wall time (ms)          : " << format_double(wall_ms, 3) << "\n";
    out << "  throughput (frames/s)   : " << format_double(throughput_fps(), 1) << "\n";
    out << "  RSS start / end / peak  : " << format_bytes(rss_start_bytes) << " / "
        << format_bytes(rss_end_bytes) << " / " << format_bytes(rss_peak_bytes) << " bytes\n";
    out << "  pitch (deg)             : min " << format_double(pitch.minimum) << ", max "
        << format_double(pitch.maximum) << ", mean " << format_double(pitch.mean) << ", std "
        << format_double(pitch.stddev) << "\n";
    out << "  roll (deg)              : min " << format_double(roll.minimum) << ", max "
        << format_double(roll.maximum) << ", mean " << format_double(roll.mean) << ", std "
        << format_double(roll.stddev) << "\n";
    out << "  large steps             : " << jump_count << "\n";
    return out.str();
}

std::string PipelineStats::to_json() const {
    std::ostringstream out;
    out << "{\n";
    out << "  \"workers\": " << workers << ",\n";
    out << "  \"queue_capacity\": " << queue_capacity << ",\n";
    out << "  \"frames_received\": " << frames_received << ",\n";
    out << "  \"frames_parsed\": " << frames_parsed << ",\n";
    out << "  \"frames_rejected\": " << frames_rejected << ",\n";
    out << "  \"frames_logged\": " << frames_logged << ",\n";
    out << "  \"lost_frames\": " << lost_frames() << ",\n";
    out << "  \"anomaly_ratio\": " << format_double(anomaly_ratio(), 6) << ",\n";
    out << "  \"queue_high_water\": " << queue_high_water << ",\n";
    out << "  \"output_high_water\": " << output_high_water << ",\n";
    out << "  \"backpressure_ratio\": " << format_double(backpressure_ratio(), 6) << ",\n";
    out << "  \"parse_latency_ms\": {\"mean\": " << format_double(parse_latency_ms.mean_ms(), 5)
        << ", \"max\": " << format_double(parse_latency_ms.max_ms(), 5) << "},\n";
    out << "  \"end_to_end_latency_ms\": {\"mean\": "
        << format_double(end_to_end_latency_ms.mean_ms(), 5) << ", \"max\": "
        << format_double(end_to_end_latency_ms.max_ms(), 5) << "},\n";
    out << "  \"wall_ms\": " << format_double(wall_ms, 3) << ",\n";
    out << "  \"throughput_fps\": " << format_double(throughput_fps(), 3) << ",\n";
    out << "  \"rss_start_bytes\": " << rss_start_bytes << ",\n";
    out << "  \"rss_end_bytes\": " << rss_end_bytes << ",\n";
    out << "  \"rss_peak_bytes\": " << rss_peak_bytes << ",\n";
    out << "  \"pitch\": {\"min\": " << format_double(pitch.minimum, 6) << ", \"max\": "
        << format_double(pitch.maximum, 6) << ", \"mean\": " << format_double(pitch.mean, 6)
        << ", \"stddev\": " << format_double(pitch.stddev, 6) << "},\n";
    out << "  \"roll\": {\"min\": " << format_double(roll.minimum, 6) << ", \"max\": "
        << format_double(roll.maximum, 6) << ", \"mean\": " << format_double(roll.mean, 6)
        << ", \"stddev\": " << format_double(roll.stddev, 6) << "},\n";
    out << "  \"jump_count\": " << jump_count << "\n";
    out << "}\n";
    return out.str();
}

}  // namespace imu_lab
