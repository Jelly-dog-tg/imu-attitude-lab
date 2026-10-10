// Core value types shared by every stage of the C++ processing pipeline.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace imu_lab {

/// One decoded attitude sample.
///
/// `pitch_deg`/`roll_deg` are the fields the existing STM32 firmware actually
/// emits. `x`/`y` are the optional experimental position fields; they are kept
/// optional (rather than defaulted to 0.0) so that "absent" and "zero" stay
/// distinguishable all the way through the pipeline.
struct AttitudeSample {
    double pitch_deg = 0.0;
    double roll_deg = 0.0;
    std::optional<double> x;
    std::optional<double> y;

    friend bool operator==(const AttitudeSample& lhs, const AttitudeSample& rhs) {
        return lhs.pitch_deg == rhs.pitch_deg && lhs.roll_deg == rhs.roll_deg &&
               lhs.x == rhs.x && lhs.y == rhs.y;
    }
    friend bool operator!=(const AttitudeSample& lhs, const AttitudeSample& rhs) {
        return !(lhs == rhs);
    }
};

/// A raw protocol line plus the two timestamps the pipeline needs.
///
/// Keeping both timestamps is deliberate:
///   * `elapsed_s` is the *logical* sample time and becomes the CSV's
///     `elapsed_s` column and the x-axis of the report;
///   * `arrival_s` is the *monotonic wall clock* when the receiver accepted the
///     frame, which is what makes end-to-end latency (arrival -> logged)
///     meaningful even though a replayed file is years "old".
struct RawFrame {
    std::uint64_t sequence = 0;
    std::string line;
    /// Logical sample time in seconds, as reported by the source.
    double elapsed_s = 0.0;
    /// Monotonic seconds (since an arbitrary epoch) when the receiver got it.
    double arrival_s = 0.0;
};

/// The result of parsing one `RawFrame`.
struct ParsedFrame {
    std::uint64_t sequence = 0;
    double elapsed_s = 0.0;
    double arrival_s = 0.0;
    AttitudeSample sample;
    /// Wall-clock milliseconds spent inside the parser for this frame.
    double parse_ms = 0.0;
};

}  // namespace imu_lab
