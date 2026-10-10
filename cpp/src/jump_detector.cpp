#include "imu_lab/jump_detector.hpp"

#include <cmath>
#include <stdexcept>

namespace imu_lab {

JumpDetector::JumpDetector(double threshold_deg) : threshold_deg_(threshold_deg) {
    if (!std::isfinite(threshold_deg) || threshold_deg <= 0.0) {
        throw std::invalid_argument(
            "JumpDetector: threshold_deg must be a finite number greater than zero");
    }
}

std::optional<JumpEvent> JumpDetector::observe(std::uint64_t sequence, double elapsed_s,
                                               const AttitudeSample& sample) noexcept {
    if (!has_previous_) {
        has_previous_ = true;
        previous_pitch_deg_ = sample.pitch_deg;
        previous_roll_deg_ = sample.roll_deg;
        previous_sequence_ = sequence;
        return std::nullopt;
    }

    const double pitch_delta = sample.pitch_deg - previous_pitch_deg_;
    const double roll_delta = sample.roll_deg - previous_roll_deg_;
    const bool pitch_jump = std::fabs(pitch_delta) > threshold_deg_;
    const bool roll_jump = std::fabs(roll_delta) > threshold_deg_;

    previous_pitch_deg_ = sample.pitch_deg;
    previous_roll_deg_ = sample.roll_deg;
    previous_sequence_ = sequence;

    if (!pitch_jump && !roll_jump) {
        return std::nullopt;
    }

    ++event_count_;
    JumpEvent event;
    event.sequence = sequence;
    event.elapsed_s = elapsed_s;
    event.pitch_delta_deg = pitch_delta;
    event.roll_delta_deg = roll_delta;
    event.pitch_jump = pitch_jump;
    event.roll_jump = roll_jump;
    return event;
}

std::optional<JumpEvent> JumpDetector::observe(double elapsed_s,
                                               const AttitudeSample& sample) noexcept {
    return observe(previous_sequence_ + 1, elapsed_s, sample);
}

void JumpDetector::reset() noexcept {
    has_previous_ = false;
    previous_pitch_deg_ = 0.0;
    previous_roll_deg_ = 0.0;
    previous_sequence_ = 0;
    event_count_ = 0;
}

}  // namespace imu_lab
