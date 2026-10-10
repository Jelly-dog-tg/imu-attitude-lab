"""Tests for input validation: missing fields, bad numbers, empty and odd files."""
from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from host.imu_lab.errors import DataFormatError, EmptyDataError, InputMissingError, UsageError
from host.imu_lab.validation import (
    REQUIRED_COLUMNS,
    Severity,
    ensure_valid,
    iter_valid_rows,
    validate_csv,
)

HEADER = "timestamp_utc,elapsed_s,pitch_deg,roll_deg,x,y,raw_line"


def _row(elapsed: str, pitch: str, roll: str, *, x: str = "", y: str = "", raw: str = "") -> str:
    quoted = f'"{raw}"' if raw else ""
    return f"2026-01-01T00:00:00.000Z,{elapsed},{pitch},{roll},{x},{y},{quoted}"


class ValidationTestCase(unittest.TestCase):
    """Base class providing a scratch directory."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def write(self, text: str, name: str = "log.csv") -> Path:
        path = self.tmp / name
        path.write_text(text, encoding="utf-8")
        return path


class CleanFileTests(ValidationTestCase):
    def test_valid_file_reports_usable(self) -> None:
        path = self.write("\n".join([
            HEADER,
            _row("0.000000", "1.0", "2.0", raw="P:1.000,R:2.000"),
            _row("0.050000", "1.5", "2.5", raw="P:1.500,R:2.500"),
            "",
        ]))
        report = validate_csv(path)
        self.assertTrue(report.ok)
        self.assertEqual(report.total_rows, 2)
        self.assertEqual(report.valid_rows, 2)
        self.assertEqual(report.invalid_rows, 0)
        self.assertEqual(report.issues, ())
        self.assertEqual(list(report.header[:4]), list(REQUIRED_COLUMNS))
        self.assertAlmostEqual(report.invalid_ratio, 0.0)
        self.assertIs(ensure_valid(report), report)

    def test_bom_prefixed_header_is_accepted(self) -> None:
        path = self.tmp / "bom.csv"
        path.write_text("\ufeff" + HEADER + "\n" + _row("0", "1", "2") + "\n", encoding="utf-8")
        self.assertTrue(validate_csv(path).ok)


class MalformedRowTests(ValidationTestCase):
    def test_each_bad_row_is_reported_with_a_code(self) -> None:
        path = self.write("\n".join([
            HEADER,
            _row("0.000000", "1.0", "2.0"),          # ok
            _row("abc", "1.5", "2.5"),               # not numeric
            _row("-1", "3.0", "4.0"),                # negative elapsed
            _row("0.15", "", "4.5"),                 # empty field
            _row("0.20", "5.0", "nan"),              # not finite
            "2026-01-01T00:00:00.250Z,0.25",         # short row
            "",
        ]))
        report = validate_csv(path)
        codes = [issue.code for issue in report.error_issues]
        self.assertEqual(report.total_rows, 6)
        self.assertEqual(report.valid_rows, 1)
        self.assertEqual(report.invalid_rows, 5)
        for expected in ("not_numeric", "negative_elapsed", "empty_field", "not_finite", "missing_column_in_row"):
            self.assertIn(expected, codes, f"missing code {expected} in {codes}")
        # Findings are located precisely.
        not_numeric = next(i for i in report.error_issues if i.code == "not_numeric")
        self.assertEqual(not_numeric.row_number, 3)
        self.assertEqual(not_numeric.field, "elapsed_s")
        self.assertEqual(not_numeric.value, "abc")

    def test_unquoted_comma_in_raw_line_is_flagged(self) -> None:
        path = self.write("\n".join([HEADER, "2026-01-01T00:00:00Z,0,1,2,,,P:1,R:2", ""]))
        report = validate_csv(path)
        self.assertIn("too_many_fields", [i.code for i in report.error_issues])

    def test_x_without_y_is_rejected(self) -> None:
        path = self.write("\n".join([HEADER, _row("0", "1", "2", x="0.5"), ""]))
        report = validate_csv(path)
        self.assertIn("incomplete_position_pair", [i.code for i in report.error_issues])

    def test_x_and_y_together_are_accepted(self) -> None:
        path = self.write("\n".join([HEADER, _row("0", "1", "2", x="0.5", y="0.25"), ""]))
        self.assertTrue(validate_csv(path).ok)

    def test_non_monotonic_and_duplicate_time_are_warnings_not_errors(self) -> None:
        path = self.write("\n".join([
            HEADER,
            _row("0.10", "1", "2"),
            _row("0.05", "1", "2"),   # goes backwards
            _row("0.50", "1", "2"),
            _row("0.50", "1", "2"),   # duplicate
            "",
        ]))
        report = validate_csv(path)
        self.assertTrue(report.ok)
        codes = [i.code for i in report.warning_issues]
        self.assertIn("time_not_monotonic", codes)
        self.assertIn("duplicate_timestamp", codes)
        self.assertEqual(report.invalid_rows, 0)

    def test_partially_bad_file_is_still_usable(self) -> None:
        path = self.write("\n".join([
            HEADER,
            _row("0.0", "1", "2"),
            _row("oops", "1", "2"),
            _row("0.2", "3", "4"),
            "",
        ]))
        report = validate_csv(path)
        self.assertTrue(report.ok)
        self.assertEqual(report.valid_rows, 2)
        self.assertEqual(report.invalid_rows, 1)


class StructuralProblemTests(ValidationTestCase):
    def test_missing_file(self) -> None:
        report = validate_csv(self.tmp / "nope.csv")
        self.assertFalse(report.file_exists)
        self.assertFalse(report.ok)
        self.assertEqual(report.fatal_issues[0].code, "file_missing")
        with self.assertRaises(InputMissingError) as ctx:
            ensure_valid(report)
        self.assertEqual(ctx.exception.exit_code.value, 4)

    def test_empty_file(self) -> None:
        path = self.write("", "empty.csv")
        report = validate_csv(path)
        self.assertEqual(report.fatal_issues[0].code, "empty_file")
        with self.assertRaises(EmptyDataError):
            ensure_valid(report)

    def test_header_only_file(self) -> None:
        path = self.write(HEADER + "\n", "header_only.csv")
        report = validate_csv(path)
        self.assertEqual(report.total_rows, 0)
        self.assertIn("no_data_rows", [i.code for i in report.fatal_issues])
        with self.assertRaises(EmptyDataError):
            ensure_valid(report)

    def test_wrong_columns_is_a_data_format_error(self) -> None:
        path = self.write("a,b,c\n1,2,3\n", "wrong.csv")
        report = validate_csv(path)
        self.assertIn("missing_columns", [i.code for i in report.fatal_issues])
        with self.assertRaises(DataFormatError) as ctx:
            ensure_valid(report)
        self.assertEqual(ctx.exception.exit_code.value, 3)

    def test_unknown_extra_column_is_only_a_warning(self) -> None:
        path = self.write(
            "timestamp_utc,elapsed_s,pitch_deg,roll_deg,temperature_c\n"
            "2026-01-01T00:00:00Z,0,1,2,25\n",
            "extra.csv",
        )
        report = validate_csv(path)
        self.assertTrue(report.ok)
        self.assertIn("unknown_columns", [i.code for i in report.warning_issues])

    def test_directory_path_is_reported(self) -> None:
        report = validate_csv(self.tmp)
        self.assertEqual(report.fatal_issues[0].code, "not_a_file")

    def test_all_rows_rejected_is_fatal(self) -> None:
        path = self.write("\n".join([HEADER, _row("x", "1", "2"), _row("y", "1", "2"), ""]))
        report = validate_csv(path)
        self.assertFalse(report.ok)
        self.assertIn("no_valid_rows", [i.code for i in report.fatal_issues])
        with self.assertRaises(EmptyDataError):
            ensure_valid(report)


class IssueCapTests(ValidationTestCase):
    def test_issue_list_is_capped_but_counts_stay_correct(self) -> None:
        rows = [HEADER] + [_row("bad", "1", "2") for _ in range(50)]
        path = self.write("\n".join(rows) + "\n")
        report = validate_csv(path, max_issues=5)
        self.assertEqual(len(report.issues), 5)
        self.assertGreater(report.truncated_issues, 0)
        self.assertEqual(report.invalid_rows, 50)
        self.assertTrue(report.exceeds_issue_cap)

    def test_max_rows_limits_scanning(self) -> None:
        rows = [HEADER] + [_row(str(i / 100), "1", "2") for i in range(20)]
        path = self.write("\n".join(rows) + "\n")
        report = validate_csv(path, max_rows=5)
        self.assertEqual(report.total_rows, 5)
        self.assertIn("row_limit_reached", [i.code for i in report.warning_issues])

    def test_invalid_arguments_raise_usage_error(self) -> None:
        path = self.write(HEADER + "\n" + _row("0", "1", "2") + "\n")
        with self.assertRaises(UsageError):
            validate_csv(path, max_issues=0)
        with self.assertRaises(UsageError):
            validate_csv(path, max_rows=0)


class StrictModeTests(ValidationTestCase):
    def test_strict_mode_rejects_warnings(self) -> None:
        path = self.write("\n".join([
            HEADER, _row("0.1", "1", "2"), _row("0.1", "1", "2"), "",
        ]))
        report = validate_csv(path)
        self.assertTrue(report.ok)
        with self.assertRaises(DataFormatError):
            ensure_valid(report, allow_warnings=False)


class ReportingTests(ValidationTestCase):
    def test_summary_and_dict_are_consistent(self) -> None:
        path = self.write("\n".join([HEADER, _row("0", "1", "2"), _row("bad", "1", "2"), ""]))
        report = validate_csv(path)
        summary = report.summary()
        self.assertEqual(summary["total_rows"], 2)
        self.assertEqual(summary["valid_rows"], 1)
        self.assertEqual(summary["invalid_rows"], 1)
        payload = report.as_dict()
        self.assertEqual(len(payload["issues"]), len(report.issues))

    def test_render_mentions_verdict(self) -> None:
        path = self.write(HEADER + "\n" + _row("0", "1", "2") + "\n")
        self.assertIn("USABLE", validate_csv(path).render())
        bad = self.write("x\n1\n", "bad.csv")
        self.assertIn("NOT USABLE", validate_csv(bad).render())

    def test_severity_values_are_strings(self) -> None:
        self.assertEqual(Severity.WARNING.value, "warning")
        self.assertEqual(str(Severity.ERROR), "error")


class IterValidRowsTests(ValidationTestCase):
    def test_yields_only_valid_rows_with_line_numbers(self) -> None:
        path = self.write("\n".join([
            HEADER,
            _row("0.0", "1", "2"),
            _row("bad", "1", "2"),
            _row("0.2", "3", "4"),
            "",
        ]))
        rows = list(iter_valid_rows(path))
        self.assertEqual([number for number, _ in rows], [2, 4])
        self.assertAlmostEqual(rows[0][1]["pitch_deg"], 1.0)
        self.assertAlmostEqual(rows[1][1]["roll_deg"], 4.0)


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
