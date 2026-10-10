// CSV loading and row-level validation for attitude logs.
//
// The C++ side mirrors the Python tooling's schema exactly, so a log produced
// by `python -m host.pipeline` can be analysed by `imu_lab_cli analyze` and vice
// versa. That round-trip property is asserted by the unit tests.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "imu_lab/attitude_sample.hpp"
#include "imu_lab/error.hpp"

namespace imu_lab {

/// One validated CSV row.
struct CsvRecord {
    std::string timestamp_utc;
    double elapsed_s = 0.0;
    AttitudeSample sample;
    std::string raw_line;
};

/// A single finding produced while validating a CSV file.
struct RowIssue {
    ErrorCode code = ErrorCode::ok;
    /// 1-based line number in the file (the header is line 1).
    std::size_t line = 0;
    std::string column;
    std::string message;

    [[nodiscard]] std::string describe() const;
};

/// Aggregate result of validating one file.
struct ValidationReport {
    std::string path;
    bool file_exists = false;
    std::size_t size_bytes = 0;
    std::vector<std::string> header;
    std::size_t total_rows = 0;
    std::size_t valid_rows = 0;
    std::size_t invalid_rows = 0;
    std::vector<RowIssue> issues;
    /// Findings dropped because `max_issues` was reached.
    std::size_t truncated_issues = 0;

    /// True when the file can be used (warnings allowed).
    [[nodiscard]] bool ok() const;
    [[nodiscard]] std::size_t count_severity(ErrorCode code) const;
    [[nodiscard]] bool has_fatal() const;
    [[nodiscard]] std::size_t warnings() const;
    [[nodiscard]] double invalid_ratio() const;
    /// Multi-line human-readable summary, matching the Python renderer's spirit.
    [[nodiscard]] std::string render(std::size_t limit = 20) const;
    /// One-line summary suitable for the pipeline report table.
    [[nodiscard]] std::string summary_line() const;
};

/// Records plus the validation outcome, returned by `load`.
struct CsvLoadResult {
    std::vector<CsvRecord> records;
    std::size_t skipped_rows = 0;
    ValidationReport report;
};

/// Stateless CSV reader.
class CsvReader {
public:
    /// Columns every attitude log must provide.
    static const std::vector<std::string>& required_columns();

    /// Columns recognised when present (anything else is reported as unknown).
    static const std::vector<std::string>& known_columns();

    /// Validate a file and describe every problem found.
    ///
    /// Never fails: a missing file is reported as `file_exists == false` so the
    /// caller can produce a friendly error with the right exit code.
    static ValidationReport validate(const std::string& path, std::size_t max_issues = 200);

    /// Load the valid rows of a file.
    ///
    /// @return `Error` for file-level problems (missing file, missing columns,
    ///         empty file); row-level problems are counted in
    ///         `CsvLoadResult::skipped_rows` and detailed in the report.
    static Result<CsvLoadResult> load(const std::string& path, std::size_t max_issues = 200);

    /// Split one CSV line honouring RFC 4180 quoting, which matters because
    /// `raw_line` legitimately contains commas and is therefore quoted.
    static std::vector<std::string> split_line(const std::string& line);

    /// Parse one already-split row into a record.
    ///
    /// @param header column names matching `fields` by position.
    static Result<CsvRecord> parse_row(const std::vector<std::string>& header,
                                       const std::vector<std::string>& fields,
                                       std::size_t line_number);
};

}  // namespace imu_lab
