// Parser for the line formats emitted by the IMU Attitude Lab firmware.
//
// Two formats are understood, both deliberately documented because the
// firmware is expected to grow:
//
//   * Current firmware (hand-written STM32 loop):
//         P:<pitch_deg>,R:<roll_deg>
//         P:<pitch_deg>,R:<roll_deg>,X:<x>,Y:<y>     (experimental position)
//
//   * Full wire protocol used by the simulator and the Python host:
//         IMU,<seq>,<t_ms>,<ax>,<ay>,<az>,<gx>,<gy>,<gz>,<mx>,<my>,<mz>,<roll>,<pitch>,<yaw>
//
// Parsing is deliberately strict and never allocates on the happy path beyond
// the returned value, because this runs once per received frame.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "imu_lab/attitude_sample.hpp"
#include "imu_lab/error.hpp"

namespace imu_lab {

/// Parsed contents of a full `IMU,...` wire-protocol record.
struct ImuRecord {
    std::int64_t sequence = 0;
    std::int64_t t_ms = 0;
    double ax_g = 0.0;
    double ay_g = 0.0;
    double az_g = 0.0;
    double gx_dps = 0.0;
    double gy_dps = 0.0;
    double gz_dps = 0.0;
    double mx_uT = 0.0;
    double my_uT = 0.0;
    double mz_uT = 0.0;
    double roll_deg = 0.0;
    double pitch_deg = 0.0;
    double yaw_deg = 0.0;
};

/// Number of comma-separated fields in an `IMU,...` record (including prefix).
inline constexpr std::size_t kImuRecordFieldCount = 15;

/// Stateless parser for the firmware's `P:`/`R:` line format.
///
/// Free functions rather than a class: the parser holds no state, and keeping
/// it stateless means worker threads can share one instance without locking.
class AttitudeLineParser {
public:
    /// Trim ASCII whitespace (including a trailing CR from Windows line ends).
    static std::string_view trim(std::string_view text) noexcept;

    /// Parse `P:<deg>,R:<deg>[,X:<v>,Y:<v>]`.
    ///
    /// Returns an `Error` with `line` left at 0; the caller fills in the source
    /// line number, which keeps this function reusable for single lines typed
    /// on the command line.
    static Result<AttitudeSample> parse_attitude_line(std::string_view line);

    /// Parse the full `IMU,...` record.
    static Result<ImuRecord> parse_imu_record(std::string_view line);

    /// True when the line looks like an `IMU,` record; cheap prefix test used
    /// by auto-detecting callers.
    static bool looks_like_imu_record(std::string_view line) noexcept;
};

/// Format a sample back into the firmware's line format (round-trip helper).
[[nodiscard]] std::string format_attitude_line(const AttitudeSample& sample);

}  // namespace imu_lab
