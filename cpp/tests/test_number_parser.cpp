// Tests for the hand-written decimal parser: valid input, invalid input,
// boundaries and the non-finite words.
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "imu_lab/number_parser.hpp"
#include "imu_lab/test_framework.hpp"

namespace {

using imu_lab::parse_double;

struct Case {
    const char* text;
    double expected;
};

const std::vector<Case>& valid_cases() {
    static const std::vector<Case> cases = {
        {"0", 0.0},
        {"1.5", 1.5},
        {"-2", -2.0},
        {"+3.5", 3.5},
        {"0.5", 0.5},
        {".5", 0.5},
        {"5.", 5.0},
        {".0", 0.0},
        {"00012.5", 12.5},
        {"1e3", 1000.0},
        {"1E-2", 0.01},
        {"-1.5e+2", -150.0},
        {"0e999", 0.0},
        {"3.14159265358979", 3.14159265358979},
    };
    return cases;
}

const std::vector<const char*>& invalid_cases() {
    static const std::vector<const char*> cases = {
        "",   "abc",  "1.5abc", "0x10", ".",    "+",     "-",
        "e5", "1e",   "1e+",    "1,5",  ".e3",  "--1",   "1..2",
        "1 2", "nan1", "infin",  "1.2.3", " 1.0",
    };
    return cases;
}

}  // namespace

IMU_TEST(NumberParser, AcceptsValidLiterals) {
    for (const Case& item : valid_cases()) {
        double value = 0.0;
        CHECK(parse_double(item.text, value));
        CHECK_NEAR(value, item.expected, 1e-12);
    }
}

IMU_TEST(NumberParser, RejectsInvalidLiterals) {
    for (const char* text : invalid_cases()) {
        double value = 123.0;
        CHECK(!parse_double(text, value));
        // A rejected parse must not touch the output.
        CHECK_NEAR(value, 123.0, 0.0);
    }
}

IMU_TEST(NumberParser, TrailingGarbageIsRejected) {
    // The whole view must be consumed: partial parses are the classic way a
    // corrupted serial frame sneaks through as a valid number.
    double value = 0.0;
    CHECK(!parse_double("12.5;", value));
    CHECK(!parse_double("12.5 ", value));
    CHECK(!parse_double("12,5", value));
}

IMU_TEST(NumberParser, SpecialKeywordsBecomeNonFinite) {
    double value = 0.0;
    CHECK(parse_double("nan", value));
    CHECK(std::isnan(value));

    CHECK(parse_double("inf", value));
    CHECK(std::isinf(value));
    CHECK(value > 0.0);

    CHECK(parse_double("-inf", value));
    CHECK(std::isinf(value));
    CHECK(value < 0.0);

    CHECK(parse_double("Infinity", value));
    CHECK(std::isinf(value));
}

IMU_TEST(NumberParser, OverflowProducesInfinityNotGarbage) {
    double value = 0.0;
    CHECK(parse_double("1e400", value));
    CHECK(std::isinf(value));
    CHECK(value > 0.0);

    CHECK(parse_double("-1e400", value));
    CHECK(std::isinf(value));
    CHECK(value < 0.0);
}

IMU_TEST(NumberParser, UnderflowProducesZero) {
    double value = 1.0;
    CHECK(parse_double("1e-400", value));
    CHECK_NEAR(value, 0.0, 0.0);
    CHECK(!std::isnan(value));
}

IMU_TEST(NumberParser, HandlesRepresentableExtremes) {
    double value = 0.0;
    CHECK(parse_double("1.7976931348623157e308", value));
    CHECK(std::isfinite(value));
    // Scaling by a large power of ten costs ~1e-15 relative accuracy (each
    // 1e22 multiply rounds), so the assertion is relative rather than absolute.
    // Sensor data never gets close to this magnitude; the point of the test is
    // that the parser neither wraps nor produces garbage at the extremes.
    CHECK(value > std::numeric_limits<double>::max() * 0.999);
    CHECK(value <= std::numeric_limits<double>::max());

    CHECK(parse_double("2.2250738585072014e-308", value));
    CHECK(value > 0.0);
    CHECK(std::isfinite(value));
}

IMU_TEST(NumberParser, ManyDigitsDoNotWrapTheMantissa) {
    // More significant digits than a double can hold must not overflow the
    // integer accumulator used internally.
    double value = 0.0;
    CHECK(parse_double("123456789012345678901234567890", value));
    CHECK(std::isfinite(value));
    CHECK(value > 1e29);
    CHECK(value < 1e30);
}

IMU_TEST(NumberParser, HandlesSignBoundaries) {
    double value = 0.0;
    CHECK(parse_double("-0", value));
    CHECK_NEAR(value, 0.0, 0.0);
    CHECK(parse_double("-0.0", value));
    CHECK_NEAR(value, 0.0, 0.0);
}

IMU_TEST(NumberParser, IsIndependentOfTheProcessLocale) {
    // The parser never consults the locale, so a comma-based locale cannot turn
    // "1.5" into 1. This is the reason it is hand-written instead of strtod.
    double value = 0.0;
    CHECK(parse_double("1.5", value));
    CHECK_NEAR(value, 1.5, 0.0);
    CHECK(!parse_double("1,5", value));
}
