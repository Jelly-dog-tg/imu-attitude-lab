// Tests for CSV splitting, loading and validation.
#include <string>
#include <vector>

#include "imu_lab/csv_reader.hpp"
#include "imu_lab/test_framework.hpp"
#include "test_support.hpp"

namespace {

using imu_lab::CsvReader;
using imu_lab::ErrorCode;
using imu_lab::RowIssue;
using imu_lab::ValidationReport;
using imu_lab::test::ScratchDir;

const char* kHeader = "timestamp_utc,elapsed_s,pitch_deg,roll_deg,x,y,raw_line";

std::string row(const std::string& elapsed, const std::string& pitch, const std::string& roll,
                const std::string& raw = "") {
    return "2026-01-01T00:00:00.000Z," + elapsed + "," + pitch + "," + roll + ",,,\"" + raw + "\"";
}

bool has_issue_with(const ValidationReport& report, ErrorCode code) {
    for (const RowIssue& issue : report.issues) {
        if (issue.code == code) {
            return true;
        }
    }
    return false;
}

}  // namespace

// --------------------------------------------------------------------------- //
// split_line
// --------------------------------------------------------------------------- //
IMU_TEST(CsvReader, SplitLineHandlesPlainFields) {
    const std::vector<std::string> fields = CsvReader::split_line("a,b,c");
    CHECK_EQ(fields.size(), static_cast<std::size_t>(3));
    CHECK_EQ(fields[0], std::string("a"));
    CHECK_EQ(fields[2], std::string("c"));
}

IMU_TEST(CsvReader, SplitLineHandlesQuotedCommas) {
    // raw_line legitimately contains commas, so this is the case that matters.
    const std::vector<std::string> fields =
        CsvReader::split_line("2026-01-01T00:00:00Z,0.5,1.0,2.0,,,\"P:1.0,R:2.0\"");
    CHECK_EQ(fields.size(), static_cast<std::size_t>(7));
    CHECK_EQ(fields[6], std::string("P:1.0,R:2.0"));
}

IMU_TEST(CsvReader, SplitLineHandlesEscapedQuotes) {
    const std::vector<std::string> fields = CsvReader::split_line("a,\"say \"\"hi\"\"\",c");
    CHECK_EQ(fields.size(), static_cast<std::size_t>(3));
    CHECK_EQ(fields[1], std::string("say \"hi\""));
}

IMU_TEST(CsvReader, SplitLineKeepsEmptyFields) {
    const std::vector<std::string> fields = CsvReader::split_line("a,,c,");
    CHECK_EQ(fields.size(), static_cast<std::size_t>(4));
    CHECK_EQ(fields[1], std::string(""));
    CHECK_EQ(fields[3], std::string(""));
}

IMU_TEST(CsvReader, SplitLineStripsCarriageReturns) {
    const std::vector<std::string> fields = CsvReader::split_line("a,b\r");
    CHECK_EQ(fields.size(), static_cast<std::size_t>(2));
    CHECK_EQ(fields[1], std::string("b"));
}

// --------------------------------------------------------------------------- //
// load
// --------------------------------------------------------------------------- //
IMU_TEST(CsvReader, LoadsAValidFile) {
    ScratchDir scratch;
    const std::string path = scratch.write("good.csv",
                                           std::string(kHeader) + "\n" +
                                               row("0.000000", "1.0", "2.0", "P:1.0,R:2.0") + "\n" +
                                               row("0.050000", "1.5", "2.5", "P:1.5,R:2.5") + "\n");

    const auto loaded = CsvReader::load(path);
    CHECK(loaded);
    CHECK_EQ(loaded.value().records.size(), static_cast<std::size_t>(2));
    CHECK_EQ(loaded.value().skipped_rows, static_cast<std::size_t>(0));
    CHECK_NEAR(loaded.value().records[0].sample.pitch_deg, 1.0, 1e-12);
    CHECK_NEAR(loaded.value().records[1].elapsed_s, 0.05, 1e-12);
    CHECK_EQ(loaded.value().records[1].raw_line, std::string("P:1.5,R:2.5"));
    CHECK(loaded.value().report.ok());
}

IMU_TEST(CsvReader, LoadsAFileWithUtf8Bom) {
    ScratchDir scratch;
    const std::string path = scratch.write(
        "bom.csv", "\xEF\xBB\xBF" + std::string(kHeader) + "\n" + row("0", "1", "2") + "\n");
    const auto loaded = CsvReader::load(path);
    CHECK(loaded);
    CHECK_EQ(loaded.value().records.size(), static_cast<std::size_t>(1));
}

IMU_TEST(CsvReader, ParsesOptionalPositionPair) {
    ScratchDir scratch;
    const std::string path = scratch.write(
        "pos.csv", std::string(kHeader) +
                       "\n2026-01-01T00:00:00Z,0.0,1.0,2.0,0.5,-0.25,\"P:1,R:2\"\n");
    const auto loaded = CsvReader::load(path);
    CHECK(loaded);
    CHECK(loaded.value().records[0].sample.x.has_value());
    CHECK_NEAR(*loaded.value().records[0].sample.y, -0.25, 1e-12);
}

IMU_TEST(CsvReader, MissingFileIsAFileNotFoundError) {
    ScratchDir scratch;
    const auto loaded = CsvReader::load(scratch.missing("nope.csv"));
    CHECK(!loaded);
    CHECK_EQ(loaded.error().code, ErrorCode::file_not_found);
}

IMU_TEST(CsvReader, EmptyFileIsRejected) {
    ScratchDir scratch;
    const std::string path = scratch.write("empty.csv", "");
    const auto loaded = CsvReader::load(path);
    CHECK(!loaded);
    CHECK_EQ(loaded.error().code, ErrorCode::empty_file);
}

IMU_TEST(CsvReader, MissingRequiredColumnIsRejected) {
    ScratchDir scratch;
    const std::string path = scratch.write("wrong.csv", "a,b,c\n1,2,3\n");
    const auto loaded = CsvReader::load(path);
    CHECK(!loaded);
    CHECK_EQ(loaded.error().code, ErrorCode::missing_column);
}

IMU_TEST(CsvReader, HeaderOnlyFileIsRejected) {
    ScratchDir scratch;
    const std::string path = scratch.write("header_only.csv", std::string(kHeader) + "\n");
    const auto loaded = CsvReader::load(path);
    CHECK(!loaded);
    CHECK_EQ(loaded.error().code, ErrorCode::empty_file);
}

// --------------------------------------------------------------------------- //
// validate
// --------------------------------------------------------------------------- //
IMU_TEST(CsvReader, ReportsEveryBadRowWithALineNumber) {
    ScratchDir scratch;
    const std::string path = scratch.write(
        "bad.csv",
        std::string(kHeader) + "\n" + row("0.0", "1.0", "2.0") + "\n" +   // line 2, fine
            row("abc", "1.5", "2.5") + "\n" +                            // line 3, not numeric
            row("-1", "3.0", "4.0") + "\n" +                             // line 4, negative
            row("0.15", "", "4.5") + "\n" +                              // line 5, empty
            row("0.20", "5.0", "nan") + "\n");                           // line 6, not finite

    const ValidationReport report = CsvReader::validate(path);
    CHECK_EQ(report.total_rows, static_cast<std::size_t>(5));
    CHECK_EQ(report.valid_rows, static_cast<std::size_t>(1));
    CHECK_EQ(report.invalid_rows, static_cast<std::size_t>(4));
    CHECK(report.ok());  // partial data is still usable
    CHECK(has_issue_with(report, ErrorCode::not_numeric));
    CHECK(has_issue_with(report, ErrorCode::negative_elapsed));
    CHECK(has_issue_with(report, ErrorCode::missing_required_field));
    CHECK(has_issue_with(report, ErrorCode::not_finite));
}

IMU_TEST(CsvReader, RowIssuesCarryTheColumnName) {
    ScratchDir scratch;
    const std::string path =
        scratch.write("col.csv", std::string(kHeader) + "\n" + row("oops", "1", "2") + "\n");
    const ValidationReport report = CsvReader::validate(path);

    // The report also carries a file-level "every row was rejected" finding, so
    // locate the row-level one instead of assuming it is the only entry.
    const RowIssue* numeric_issue = nullptr;
    for (const RowIssue& issue : report.issues) {
        if (issue.code == ErrorCode::not_numeric) {
            numeric_issue = &issue;
        }
    }
    CHECK(numeric_issue != nullptr);
    CHECK_EQ(numeric_issue->column, std::string("elapsed_s"));
    CHECK_EQ(numeric_issue->line, static_cast<std::size_t>(2));
}

IMU_TEST(CsvReader, RejectsRowsWithMoreFieldsThanTheHeader) {
    ScratchDir scratch;
    // An unquoted comma inside raw_line is the classic way this happens.
    const std::string path = scratch.write(
        "overflow.csv", std::string(kHeader) + "\n2026-01-01T00:00:00Z,0,1,2,,,P:1,R:2\n");
    const ValidationReport report = CsvReader::validate(path);
    CHECK(has_issue_with(report, ErrorCode::wrong_field_count));
}

IMU_TEST(CsvReader, RejectsIncompletePositionPairInARow) {
    ScratchDir scratch;
    const std::string path = scratch.write(
        "pair.csv", std::string(kHeader) + "\n2026-01-01T00:00:00Z,0,1,2,0.5,,\"P:1,R:2\"\n");
    const ValidationReport report = CsvReader::validate(path);
    CHECK(has_issue_with(report, ErrorCode::incomplete_position_pair));
    CHECK_EQ(report.invalid_rows, static_cast<std::size_t>(1));
}

IMU_TEST(CsvReader, UnknownColumnsAreOnlyAdvisory) {
    ScratchDir scratch;
    const std::string path = scratch.write(
        "extra.csv",
        "timestamp_utc,elapsed_s,pitch_deg,roll_deg,temperature_c\n2026-01-01T00:00:00Z,0,1,2,25\n");
    const ValidationReport report = CsvReader::validate(path);
    CHECK(report.ok());
    CHECK(has_issue_with(report, ErrorCode::unknown_column));
    CHECK(report.warnings() >= static_cast<std::size_t>(1));
}

IMU_TEST(CsvReader, DuplicateColumnsAreFatal) {
    ScratchDir scratch;
    const std::string path = scratch.write(
        "dup.csv",
        "timestamp_utc,elapsed_s,pitch_deg,roll_deg,elapsed_s\n"
        "2026-01-01T00:00:00Z,0,1,2,0\n");
    const ValidationReport report = CsvReader::validate(path);
    CHECK(!report.ok());
    CHECK(has_issue_with(report, ErrorCode::duplicate_column));
}

IMU_TEST(CsvReader, MissingFileReportIsNotUsable) {
    ScratchDir scratch;
    const ValidationReport report = CsvReader::validate(scratch.missing("ghost.csv"));
    CHECK(!report.file_exists);
    CHECK(!report.ok());
    CHECK_EQ(report.valid_rows, static_cast<std::size_t>(0));
}

IMU_TEST(CsvReader, EmptyFileReportIsNotUsable) {
    ScratchDir scratch;
    const ValidationReport report = CsvReader::validate(scratch.write("zero.csv", ""));
    CHECK(report.file_exists);
    CHECK(!report.ok());
    CHECK(has_issue_with(report, ErrorCode::empty_file));
}

IMU_TEST(CsvReader, IssueCapKeepsCountsAccurate) {
    ScratchDir scratch;
    std::string content = kHeader;
    for (int index = 0; index < 50; ++index) {
        content += "\n" + row("bad", "1", "2");
    }
    const std::string path = scratch.write("many.csv", content + "\n");

    const ValidationReport report = CsvReader::validate(path, 5);
    // Every finding goes through the same cap, including the summary finding, so
    // a corrupt file can never grow the report without bound.
    CHECK_EQ(report.issues.size(), static_cast<std::size_t>(5));
    CHECK(report.truncated_issues > static_cast<std::size_t>(0));
    CHECK_EQ(report.total_rows, static_cast<std::size_t>(50));
    CHECK_EQ(report.invalid_rows, static_cast<std::size_t>(50));
}

IMU_TEST(CsvReader, RenderAndSummaryMentionTheVerdict) {
    ScratchDir scratch;
    const std::string good =
        scratch.write("g.csv", std::string(kHeader) + "\n" + row("0", "1", "2") + "\n");
    const ValidationReport good_report = CsvReader::validate(good);
    CHECK(good_report.render().find("USABLE") != std::string::npos);
    CHECK(good_report.summary_line().find("1/1") != std::string::npos);

    const std::string bad = scratch.write("b.csv", "x,y\n1,2\n");
    CHECK(CsvReader::validate(bad).render().find("NOT USABLE") != std::string::npos);
}
