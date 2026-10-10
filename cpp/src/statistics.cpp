#include "imu_lab/statistics.hpp"

#include <algorithm>
#include <cmath>

namespace imu_lab {

void RunningStats::add(double value) noexcept {
    ++count_;
    minimum_ = std::min(minimum_, value);
    maximum_ = std::max(maximum_, value);

    // Welford update: keeps the running mean and the sum of squared
    // deviations correct without a second pass over the data.
    const double delta = value - mean_;
    mean_ += delta / static_cast<double>(count_);
    const double delta2 = value - mean_;
    m2_ += delta * delta2;
}

double RunningStats::stddev() const noexcept {
    return std::sqrt(variance());
}

AngleStats RunningStats::snapshot() const noexcept {
    AngleStats stats;
    stats.count = count_;
    stats.minimum = minimum();
    stats.maximum = maximum();
    stats.mean = mean();
    stats.stddev = stddev();
    return stats;
}

void RunningStats::reset() noexcept {
    count_ = 0;
    mean_ = 0.0;
    m2_ = 0.0;
    minimum_ = std::numeric_limits<double>::infinity();
    maximum_ = -std::numeric_limits<double>::infinity();
}

}  // namespace imu_lab
