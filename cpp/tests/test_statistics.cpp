// Tests for the Welford statistics accumulator.
#include <cmath>
#include <cstddef>
#include <vector>

#include "imu_lab/statistics.hpp"
#include "imu_lab/test_framework.hpp"

namespace {

using imu_lab::RunningStats;

/// Reference (two-pass) implementation to compare against.
struct Reference {
    double minimum = 0.0;
    double maximum = 0.0;
    double mean = 0.0;
    double population_stddev = 0.0;
};

Reference reference(const std::vector<double>& values) {
    Reference result;
    if (values.empty()) {
        return result;
    }
    result.minimum = values.front();
    result.maximum = values.front();
    double sum = 0.0;
    for (const double value : values) {
        result.minimum = std::fmin(result.minimum, value);
        result.maximum = std::fmax(result.maximum, value);
        sum += value;
    }
    result.mean = sum / static_cast<double>(values.size());

    double squared = 0.0;
    for (const double value : values) {
        const double delta = value - result.mean;
        squared += delta * delta;
    }
    result.population_stddev = std::sqrt(squared / static_cast<double>(values.size()));
    return result;
}

}  // namespace

IMU_TEST(Statistics, EmptyAccumulatorReportsZeros) {
    const RunningStats stats;
    CHECK_EQ(stats.count(), static_cast<std::size_t>(0));
    CHECK_NEAR(stats.mean(), 0.0, 0.0);
    CHECK_NEAR(stats.stddev(), 0.0, 0.0);
    CHECK_NEAR(stats.minimum(), 0.0, 0.0);
    CHECK_NEAR(stats.maximum(), 0.0, 0.0);
    CHECK_EQ(stats.snapshot().count, static_cast<std::size_t>(0));
}

IMU_TEST(Statistics, SingleSampleHasZeroStdDev) {
    RunningStats stats;
    stats.add(7.5);
    CHECK_EQ(stats.count(), static_cast<std::size_t>(1));
    CHECK_NEAR(stats.mean(), 7.5, 0.0);
    CHECK_NEAR(stats.minimum(), 7.5, 0.0);
    CHECK_NEAR(stats.maximum(), 7.5, 0.0);
    CHECK_NEAR(stats.stddev(), 0.0, 0.0);
}

IMU_TEST(Statistics, MatchesATwoPassReference) {
    const std::vector<double> values = {2.0, 4.0, 4.0, 4.0, 5.0, 5.0, 7.0, 9.0};
    const Reference expected = reference(values);

    RunningStats stats;
    for (const double value : values) {
        stats.add(value);
    }

    CHECK_EQ(stats.count(), values.size());
    CHECK_NEAR(stats.minimum(), expected.minimum, 1e-12);
    CHECK_NEAR(stats.maximum(), expected.maximum, 1e-12);
    CHECK_NEAR(stats.mean(), expected.mean, 1e-12);
    // The classic textbook answer for this series is stddev == 2.
    CHECK_NEAR(stats.stddev(), expected.population_stddev, 1e-12);
    CHECK_NEAR(stats.stddev(), 2.0, 1e-12);
}

IMU_TEST(Statistics, IsStableWithALargeOffset) {
    // A naive sum-of-squares accumulator loses all precision here; Welford does
    // not, which is the reason it was chosen for long acquisition runs.
    const std::vector<double> values = {1.0e9 + 4.0, 1.0e9 + 7.0, 1.0e9 + 13.0, 1.0e9 + 16.0};
    const Reference expected = reference(values);

    RunningStats stats;
    for (const double value : values) {
        stats.add(value);
    }
    CHECK_NEAR(stats.mean(), expected.mean, 1e-6);
    CHECK_NEAR(stats.stddev(), expected.population_stddev, 1e-6);
}

IMU_TEST(Statistics, HandlesConstantSeries) {
    RunningStats stats;
    for (int index = 0; index < 1000; ++index) {
        stats.add(-42.0);
    }
    CHECK_NEAR(stats.mean(), -42.0, 1e-12);
    CHECK_NEAR(stats.stddev(), 0.0, 1e-12);
    CHECK_NEAR(stats.variance(), 0.0, 1e-12);
}

IMU_TEST(Statistics, TracksNegativeValues) {
    RunningStats stats;
    stats.add(-5.0);
    stats.add(-10.0);
    stats.add(-1.0);
    CHECK_NEAR(stats.minimum(), -10.0, 0.0);
    CHECK_NEAR(stats.maximum(), -1.0, 0.0);
    CHECK_NEAR(stats.mean(), -5.333333333333333, 1e-12);
}

IMU_TEST(Statistics, SnapshotReflectsTheAccumulator) {
    RunningStats stats;
    stats.add(1.0);
    stats.add(3.0);
    const imu_lab::AngleStats snapshot = stats.snapshot();
    CHECK_EQ(snapshot.count, static_cast<std::size_t>(2));
    CHECK_NEAR(snapshot.minimum, 1.0, 0.0);
    CHECK_NEAR(snapshot.maximum, 3.0, 0.0);
    CHECK_NEAR(snapshot.mean, 2.0, 0.0);
    CHECK_NEAR(snapshot.stddev, 1.0, 0.0);
}

IMU_TEST(Statistics, ResetReturnsToTheEmptyState) {
    RunningStats stats;
    stats.add(5.0);
    stats.add(15.0);
    stats.reset();
    CHECK_EQ(stats.count(), static_cast<std::size_t>(0));
    CHECK_NEAR(stats.mean(), 0.0, 0.0);

    stats.add(1.0);
    CHECK_NEAR(stats.minimum(), 1.0, 0.0);
    CHECK_NEAR(stats.maximum(), 1.0, 0.0);
}
