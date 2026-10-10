// Tests for the large-angle-step detector, including exact-threshold boundaries.
#include <cmath>
#include <limits>
#include <stdexcept>

#include "imu_lab/jump_detector.hpp"
#include "imu_lab/test_framework.hpp"

namespace {

using imu_lab::AttitudeSample;
using imu_lab::JumpDetector;

AttitudeSample sample_with(double pitch, double roll) {
    return AttitudeSample{pitch, roll, std::nullopt, std::nullopt};
}

}  // namespace

IMU_TEST(JumpDetector, RejectsInvalidThresholds) {
    CHECK_THROWS_AS(JumpDetector(0.0), std::invalid_argument);
    CHECK_THROWS_AS(JumpDetector(-1.0), std::invalid_argument);
    CHECK_THROWS_AS(JumpDetector(std::nan("")), std::invalid_argument);
    CHECK_THROWS_AS(JumpDetector(std::numeric_limits<double>::infinity()), std::invalid_argument);
}

IMU_TEST(JumpDetector, DefaultThresholdIsThirtyDegrees) {
    const JumpDetector detector;
    CHECK_NEAR(detector.threshold_deg(), 30.0, 0.0);
    CHECK_EQ(detector.event_count(), static_cast<std::size_t>(0));
    CHECK(!detector.has_previous());
}

IMU_TEST(JumpDetector, FirstSampleNeverProducesAnEvent) {
    JumpDetector detector(30.0);
    const auto event = detector.observe(0, 0.0, sample_with(100.0, -100.0));
    CHECK(!event.has_value());
    CHECK(detector.has_previous());
    CHECK_EQ(detector.event_count(), static_cast<std::size_t>(0));
}

IMU_TEST(JumpDetector, FlagsAPitchJump) {
    JumpDetector detector(30.0);
    (void)detector.observe(0, 0.0, sample_with(0.0, 0.0));
    const auto event = detector.observe(1, 0.05, sample_with(45.0, 1.0));

    CHECK(event.has_value());
    CHECK(event->pitch_jump);
    CHECK(!event->roll_jump);
    CHECK_EQ(event->sequence, static_cast<std::uint64_t>(1));
    CHECK_NEAR(event->elapsed_s, 0.05, 1e-12);
    CHECK_NEAR(event->pitch_delta_deg, 45.0, 1e-12);
    CHECK_NEAR(event->roll_delta_deg, 1.0, 1e-12);
    CHECK_EQ(detector.event_count(), static_cast<std::size_t>(1));
}

IMU_TEST(JumpDetector, FlagsARollJumpOnTheCorrectChannel) {
    JumpDetector detector(30.0);
    (void)detector.observe(0, 0.0, sample_with(0.0, 0.0));
    const auto event = detector.observe(1, 0.05, sample_with(0.5, -31.5));

    CHECK(event.has_value());
    CHECK(!event->pitch_jump);
    CHECK(event->roll_jump);
}

IMU_TEST(JumpDetector, FlagsBothChannelsWhenBothMove) {
    JumpDetector detector(10.0);
    (void)detector.observe(0, 0.0, sample_with(0.0, 0.0));
    const auto event = detector.observe(1, 0.05, sample_with(50.0, -50.0));

    CHECK(event.has_value());
    CHECK(event->pitch_jump);
    CHECK(event->roll_jump);
}

IMU_TEST(JumpDetector, ThresholdComparisonIsStrictlyGreaterThan) {
    // Exactly at the threshold must NOT be flagged: otherwise the reported
    // "large steps" count would depend on floating-point luck at the boundary.
    JumpDetector detector(30.0);
    (void)detector.observe(0, 0.0, sample_with(0.0, 0.0));

    CHECK(!detector.observe(1, 0.01, sample_with(30.0, 0.0)).has_value());
    CHECK(!detector.observe(2, 0.02, sample_with(0.0, -30.0)).has_value());
    CHECK_EQ(detector.event_count(), static_cast<std::size_t>(0));
}

IMU_TEST(JumpDetector, JustAboveTheThresholdIsFlagged) {
    JumpDetector detector(30.0);
    (void)detector.observe(0, 0.0, sample_with(0.0, 0.0));
    const auto event = detector.observe(1, 0.01, sample_with(30.001, 0.0));
    CHECK(event.has_value());
    CHECK(event->pitch_jump);
}

IMU_TEST(JumpDetector, UsesAbsoluteMagnitude) {
    JumpDetector detector(30.0);
    (void)detector.observe(0, 0.0, sample_with(100.0, 0.0));
    const auto event = detector.observe(1, 0.01, sample_with(60.0, 0.0));
    CHECK(event.has_value());
    CHECK_NEAR(event->pitch_delta_deg, -40.0, 1e-12);
}

IMU_TEST(JumpDetector, ConsecutiveJumpsAreEachReported) {
    JumpDetector detector(10.0);
    (void)detector.observe(0, 0.0, sample_with(0.0, 0.0));
    CHECK(detector.observe(1, 0.01, sample_with(20.0, 0.0)).has_value());
    CHECK(detector.observe(2, 0.02, sample_with(0.0, 0.0)).has_value());
    CHECK_EQ(detector.event_count(), static_cast<std::size_t>(2));
}

IMU_TEST(JumpDetector, ResetClearsAllState) {
    JumpDetector detector(10.0);
    (void)detector.observe(0, 0.0, sample_with(0.0, 0.0));
    (void)detector.observe(1, 0.01, sample_with(100.0, 0.0));
    CHECK_EQ(detector.event_count(), static_cast<std::size_t>(1));
    CHECK(detector.has_previous());

    detector.reset();
    CHECK_EQ(detector.event_count(), static_cast<std::size_t>(0));
    CHECK(!detector.has_previous());
    CHECK(!detector.observe(0, 0.0, sample_with(500.0, 500.0)).has_value());
}

IMU_TEST(JumpDetector, SequenceOnlyOverloadAutoIncrements) {
    JumpDetector detector(10.0);
    // Successive samples are numbered 1, 2, 3, ... so the event triggered by the
    // second sample carries sequence 2.
    (void)detector.observe(0.0, sample_with(0.0, 0.0));
    const auto first_event = detector.observe(0.01, sample_with(50.0, 0.0));
    CHECK(first_event.has_value());
    CHECK_EQ(first_event->sequence, static_cast<std::uint64_t>(2));

    const auto second_event = detector.observe(0.02, sample_with(0.0, 0.0));
    CHECK(second_event.has_value());
    CHECK_EQ(second_event->sequence, static_cast<std::uint64_t>(3));
}

IMU_TEST(JumpDetector, StableSignalProducesNoEvents) {
    JumpDetector detector(5.0);
    for (int index = 0; index < 500; ++index) {
        const double pitch = 0.001 * static_cast<double>(index % 5);
        CHECK(!detector.observe(static_cast<std::uint64_t>(index), index * 0.01,
                                sample_with(pitch, -pitch))
                   .has_value());
    }
    CHECK_EQ(detector.event_count(), static_cast<std::size_t>(0));
}
