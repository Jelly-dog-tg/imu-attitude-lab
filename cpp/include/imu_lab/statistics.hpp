// Numerically stable statistics for attitude channels.
#pragma once

#include <cstddef>
#include <limits>

namespace imu_lab {

/// Summary of one attitude channel, matching the Python report's fields.
struct AngleStats {
    std::size_t count = 0;
    double minimum = 0.0;
    double maximum = 0.0;
    double mean = 0.0;
    /// Population standard deviation (matches Python's `statistics.pstdev`).
    double stddev = 0.0;
};

/// Single-pass accumulator using Welford's algorithm.
///
/// Streaming the whole log through an O(1) accumulator matters here: the C++
/// core is expected to keep up with a live serial feed, and buffering every
/// sample just to compute a mean would defeat the point of the bounded queue.
class RunningStats {
public:
    RunningStats() = default;

    void add(double value) noexcept;

    [[nodiscard]] std::size_t count() const noexcept { return count_; }
    [[nodiscard]] double minimum() const noexcept { return count_ ? minimum_ : 0.0; }
    [[nodiscard]] double maximum() const noexcept { return count_ ? maximum_ : 0.0; }
    [[nodiscard]] double mean() const noexcept { return count_ ? mean_ : 0.0; }
    [[nodiscard]] double variance() const noexcept {
        return count_ > 1 ? m2_ / static_cast<double>(count_) : 0.0;
    }
    /// Population standard deviation; defined in the translation unit so that
    /// `<cmath>` does not leak into every includer.
    [[nodiscard]] double stddev() const noexcept;

    /// Snapshot suitable for reporting or serialisation.
    [[nodiscard]] AngleStats snapshot() const noexcept;

    void reset() noexcept;

private:
    std::size_t count_ = 0;
    double mean_ = 0.0;
    double m2_ = 0.0;
    double minimum_ = std::numeric_limits<double>::infinity();
    double maximum_ = -std::numeric_limits<double>::infinity();
};

}  // namespace imu_lab
