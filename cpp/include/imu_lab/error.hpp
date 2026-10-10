// Error handling primitives for the IMU Attitude Lab C++ core.
//
// The parser rejects a lot of input (serial noise, truncated frames, NaN from
// a floating sensor) on the hot path, so errors are *values* rather than
// exceptions: no allocation, no unwinding, and every failure carries a stable
// machine-readable code so callers and tests can branch on it.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace imu_lab {

/// Stable classification of every failure the core can produce.
enum class ErrorCode {
    ok = 0,
    empty_line,
    missing_separator,
    unknown_field,
    duplicate_field,
    not_numeric,
    not_finite,
    missing_required_field,
    incomplete_position_pair,
    negative_elapsed,
    negative_value,
    wrong_field_count,
    invalid_prefix,
    file_not_found,
    file_not_writable,
    empty_file,
    missing_column,
    duplicate_column,
    unknown_column,
    not_a_number_in_column,
    unsupported_source,
    queue_closed,
    internal,
};

/// Human-readable name for a code, so logs and reports never print an integer.
const char* to_string(ErrorCode code) noexcept;

/// A classified failure with optional context.
struct Error {
    ErrorCode code = ErrorCode::ok;
    std::string message;
    /// 1-based source line, when the failure belongs to a specific row.
    std::size_t line = 0;
    /// Column or field name, when known.
    std::string field;

    Error() = default;
    explicit Error(ErrorCode c, std::string msg = {}) : code(c), message(std::move(msg)) {}

    static Error at(ErrorCode c, std::string msg, std::size_t line_number) {
        Error error(c, std::move(msg));
        error.line = line_number;
        return error;
    }

    static Error in_field(ErrorCode c, std::string msg, std::string field_name, std::size_t line_number = 0) {
        Error error(c, std::move(msg));
        error.field = std::move(field_name);
        error.line = line_number;
        return error;
    }

    /// Multi-line description used by the CLI.
    [[nodiscard]] std::string describe() const;
};

/// Outcome of an operation that produces a value.
template <typename T>
class Result {
public:
    Result(T value) : data_(std::move(value)) {}                    // NOLINT(google-explicit-constructor)
    Result(Error error) : data_(std::move(error)) {}                // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<T>(data_);
    }
    explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] T& value() & { return std::get<T>(data_); }
    [[nodiscard]] const T& value() const& { return std::get<T>(data_); }
    [[nodiscard]] T&& value() && { return std::get<T>(std::move(data_)); }

    [[nodiscard]] const Error& error() const& { return std::get<Error>(data_); }

    /// Value if present, otherwise `fallback`.
    [[nodiscard]] T value_or(T fallback) const {
        return has_value() ? std::get<T>(data_) : std::move(fallback);
    }

private:
    std::variant<T, Error> data_;
};

/// Outcome of an operation that produces no value.
class Status {
public:
    Status() = default;                                             // success
    Status(Error error) : error_(std::move(error)) {}               // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }
    [[nodiscard]] const Error& error() const { return *error_; }

    static Status success() { return Status{}; }

private:
    std::optional<Error> error_;
};

inline Status success() { return Status::success(); }

}  // namespace imu_lab
