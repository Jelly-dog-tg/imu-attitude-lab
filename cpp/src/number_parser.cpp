#include "imu_lab/number_parser.hpp"

#include <array>
#include <cstdint>
#include <limits>

namespace imu_lab {
namespace {

constexpr bool is_digit(char character) noexcept {
    return character >= '0' && character <= '9';
}

constexpr char to_lower(char character) noexcept {
    return (character >= 'A' && character <= 'Z') ? static_cast<char>(character + ('a' - 'A')) : character;
}

/// Exact powers of ten that are representable as a double without rounding.
constexpr std::array<double, 23> kExactPowersOfTen = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

/// Scale `mantissa` by 10^exponent.
///
/// Splitting the exponent keeps the common case exact (a single table lookup).
/// Exponents beyond +/-350 cannot be represented by a double at all, so they are
/// short-circuited to inf/0 instead of being scaled step by step.
double scale_by_power_of_ten(std::uint64_t mantissa, int exponent) noexcept {
    constexpr int kMaxExact = static_cast<int>(kExactPowersOfTen.size()) - 1;
    constexpr int kRepresentableLimit = 350;

    if (mantissa == 0) {
        return 0.0;
    }
    if (exponent > kRepresentableLimit) {
        return std::numeric_limits<double>::infinity();
    }
    if (exponent < -kRepresentableLimit) {
        return 0.0;
    }

    double result = static_cast<double>(mantissa);
    int remaining = exponent;
    while (remaining > kMaxExact) {
        result *= kExactPowersOfTen[kMaxExact];
        remaining -= kMaxExact;
    }
    while (remaining < -kMaxExact) {
        result /= kExactPowersOfTen[kMaxExact];
        remaining += kMaxExact;
    }
    if (remaining >= 0) {
        result *= kExactPowersOfTen[static_cast<std::size_t>(remaining)];
    } else {
        result /= kExactPowersOfTen[static_cast<std::size_t>(-remaining)];
    }
    return result;
}

/// Recognise `nan`, `inf`, `infinity` in any case, with an optional sign.
bool parse_non_finite(std::string_view text, double& out) noexcept {
    if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return false;
    }

    char lowered[8] = {};
    // "infinity" is exactly 8 characters, so the guard must be `>` not `>=`.
    if (text.size() > sizeof(lowered)) {
        return false;
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        lowered[index] = to_lower(text[index]);
    }
    const std::string_view word(lowered, text.size());

    if (word == "nan") {
        out = std::numeric_limits<double>::quiet_NaN();
        return true;
    }
    if (word == "inf" || word == "infinity") {
        out = std::numeric_limits<double>::infinity();
        return true;
    }
    return false;
}

}  // namespace

bool parse_double(std::string_view text, double& out) noexcept {
    if (text.empty()) {
        return false;
    }

    // Non-finite words first: they contain no digits so the scanner below would
    // reject them, but the caller wants to report them as "not finite".
    {
        double non_finite = 0.0;
        if (parse_non_finite(text, non_finite)) {
            // Preserve the sign so "-inf" does not silently become "+inf".
            out = (text.front() == '-') ? -non_finite : non_finite;
            return true;
        }
    }

    std::size_t index = 0;
    bool negative = false;
    if (text[index] == '+' || text[index] == '-') {
        negative = (text[index] == '-');
        ++index;
    }

    //: Beyond 19 significant digits a double cannot represent the difference,
    //: so extra digits are consumed but only shift the exponent.
    constexpr int kMaxSignificantDigits = 19;

    std::uint64_t mantissa = 0;
    int significant_digits = 0;
    int exponent = 0;
    bool saw_digit = false;

    while (index < text.size() && is_digit(text[index])) {
        if (significant_digits < kMaxSignificantDigits) {
            mantissa = mantissa * 10u + static_cast<std::uint64_t>(text[index] - '0');
            ++significant_digits;
        } else {
            ++exponent;
        }
        saw_digit = true;
        ++index;
    }

    if (index < text.size() && text[index] == '.') {
        ++index;
        while (index < text.size() && is_digit(text[index])) {
            if (significant_digits < kMaxSignificantDigits) {
                mantissa = mantissa * 10u + static_cast<std::uint64_t>(text[index] - '0');
                ++significant_digits;
                --exponent;
            }
            saw_digit = true;
            ++index;
        }
    }

    if (!saw_digit) {
        return false;  // ".", "+", "-", "e5" and friends
    }

    if (index < text.size() && (text[index] == 'e' || text[index] == 'E')) {
        ++index;
        int exponent_sign = 1;
        if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
            exponent_sign = (text[index] == '-') ? -1 : 1;
            ++index;
        }
        if (index >= text.size() || !is_digit(text[index])) {
            return false;  // "1e", "1e+"
        }
        int explicit_exponent = 0;
        while (index < text.size() && is_digit(text[index])) {
            // Clamp so a malicious "1e999999999" cannot overflow the counter.
            if (explicit_exponent < 1'000'000) {
                explicit_exponent = explicit_exponent * 10 + (text[index] - '0');
            }
            ++index;
        }
        exponent += exponent_sign * explicit_exponent;
    }

    if (index != text.size()) {
        return false;  // trailing garbage such as "1.5abc" or "1,5"
    }

    const double magnitude = scale_by_power_of_ten(mantissa, exponent);
    out = negative ? -magnitude : magnitude;
    return true;
}

}  // namespace imu_lab
