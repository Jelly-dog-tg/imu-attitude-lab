#include "imu_lab/protocol_parser.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <optional>
#include <system_error>

#include "imu_lab/number_parser.hpp"

namespace imu_lab {
namespace {

/// Convert one text field into a finite double.
///
/// The literal itself is decoded by `parse_double` (locale independent, no
/// allocation); this wrapper adds the field-level classification the caller
/// needs.
Status parse_number(std::string_view text, double& out, const std::string& field) {
    if (text.empty()) {
        return Error::in_field(ErrorCode::not_numeric, "field '" + field + "' is empty", field);
    }

    double value = 0.0;
    if (!parse_double(text, value)) {
        return Error::in_field(
            ErrorCode::not_numeric,
            "field '" + field + "' is not numeric: '" + std::string(text) + "'", field);
    }
    if (!std::isfinite(value)) {
        return Error::in_field(
            ErrorCode::not_finite,
            "field '" + field + "' must be a finite number, got '" + std::string(text) + "'", field);
    }
    out = value;
    return success();
}

Status parse_integer(std::string_view text, std::int64_t& out, const std::string& field) {
    if (text.empty()) {
        return Error::in_field(ErrorCode::not_numeric, "field '" + field + "' is empty", field);
    }
    if (text.front() == '+') {
        text.remove_prefix(1);
    }

    std::int64_t value = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const std::from_chars_result result = std::from_chars(first, last, value);

    if (result.ec != std::errc{} || result.ptr != last) {
        return Error::in_field(
            ErrorCode::not_numeric,
            "field '" + field + "' is not an integer: '" + std::string(text) + "'", field);
    }
    if (value < 0) {
        return Error::in_field(
            ErrorCode::negative_value, "field '" + field + "' must be non-negative", field);
    }
    out = value;
    return success();
}

/// Split on `,` and copy the `index`-th field, trimmed. Returns nullopt when the
/// record has fewer fields than requested.
std::optional<std::string_view> nth_field(std::string_view text, std::size_t index) {
    std::size_t start = 0;
    for (std::size_t current = 0;; ++current) {
        std::size_t end = text.find(',', start);
        const bool last = (end == std::string_view::npos);
        if (last) {
            end = text.size();
        }
        if (current == index) {
            return AttitudeLineParser::trim(text.substr(start, end - start));
        }
        if (last) {
            return std::nullopt;
        }
        start = end + 1;
    }
}

}  // namespace

const char* to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::ok: return "ok";
        case ErrorCode::empty_line: return "empty_line";
        case ErrorCode::missing_separator: return "missing_separator";
        case ErrorCode::unknown_field: return "unknown_field";
        case ErrorCode::duplicate_field: return "duplicate_field";
        case ErrorCode::not_numeric: return "not_numeric";
        case ErrorCode::not_finite: return "not_finite";
        case ErrorCode::missing_required_field: return "missing_required_field";
        case ErrorCode::incomplete_position_pair: return "incomplete_position_pair";
        case ErrorCode::negative_elapsed: return "negative_elapsed";
        case ErrorCode::negative_value: return "negative_value";
        case ErrorCode::wrong_field_count: return "wrong_field_count";
        case ErrorCode::invalid_prefix: return "invalid_prefix";
        case ErrorCode::file_not_found: return "file_not_found";
        case ErrorCode::file_not_writable: return "file_not_writable";
        case ErrorCode::empty_file: return "empty_file";
        case ErrorCode::missing_column: return "missing_column";
        case ErrorCode::duplicate_column: return "duplicate_column";
        case ErrorCode::unknown_column: return "unknown_column";
        case ErrorCode::not_a_number_in_column: return "not_a_number_in_column";
        case ErrorCode::unsupported_source: return "unsupported_source";
        case ErrorCode::queue_closed: return "queue_closed";
        case ErrorCode::internal: return "internal";
    }
    return "unknown";
}

std::string Error::describe() const {
    std::string text = "[";
    text += to_string(code);
    text += "] ";
    text += message;
    if (line != 0) {
        text += " (line ";
        text += std::to_string(line);
        text += ")";
    }
    return text;
}

std::string_view AttitudeLineParser::trim(std::string_view text) noexcept {
    constexpr std::string_view kWhitespace = " \t\r\n\f\v";
    const std::size_t first = text.find_first_not_of(kWhitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(kWhitespace);
    return text.substr(first, last - first + 1);
}

Result<AttitudeSample> AttitudeLineParser::parse_attitude_line(std::string_view line) {
    const std::string_view text = trim(line);
    if (text.empty()) {
        return Error(ErrorCode::empty_line, "empty line");
    }

    bool seen_pitch = false;
    bool seen_roll = false;
    bool seen_x = false;
    bool seen_y = false;
    double pitch = 0.0;
    double roll = 0.0;
    double x = 0.0;
    double y = 0.0;

    std::size_t start = 0;
    for (;;) {
        std::size_t end = text.find(',', start);
        const bool last = (end == std::string_view::npos);
        if (last) {
            end = text.size();
        }
        const std::string_view item = trim(text.substr(start, end - start));

        const std::size_t colon = item.find(':');
        if (colon == std::string_view::npos) {
            return Error(ErrorCode::missing_separator,
                         "each field must look like KEY:value, got '" + std::string(item) + "'");
        }
        const std::string_view key = trim(item.substr(0, colon));
        const std::string_view raw_value = trim(item.substr(colon + 1));

        double* target = nullptr;
        bool* seen = nullptr;
        if (key == "P") {
            target = &pitch;
            seen = &seen_pitch;
        } else if (key == "R") {
            target = &roll;
            seen = &seen_roll;
        } else if (key == "X") {
            target = &x;
            seen = &seen_x;
        } else if (key == "Y") {
            target = &y;
            seen = &seen_y;
        } else {
            return Error(ErrorCode::unknown_field, "unknown field: '" + std::string(key) + "'");
        }

        if (*seen) {
            return Error(ErrorCode::duplicate_field, "duplicate field: '" + std::string(key) + "'");
        }

        const std::string field_name(key);
        double value = 0.0;
        if (const Status parsed = parse_number(raw_value, value, field_name); !parsed) {
            return parsed.error();
        }
        *target = value;
        *seen = true;

        if (last) {
            break;
        }
        start = end + 1;
    }

    if (!seen_pitch || !seen_roll) {
        return Error(ErrorCode::missing_required_field, "required fields P and R are missing");
    }
    if (seen_x != seen_y) {
        return Error(ErrorCode::incomplete_position_pair,
                     "experimental position requires both X and Y, or neither");
    }

    AttitudeSample sample;
    sample.pitch_deg = pitch;
    sample.roll_deg = roll;
    if (seen_x) {
        sample.x = x;
        sample.y = y;
    }
    return sample;
}

Result<ImuRecord> AttitudeLineParser::parse_imu_record(std::string_view line) {
    const std::string_view text = trim(line);
    if (text.empty()) {
        return Error(ErrorCode::empty_line, "empty line");
    }

    const auto prefix = nth_field(text, 0);
    if (!prefix || *prefix != "IMU") {
        return Error(ErrorCode::invalid_prefix, "invalid record prefix: expected 'IMU'");
    }

    // Count the fields once so a truncated record reports a useful number.
    std::size_t count = 0;
    for (std::size_t index = 0; nth_field(text, index).has_value(); ++index) {
        count = index + 1;
    }
    if (count != kImuRecordFieldCount) {
        return Error(ErrorCode::wrong_field_count,
                     "invalid field count: expected " + std::to_string(kImuRecordFieldCount) +
                         ", got " + std::to_string(count));
    }

    static constexpr std::array<std::string_view, 12> kNumericFields = {
        "ax_g", "ay_g", "az_g", "gx_dps", "gy_dps", "gz_dps",
        "mx_uT", "my_uT", "mz_uT", "roll_deg", "pitch_deg", "yaw_deg",
    };

    ImuRecord record;
    if (const Status parsed = parse_integer(*nth_field(text, 1), record.sequence, "seq"); !parsed) {
        return parsed.error();
    }
    if (const Status parsed = parse_integer(*nth_field(text, 2), record.t_ms, "t_ms"); !parsed) {
        return parsed.error();
    }

    std::array<double, 12> values{};
    for (std::size_t index = 0; index < values.size(); ++index) {
        const auto field = nth_field(text, index + 3);
        if (!field) {
            return Error(ErrorCode::wrong_field_count, "record ended before field " +
                                                           std::string(kNumericFields[index]));
        }
        if (const Status parsed = parse_number(*field, values[index], std::string(kNumericFields[index]));
            !parsed) {
            return parsed.error();
        }
    }

    record.ax_g = values[0];
    record.ay_g = values[1];
    record.az_g = values[2];
    record.gx_dps = values[3];
    record.gy_dps = values[4];
    record.gz_dps = values[5];
    record.mx_uT = values[6];
    record.my_uT = values[7];
    record.mz_uT = values[8];
    record.roll_deg = values[9];
    record.pitch_deg = values[10];
    record.yaw_deg = values[11];
    return record;
}

bool AttitudeLineParser::looks_like_imu_record(std::string_view line) noexcept {
    const std::string_view text = trim(line);
    return text.size() >= 4 && text[0] == 'I' && text[1] == 'M' && text[2] == 'U' && text[3] == ',';
}

std::string format_attitude_line(const AttitudeSample& sample) {
    // Sized for the worst case a finite double can produce: two values of
    // `-1.797...e308` with 6 decimals need roughly 320 characters each.
    constexpr std::size_t kBufferSize = 768;
    char buffer[kBufferSize] = {};

#if defined(__GNUC__) && !defined(__clang__)
    // GCC 13 cannot bound `%.6f` of a runtime double and would otherwise reject
    // this under -Werror. The size above is derived from the worst case and the
    // result is clamped below, so the warning is a false positive here.
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
    const int written =
        (sample.x.has_value() && sample.y.has_value())
            ? std::snprintf(buffer, sizeof(buffer), "P:%.6f,R:%.6f,X:%.6f,Y:%.6f",
                            sample.pitch_deg, sample.roll_deg, *sample.x, *sample.y)
            : std::snprintf(buffer, sizeof(buffer), "P:%.6f,R:%.6f", sample.pitch_deg,
                            sample.roll_deg);
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic pop
#endif

    if (written <= 0) {
        return {};
    }
    const auto length = static_cast<std::size_t>(written) < kBufferSize
                            ? static_cast<std::size_t>(written)
                            : kBufferSize - 1;
    return std::string(buffer, length);
}

}  // namespace imu_lab
