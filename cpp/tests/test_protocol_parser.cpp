// Tests for the firmware line parser: valid frames, every documented rejection
// path, boundary values and round-tripping.
#include <cmath>
#include <limits>
#include <string>

#include "imu_lab/protocol_parser.hpp"
#include "imu_lab/test_framework.hpp"

namespace {

using imu_lab::AttitudeLineParser;
using imu_lab::AttitudeSample;
using imu_lab::ErrorCode;
using imu_lab::Result;

/// `to_string` returns `const char*`; wrap it so CHECK_EQ compares contents
/// rather than pointers.
std::string code_name(ErrorCode code) { return std::string(imu_lab::to_string(code)); }

/// Assert that parsing `text` fails with `expected_code`.
void check_rejected(const std::string& text, ErrorCode expected_code) {
    const Result<AttitudeSample> parsed = AttitudeLineParser::parse_attitude_line(text);
    CHECK(!parsed);
    CHECK_EQ(code_name(parsed.error().code), code_name(expected_code));
}

}  // namespace

IMU_TEST(ProtocolParser, ParsesCurrentFirmwareFormat) {
    const Result<AttitudeSample> parsed = AttitudeLineParser::parse_attitude_line("P:10.25,R:-5.50");
    CHECK(parsed);
    CHECK_NEAR(parsed.value().pitch_deg, 10.25, 1e-12);
    CHECK_NEAR(parsed.value().roll_deg, -5.5, 1e-12);
    CHECK(!parsed.value().x.has_value());
    CHECK(!parsed.value().y.has_value());
}

IMU_TEST(ProtocolParser, AcceptsReversedFieldOrder) {
    const Result<AttitudeSample> parsed = AttitudeLineParser::parse_attitude_line("R:1.0,P:2.0");
    CHECK(parsed);
    CHECK_NEAR(parsed.value().pitch_deg, 2.0, 1e-12);
    CHECK_NEAR(parsed.value().roll_deg, 1.0, 1e-12);
}

IMU_TEST(ProtocolParser, TrimsWhitespaceAndCarriageReturns) {
    // Windows line endings leave a '\r' after each frame.
    const Result<AttitudeSample> parsed =
        AttitudeLineParser::parse_attitude_line("  P: 1.5 , R: -2.5 \r\n");
    CHECK(parsed);
    CHECK_NEAR(parsed.value().pitch_deg, 1.5, 1e-12);
    CHECK_NEAR(parsed.value().roll_deg, -2.5, 1e-12);
}

IMU_TEST(ProtocolParser, ParsesExperimentalPositionPair) {
    const Result<AttitudeSample> parsed =
        AttitudeLineParser::parse_attitude_line("P:1,R:2,X:0.5,Y:-0.25");
    CHECK(parsed);
    CHECK(parsed.value().x.has_value());
    CHECK(parsed.value().y.has_value());
    CHECK_NEAR(*parsed.value().x, 0.5, 1e-12);
    CHECK_NEAR(*parsed.value().y, -0.25, 1e-12);
}

IMU_TEST(ProtocolParser, RejectsEmptyAndWhitespaceOnlyLines) {
    check_rejected("", ErrorCode::empty_line);
    check_rejected("   \t ", ErrorCode::empty_line);
    check_rejected("\r\n", ErrorCode::empty_line);
}

IMU_TEST(ProtocolParser, RejectsFieldWithoutColon) {
    check_rejected("P1.5,R2.5", ErrorCode::missing_separator);
    check_rejected("P:1.5,R2.5", ErrorCode::missing_separator);
    check_rejected("P:1.5,R:2.5,", ErrorCode::missing_separator);
}

IMU_TEST(ProtocolParser, RejectsUnknownField) {
    check_rejected("P:1,R:2,Z:3", ErrorCode::unknown_field);
    check_rejected("p:1,r:2", ErrorCode::unknown_field);  // case sensitive on purpose
}

IMU_TEST(ProtocolParser, RejectsDuplicateField) {
    check_rejected("P:1,P:2,R:3", ErrorCode::duplicate_field);
    check_rejected("P:1,R:2,X:3,X:4,Y:5", ErrorCode::duplicate_field);
}

IMU_TEST(ProtocolParser, RejectsNonNumericField) {
    check_rejected("P:abc,R:2", ErrorCode::not_numeric);
    check_rejected("P:1,R:", ErrorCode::not_numeric);
    check_rejected("P:1e,R:2", ErrorCode::not_numeric);
}

IMU_TEST(ProtocolParser, RejectsNonFiniteField) {
    check_rejected("P:nan,R:2", ErrorCode::not_finite);
    check_rejected("P:1,R:inf", ErrorCode::not_finite);
    check_rejected("P:-inf,R:2", ErrorCode::not_finite);
}

IMU_TEST(ProtocolParser, RejectsMissingRequiredFields) {
    check_rejected("P:1", ErrorCode::missing_required_field);
    check_rejected("R:2", ErrorCode::missing_required_field);
    check_rejected("X:1,Y:2", ErrorCode::missing_required_field);
}

IMU_TEST(ProtocolParser, RejectsIncompletePositionPair) {
    check_rejected("P:1,R:2,X:3", ErrorCode::incomplete_position_pair);
    check_rejected("P:1,R:2,Y:3", ErrorCode::incomplete_position_pair);
}

IMU_TEST(ProtocolParser, AcceptsBoundaryValues) {
    double value = 0.0;

    Result<AttitudeSample> parsed = AttitudeLineParser::parse_attitude_line("P:0,R:0");
    CHECK(parsed);
    CHECK_NEAR(parsed.value().pitch_deg, 0.0, 0.0);

    parsed = AttitudeLineParser::parse_attitude_line("P:-180,R:180");
    CHECK(parsed);
    CHECK_NEAR(parsed.value().pitch_deg, -180.0, 1e-12);
    CHECK_NEAR(parsed.value().roll_deg, 180.0, 1e-12);

    // A physically absurd but finite value must still parse: the parser's job is
    // syntax, and rejecting outliers is the jump detector's job.
    parsed = AttitudeLineParser::parse_attitude_line("P:99999.5,R:-99999.5");
    CHECK(parsed);
    CHECK_NEAR(parsed.value().pitch_deg, 99999.5, 1e-9);

    (void)value;
}

IMU_TEST(ProtocolParser, RoundTripsThroughFormatting) {
    const AttitudeSample original{12.5, -3.25, 0.5, -0.75};
    const std::string line = imu_lab::format_attitude_line(original);
    CHECK_EQ(line.substr(0, 2), std::string("P:"));

    const Result<AttitudeSample> parsed = AttitudeLineParser::parse_attitude_line(line);
    CHECK(parsed);
    CHECK_NEAR(parsed.value().pitch_deg, original.pitch_deg, 1e-6);
    CHECK_NEAR(parsed.value().roll_deg, original.roll_deg, 1e-6);
    CHECK(parsed.value().x.has_value());
    CHECK_NEAR(*parsed.value().x, *original.x, 1e-6);
}

IMU_TEST(ProtocolParser, FormatsWithoutPositionPair) {
    const AttitudeSample sample{1.0, 2.0, std::nullopt, std::nullopt};
    const std::string line = imu_lab::format_attitude_line(sample);
    CHECK(line.find("X:") == std::string::npos);
    CHECK(line.find("Y:") == std::string::npos);
    CHECK_EQ(line, std::string("P:1.000000,R:2.000000"));
}

// --------------------------------------------------------------------------- //
// Full IMU wire protocol
// --------------------------------------------------------------------------- //
IMU_TEST(ProtocolParser, ParsesFullImuRecord) {
    const std::string line =
        "IMU,42,1500,0.01,0.02,1.00,0.1,0.2,0.3,10,20,30,1.5,2.5,3.5";
    const Result<imu_lab::ImuRecord> parsed = AttitudeLineParser::parse_imu_record(line);
    CHECK(parsed);
    CHECK_EQ(parsed.value().sequence, static_cast<std::int64_t>(42));
    CHECK_EQ(parsed.value().t_ms, static_cast<std::int64_t>(1500));
    CHECK_NEAR(parsed.value().ax_g, 0.01, 1e-12);
    CHECK_NEAR(parsed.value().mz_uT, 30.0, 1e-12);
    CHECK_NEAR(parsed.value().roll_deg, 1.5, 1e-12);
    CHECK_NEAR(parsed.value().pitch_deg, 2.5, 1e-12);
    CHECK_NEAR(parsed.value().yaw_deg, 3.5, 1e-12);
}

IMU_TEST(ProtocolParser, RejectsWrongImuPrefix) {
    const Result<imu_lab::ImuRecord> parsed =
        AttitudeLineParser::parse_imu_record("IMX,1,2,3,4,5,6,7,8,9,10,11,12,13,14");
    CHECK(!parsed);
    CHECK_EQ(code_name(parsed.error().code), code_name(ErrorCode::invalid_prefix));
}

IMU_TEST(ProtocolParser, RejectsWrongImuFieldCount) {
    const Result<imu_lab::ImuRecord> parsed = AttitudeLineParser::parse_imu_record("IMU,1,2,3");
    CHECK(!parsed);
    CHECK_EQ(code_name(parsed.error().code), code_name(ErrorCode::wrong_field_count));
}

IMU_TEST(ProtocolParser, RejectsNegativeSequenceAndTime) {
    Result<imu_lab::ImuRecord> parsed = AttitudeLineParser::parse_imu_record(
        "IMU,-1,1500,0,0,0,0,0,0,0,0,0,0,0,0");
    CHECK(!parsed);
    CHECK_EQ(code_name(parsed.error().code), code_name(ErrorCode::negative_value));

    parsed = AttitudeLineParser::parse_imu_record(
        "IMU,1,-5,0,0,0,0,0,0,0,0,0,0,0,0");
    CHECK(!parsed);
    CHECK_EQ(code_name(parsed.error().code), code_name(ErrorCode::negative_value));
}

IMU_TEST(ProtocolParser, ImuRecordNonNumericFieldIsReportedWithItsName) {
    const Result<imu_lab::ImuRecord> parsed = AttitudeLineParser::parse_imu_record(
        "IMU,1,2,0,0,0,0,0,0,0,0,0,0,0,oops");
    CHECK(!parsed);
    CHECK_EQ(parsed.error().field, std::string("yaw_deg"));
}

IMU_TEST(ProtocolParser, LooksLikeDetectsTheImuPrefix) {
    CHECK(AttitudeLineParser::looks_like_imu_record("IMU,1,2"));
    CHECK(AttitudeLineParser::looks_like_imu_record("  IMU,1,2"));
    CHECK(!AttitudeLineParser::looks_like_imu_record("P:1,R:2"));
    CHECK(!AttitudeLineParser::looks_like_imu_record("IMU"));
    CHECK(!AttitudeLineParser::looks_like_imu_record("imu,1,2"));
}
