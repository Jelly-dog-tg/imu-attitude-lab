#include "imu_lab/csv_reader.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_set>

#include "imu_lab/number_parser.hpp"

namespace imu_lab {
namespace {

/// Strip a UTF-8 BOM, which Windows tools love to prepend.
void strip_bom(std::string& text) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
}

std::string trim_copy(const std::string& text) {
    constexpr const char* kWhitespace = " \t\r\n\f\v";
    const std::size_t first = text.find_first_not_of(kWhitespace);
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(kWhitespace);
    return text.substr(first, last - first + 1);
}

/// Hard failures: the whole file is unusable, so `ok()` must be false.
bool is_fatal(ErrorCode code) {
    switch (code) {
        case ErrorCode::file_not_found:
        case ErrorCode::empty_file:
        case ErrorCode::missing_column:
        case ErrorCode::duplicate_column:
            return true;
        default:
            return false;
    }
}

/// Advisory findings: the file is still usable.
bool is_warning(ErrorCode code) {
    return !is_fatal(code);
}

}  // namespace

std::string RowIssue::describe() const {
    std::string text = "[";
    text += to_string(code);
    text += "] line ";
    text += std::to_string(line);
    if (!column.empty()) {
        text += " column '";
        text += column;
        text += "'";
    }
    text += ": ";
    text += message;
    return text;
}

bool ValidationReport::ok() const {
    if (!file_exists || valid_rows == 0) {
        return false;
    }
    return !has_fatal();
}

bool ValidationReport::has_fatal() const {
    return std::any_of(issues.begin(), issues.end(),
                       [](const RowIssue& issue) { return is_fatal(issue.code); });
}

std::size_t ValidationReport::count_severity(ErrorCode code) const {
    return static_cast<std::size_t>(std::count_if(
        issues.begin(), issues.end(), [code](const RowIssue& issue) { return issue.code == code; }));
}

std::size_t ValidationReport::warnings() const {
    return static_cast<std::size_t>(
        std::count_if(issues.begin(), issues.end(),
                      [](const RowIssue& issue) { return is_warning(issue.code); }));
}

double ValidationReport::invalid_ratio() const {
    return total_rows == 0 ? 0.0
                           : static_cast<double>(invalid_rows) / static_cast<double>(total_rows);
}

std::string ValidationReport::render(std::size_t limit) const {
    std::ostringstream out;
    out << "Validation report for " << path << "\n";
    if (!file_exists) {
        out << "  file does not exist\n";
        return out.str();
    }
    out << "  size: " << size_bytes << " bytes\n";
    out << "  columns: ";
    for (std::size_t index = 0; index < header.size(); ++index) {
        out << (index ? ", " : "") << header[index];
    }
    if (header.empty()) {
        out << "<none>";
    }
    out << "\n";
    out << "  rows: " << total_rows << " total, " << valid_rows << " valid, " << invalid_rows
        << " invalid\n";
    out << "  findings: " << issues.size() << " recorded";
    if (truncated_issues != 0) {
        out << " (+" << truncated_issues << " suppressed)";
    }
    out << "\n";
    const std::size_t shown = std::min(limit, issues.size());
    for (std::size_t index = 0; index < shown; ++index) {
        out << "    " << issues[index].describe() << "\n";
    }
    if (issues.size() > shown) {
        out << "    ... and " << (issues.size() - shown + truncated_issues)
            << " more finding(s)\n";
    }
    out << "  verdict: " << (ok() ? "USABLE" : "NOT USABLE") << "\n";
    return out.str();
}

std::string ValidationReport::summary_line() const {
    std::ostringstream out;
    out << valid_rows << '/' << total_rows << " rows valid, " << invalid_rows << " rejected, "
        << issues.size() << " finding(s)";
    return out.str();
}

const std::vector<std::string>& CsvReader::required_columns() {
    static const std::vector<std::string> columns = {
        "timestamp_utc", "elapsed_s", "pitch_deg", "roll_deg"};
    return columns;
}

const std::vector<std::string>& CsvReader::known_columns() {
    static const std::vector<std::string> columns = {
        "timestamp_utc", "elapsed_s", "pitch_deg", "roll_deg", "x", "y", "raw_line"};
    return columns;
}

std::vector<std::string> CsvReader::split_line(const std::string& line) {
    std::vector<std::string> fields;
    std::string current;
    bool in_quotes = false;

    for (std::size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];

        if (in_quotes) {
            if (character == '"') {
                // "" inside a quoted field is a literal quote.
                if (index + 1 < line.size() && line[index + 1] == '"') {
                    current.push_back('"');
                    ++index;
                } else {
                    in_quotes = false;
                }
            } else {
                current.push_back(character);
            }
            continue;
        }

        if (character == '"' && current.empty()) {
            in_quotes = true;
        } else if (character == ',') {
            fields.push_back(current);
            current.clear();
        } else if (character != '\r') {
            current.push_back(character);
        }
    }
    fields.push_back(current);
    return fields;
}

Result<CsvRecord> CsvReader::parse_row(const std::vector<std::string>& header,
                                       const std::vector<std::string>& fields,
                                       std::size_t line_number) {
    if (fields.size() > header.size()) {
        return Error::at(ErrorCode::wrong_field_count,
                         "row has " + std::to_string(fields.size()) +
                             " field(s) but the header declares " + std::to_string(header.size()) +
                             " (an unquoted comma in raw_line is the usual cause)",
                         line_number);
    }

    CsvRecord record;
    for (std::size_t index = 0; index < header.size(); ++index) {
        const std::string& column = header[index];
        const bool present = index < fields.size();
        const std::string value = present ? trim_copy(fields[index]) : std::string{};

        if (column == "timestamp_utc") {
            if (value.empty()) {
                return Error::in_field(ErrorCode::missing_required_field,
                                       "timestamp_utc is empty", column, line_number);
            }
            record.timestamp_utc = value;
        } else if (column == "raw_line") {
            record.raw_line = value;
        } else if (column == "elapsed_s" || column == "pitch_deg" || column == "roll_deg" ||
                   column == "x" || column == "y") {
            const bool optional_column = (column == "x" || column == "y");
            if (value.empty()) {
                if (optional_column) {
                    continue;  // absent optional field: leave the optional empty
                }
                return Error::in_field(ErrorCode::missing_required_field, column + " is empty",
                                       column, line_number);
            }
            double parsed = 0.0;
            if (!parse_double(value, parsed)) {
                return Error::in_field(ErrorCode::not_numeric,
                                       column + " is not a number: '" + value + "'", column,
                                       line_number);
            }
            if (!std::isfinite(parsed)) {
                return Error::in_field(ErrorCode::not_finite, column + " is NaN or infinite",
                                       column, line_number);
            }
            if (column == "elapsed_s") {
                if (parsed < 0.0) {
                    return Error::in_field(ErrorCode::negative_elapsed,
                                           "elapsed_s is negative (" + value + ")", column,
                                           line_number);
                }
                record.elapsed_s = parsed;
            } else if (column == "pitch_deg") {
                record.sample.pitch_deg = parsed;
            } else if (column == "roll_deg") {
                record.sample.roll_deg = parsed;
            } else if (column == "x") {
                record.sample.x = parsed;
            } else {
                record.sample.y = parsed;
            }
        }
        // Unknown columns are ignored on purpose; validate() reports them.
    }

    if (record.sample.x.has_value() != record.sample.y.has_value()) {
        return Error::at(ErrorCode::incomplete_position_pair,
                         "experimental position requires both x and y, or neither", line_number);
    }
    return record;
}

ValidationReport CsvReader::validate(const std::string& path, std::size_t max_issues) {
    ValidationReport report;
    report.path = path;

    std::ifstream probe(path, std::ios::binary);
    if (!probe) {
        return report;  // file_exists stays false
    }

    probe.seekg(0, std::ios::end);
    const std::streamoff size = probe.tellg();
    probe.seekg(0, std::ios::beg);
    report.file_exists = true;
    report.size_bytes = size > 0 ? static_cast<std::size_t>(size) : 0;
    if (report.size_bytes == 0) {
        report.issues.push_back({ErrorCode::empty_file, 0, {}, "file is empty (0 bytes)"});
        return report;
    }

    auto add_issue = [&report, max_issues](RowIssue issue) {
        if (report.issues.size() < max_issues) {
            report.issues.push_back(std::move(issue));
        } else {
            ++report.truncated_issues;
        }
    };

    std::string line;
    std::size_t line_number = 0;
    bool header_seen = false;
    std::unordered_set<std::string> header_set;
    bool has_fatal_header = false;

    while (std::getline(probe, line)) {
        ++line_number;
        strip_bom(line);
        if (line_number == 1) {
            report.header = split_line(line);
            for (const std::string& column : report.header) {
                header_set.insert(column);
            }
            std::unordered_set<std::string> seen;
            for (const std::string& column : report.header) {
                if (!seen.insert(column).second) {
                    add_issue({ErrorCode::duplicate_column, 1, column,
                               "header repeats this column name"});
                }
            }
            for (const std::string& column : required_columns()) {
                if (header_set.find(column) == header_set.end()) {
                    add_issue({ErrorCode::missing_column, 1, column,
                               "required column is absent from the header"});
                    has_fatal_header = true;
                }
            }
            for (const std::string& column : report.header) {
                if (std::find(known_columns().begin(), known_columns().end(), column) ==
                    known_columns().end()) {
                    add_issue({ErrorCode::unknown_column, 1, column,
                               "unknown column is ignored"});
                }
            }
            header_seen = true;
            continue;
        }

        if (line.empty()) {
            continue;  // a blank trailing line is not a data row
        }
        ++report.total_rows;

        if (has_fatal_header) {
            ++report.invalid_rows;
            continue;
        }

        const Result<CsvRecord> parsed = parse_row(report.header, split_line(line), line_number);
        if (parsed) {
            ++report.valid_rows;
        } else {
            ++report.invalid_rows;
            add_issue({parsed.error().code, line_number, parsed.error().field,
                       parsed.error().message});
        }
    }

    if (!header_seen) {
        add_issue({ErrorCode::empty_file, 0, {}, "file has no header line"});
    } else if (report.total_rows == 0 && !has_fatal_header) {
        add_issue({ErrorCode::empty_file, 0, {}, "file contains a header but no data rows"});
    } else if (report.valid_rows == 0 && report.total_rows > 0) {
        add_issue({ErrorCode::missing_column, 0, {}, "every data row was rejected"});
    }
    return report;
}

Result<CsvLoadResult> CsvReader::load(const std::string& path, std::size_t max_issues) {
    CsvLoadResult result;
    result.report = validate(path, max_issues);

    if (!result.report.file_exists) {
        return Error(ErrorCode::file_not_found, "file not found: " + path);
    }
    if (result.report.size_bytes == 0) {
        return Error(ErrorCode::empty_file, "file is empty (0 bytes): " + path);
    }
    for (const RowIssue& issue : result.report.issues) {
        if (issue.code == ErrorCode::missing_column && issue.line == 1) {
            return Error::in_field(ErrorCode::missing_column,
                                   "CSV is missing required column '" + issue.column + "'",
                                   issue.column);
        }
    }
    if (!result.report.ok()) {
        return Error(ErrorCode::empty_file,
                     "no usable rows in " + path + " (" +
                         std::to_string(result.report.invalid_rows) + " rejected)");
    }

    std::ifstream stream(path);
    std::string line;
    std::size_t line_number = 0;
    bool header_read = false;
    std::vector<std::string> header;

    while (std::getline(stream, line)) {
        ++line_number;
        strip_bom(line);
        if (!header_read) {
            header = split_line(line);
            header_read = true;
            continue;
        }
        if (line.empty()) {
            continue;
        }
        Result<CsvRecord> parsed = parse_row(header, split_line(line), line_number);
        if (parsed) {
            result.records.push_back(std::move(parsed).value());
        } else {
            ++result.skipped_rows;
        }
    }

    if (result.records.empty()) {
        return Error(ErrorCode::empty_file, "no usable rows in " + path);
    }
    return result;
}

}  // namespace imu_lab
