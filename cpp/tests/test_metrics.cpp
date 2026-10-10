// Tests for latency aggregation and the derived stability metrics.
#include <cstddef>
#include <string>

#include "imu_lab/metrics.hpp"
#include "imu_lab/test_framework.hpp"

namespace {

using imu_lab::LatencyStats;
using imu_lab::PipelineStats;

}  // namespace

IMU_TEST(Metrics, LatencyStatsTracksMinMeanMax) {
    LatencyStats stats;
    CHECK_EQ(stats.count(), static_cast<std::uint64_t>(0));
    CHECK_NEAR(stats.mean_ms(), 0.0, 0.0);

    stats.add(1.0);
    stats.add(5.0);
    stats.add(3.0);
    CHECK_EQ(stats.count(), static_cast<std::uint64_t>(3));
    CHECK_NEAR(stats.min_ms(), 1.0, 0.0);
    CHECK_NEAR(stats.max_ms(), 5.0, 0.0);
    CHECK_NEAR(stats.mean_ms(), 3.0, 0.0);
    CHECK_NEAR(stats.total_ms(), 9.0, 0.0);
}

IMU_TEST(Metrics, LatencyStatsSingleSample) {
    LatencyStats stats;
    stats.add(2.5);
    CHECK_EQ(stats.count(), static_cast<std::uint64_t>(1));
    CHECK_NEAR(stats.min_ms(), 2.5, 0.0);
    CHECK_NEAR(stats.max_ms(), 2.5, 0.0);
    CHECK_NEAR(stats.mean_ms(), 2.5, 0.0);
}

IMU_TEST(Metrics, LatencyStatsMergeCombinesSeries) {
    LatencyStats left;
    left.add(1.0);
    left.add(2.0);

    LatencyStats right;
    right.add(10.0);

    left.merge(right);
    CHECK_EQ(left.count(), static_cast<std::uint64_t>(3));
    CHECK_NEAR(left.min_ms(), 1.0, 0.0);
    CHECK_NEAR(left.max_ms(), 10.0, 0.0);
    CHECK_NEAR(left.mean_ms(), 13.0 / 3.0, 1e-12);
}

IMU_TEST(Metrics, LatencyStatsMergeIntoSomethingEmptyAdoptsTheOther) {
    LatencyStats empty;
    LatencyStats other;
    other.add(4.0);
    other.add(8.0);
    empty.merge(other);
    CHECK_EQ(empty.count(), static_cast<std::uint64_t>(2));
    CHECK_NEAR(empty.min_ms(), 4.0, 0.0);
    CHECK_NEAR(empty.max_ms(), 8.0, 0.0);
}

IMU_TEST(Metrics, LatencyStatsMergingNothingChangesNothing) {
    LatencyStats stats;
    stats.add(3.0);
    stats.merge(LatencyStats{});
    CHECK_EQ(stats.count(), static_cast<std::uint64_t>(1));
    CHECK_NEAR(stats.mean_ms(), 3.0, 0.0);
}

IMU_TEST(Metrics, LatencyStatsResetClears) {
    LatencyStats stats;
    stats.add(1.0);
    stats.reset();
    CHECK_EQ(stats.count(), static_cast<std::uint64_t>(0));
    CHECK_NEAR(stats.max_ms(), 0.0, 0.0);
}

IMU_TEST(Metrics, AnomalyRatioIsRejectedOverReceived) {
    PipelineStats stats;
    CHECK_NEAR(stats.anomaly_ratio(), 0.0, 0.0);  // no division by zero

    stats.frames_received = 1000;
    stats.frames_rejected = 25;
    CHECK_NEAR(stats.anomaly_ratio(), 0.025, 1e-12);
}

IMU_TEST(Metrics, AnomalyRatioCanBeComplete) {
    PipelineStats stats;
    stats.frames_received = 10;
    stats.frames_rejected = 10;
    CHECK_NEAR(stats.anomaly_ratio(), 1.0, 1e-12);
}

IMU_TEST(Metrics, LostFramesCountsUnaccountedFrames) {
    PipelineStats stats;
    stats.frames_received = 100;
    stats.frames_parsed = 90;
    stats.frames_rejected = 10;
    CHECK_EQ(stats.lost_frames(), static_cast<std::uint64_t>(0));

    stats.frames_parsed = 80;
    CHECK_EQ(stats.lost_frames(), static_cast<std::uint64_t>(10));

    // Never underflows if the counters are inconsistent.
    stats.frames_received = 1;
    CHECK_EQ(stats.lost_frames(), static_cast<std::uint64_t>(0));
}

IMU_TEST(Metrics, ThroughputUsesWallTime) {
    PipelineStats stats;
    stats.frames_received = 2000;
    stats.wall_ms = 1000.0;
    CHECK_NEAR(stats.throughput_fps(), 2000.0, 1e-9);

    stats.wall_ms = 0.0;  // must not divide by zero
    CHECK_NEAR(stats.throughput_fps(), 0.0, 0.0);
}

IMU_TEST(Metrics, BackpressureRatioHandlesUnboundedQueues) {
    PipelineStats stats;
    stats.queue_capacity = 0;  // unbounded
    stats.queue_high_water = 500;
    CHECK_NEAR(stats.backpressure_ratio(), 0.0, 0.0);

    stats.queue_capacity = 1000;
    stats.queue_high_water = 250;
    CHECK_NEAR(stats.backpressure_ratio(), 0.25, 1e-12);
}

IMU_TEST(Metrics, RenderMentionsEverySection) {
    PipelineStats stats;
    stats.workers = 3;
    stats.queue_capacity = 128;
    stats.frames_received = 500;
    stats.frames_parsed = 480;
    stats.frames_rejected = 20;
    stats.frames_logged = 500;
    stats.jump_count = 2;
    stats.parse_latency_ms.add(0.05);

    const std::string text = stats.render();
    for (const char* needle : {"Pipeline metrics", "frames received", "anomaly ratio",
                               "queue high-water mark", "parse latency", "throughput",
                               "RSS start", "large steps"}) {
        CHECK(text.find(needle) != std::string::npos);
    }
    CHECK(text.find("unbounded") == std::string::npos);
}

IMU_TEST(Metrics, RenderSaysUnboundedWhenCapacityIsZero) {
    PipelineStats stats;
    CHECK(stats.render().find("unbounded") != std::string::npos);
}

IMU_TEST(Metrics, JsonContainsTheKeyFields) {
    PipelineStats stats;
    stats.workers = 2;
    stats.frames_received = 100;
    stats.frames_parsed = 98;
    stats.frames_rejected = 2;
    stats.wall_ms = 50.0;
    stats.pitch = imu_lab::AngleStats{10, -1.0, 1.0, 0.0, 0.5};
    stats.roll = imu_lab::AngleStats{10, -2.0, 2.0, 0.0, 1.0};

    const std::string json = stats.to_json();
    for (const char* needle : {"\"workers\": 2", "\"frames_received\": 100",
                               "\"frames_rejected\": 2", "\"anomaly_ratio\"",
                               "\"queue_high_water\"", "\"throughput_fps\"",
                               "\"rss_peak_bytes\"", "\"pitch\"", "\"roll\"",
                               "\"jump_count\""}) {
        CHECK(json.find(needle) != std::string::npos);
    }
}

IMU_TEST(Metrics, ResidentSetSizeIsReportedOnSupportedPlatforms) {
    // On Windows (GetProcessMemoryInfo) and Linux (/proc/self/statm) this must be
    // a positive number. A zero result is only acceptable on an unsupported
    // platform, so the assertion is deliberately soft about the value and strict
    // about not crashing.
    const std::size_t rss = imu_lab::current_rss_bytes();
#if defined(_WIN32) || defined(__linux__) || defined(__unix__)
    CHECK(rss > static_cast<std::size_t>(0));
#else
    (void)rss;
#endif
}
