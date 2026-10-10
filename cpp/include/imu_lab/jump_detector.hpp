// Large-angle-step ("jump") detection.
//
// A jump is a review hint, not proof of a sensor fault: real motion can also
// exceed the threshold. The Python and C++ implementations must agree on which
// frames they flag, so the algorithm is intentionally the simplest possible —
// compare each sample with the immediately preceding one, per channel.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "imu_lab/attitude_sample.hpp"

namespace imu_lab {

/// One flagged transition between adjacent samples.
struct JumpEvent {
    /// Sequence number of the *later* sample in the pair.
    std::uint64_t sequence = 0;
    double elapsed_s = 0.0;
    double pitch_delta_deg = 0.0;
    double roll_delta_deg = 0.0;
    bool pitch_jump = false;
    bool roll_jump = false;
};

/// Stateful, single-pass jump detector.
///
/// Holding the previous sample is what makes this stateful; the pipeline keeps
/// one detector per logical stream and runs it on the (ordered) logger thread so
/// the result does not depend on how many worker threads are parsing frames.
class JumpDetector {
public:
    /// Default threshold in degrees, matching `--jump-threshold-deg` elsewhere.
    static constexpr double kDefaultThresholdDeg = 30.0;

    /// @throws std::invalid_argument when `threshold_deg` is not finite or <= 0.
    explicit JumpDetector(double threshold_deg = kDefaultThresholdDeg);

    [[nodiscard]] double threshold_deg() const noexcept { return threshold_deg_; }
    [[nodiscard]] std::size_t event_count() const noexcept { return event_count_; }
    [[nodiscard]] bool has_previous() const noexcept { return has_previous_; }

    /// Feed the next sample in order.
    ///
    /// @return the event when this sample closes a flagged transition, otherwise
    ///         `std::nullopt`. The first sample never produces an event.
    std::optional<JumpEvent> observe(std::uint64_t sequence, double elapsed_s,
                                     const AttitudeSample& sample) noexcept;

    /// Convenience overload for a sample without a sequence number.
    ///
    /// Successive calls are numbered 1, 2, 3, ... so the event for the *second*
    /// sample carries sequence 2.
    std::optional<JumpEvent> observe(double elapsed_s, const AttitudeSample& sample) noexcept;

    void reset() noexcept;

private:
    double threshold_deg_;
    bool has_previous_ = false;
    double previous_pitch_deg_ = 0.0;
    double previous_roll_deg_ = 0.0;
    std::uint64_t previous_sequence_ = 0;
    std::size_t event_count_ = 0;
};

}  // namespace imu_lab
