"""Tests for the unified error taxonomy and exit-code mapping."""
from __future__ import annotations

import json
import unittest

from host.imu_lab.errors import (
    DataFormatError,
    EmptyDataError,
    ExitCode,
    ImuLabError,
    InputMissingError,
    SourceError,
    SourceUnavailableError,
    StageError,
    UsageError,
    exit_code_for,
    render_failure,
    translate,
)


class ExitCodeTests(unittest.TestCase):
    def test_exit_codes_are_stable_and_distinct(self) -> None:
        expected = {
            "OK": 0,
            "NO_DATA": 1,
            "USAGE": 2,
            "DATA_INVALID": 3,
            "INPUT_MISSING": 4,
            "SOURCE_UNAVAILABLE": 5,
            "INTERNAL": 10,
        }
        actual = {name: int(getattr(ExitCode, name)) for name in expected}
        self.assertEqual(actual, expected)


class ErrorHierarchyTests(unittest.TestCase):
    def test_every_error_is_an_imulab_error(self) -> None:
        for error in (
            UsageError("x"),
            InputMissingError("x"),
            DataFormatError("x"),
            EmptyDataError("x"),
            SourceUnavailableError("x"),
            StageError("acquire", ValueError("boom")),
        ):
            self.assertIsInstance(error, ImuLabError)
            self.assertIsInstance(error, Exception)

    def test_usage_and_format_errors_remain_value_errors(self) -> None:
        # Backwards compatibility: library code and older callers catch ValueError.
        self.assertIsInstance(UsageError("bad argument"), ValueError)
        self.assertIsInstance(DataFormatError("bad file"), ValueError)

    def test_source_error_alias_points_at_the_new_class(self) -> None:
        self.assertIs(SourceError, SourceUnavailableError)


class HelpersTests(unittest.TestCase):
    def test_for_path_sets_role_and_hints(self) -> None:
        error = InputMissingError.for_path("data/missing.csv", role="input CSV")
        self.assertIn("input CSV not found", error.message)
        self.assertIn("data/missing.csv", error.message)
        self.assertTrue(error.hints)
        self.assertEqual(error.context["role"], "input CSV")

    def test_for_serial_mentions_port_and_recovery_steps(self) -> None:
        error = SourceUnavailableError.for_serial("COM9", "Access is denied")
        self.assertIn("COM9", error.message)
        self.assertIn("Access is denied", error.message)
        joined = " ".join(error.hints)
        self.assertIn("simulated", joined)
        self.assertEqual(error.context["port"], "COM9")

    def test_for_missing_dependency_suggests_offline_fallback(self) -> None:
        error = SourceUnavailableError.for_missing_dependency("matplotlib", "No module named 'matplotlib'")
        self.assertIn("matplotlib", error.message)
        self.assertTrue(any("pip install" in hint for hint in error.hints))

    def test_empty_data_error_reports_skipped_rows(self) -> None:
        error = EmptyDataError.for_source("COM3", skipped_rows=7)
        self.assertEqual(error.exit_code, ExitCode.NO_DATA)
        self.assertEqual(error.context["skipped_rows"], 7)
        self.assertTrue(any("7 row(s)" in hint for hint in error.hints))

    def test_stage_error_preserves_cause_and_stage(self) -> None:
        cause = ValueError("CSV must contain columns: elapsed_s")
        error = StageError("validate", cause)
        self.assertEqual(error.stage, "validate")
        self.assertIn("validate", error.message)
        self.assertIs(error.__cause__, cause)
        # The wrapped DataFormatError keeps its own exit code.
        self.assertEqual(error.exit_code, ExitCode.DATA_INVALID)

    def test_render_includes_hints_block(self) -> None:
        error = InputMissingError.for_path("a.csv")
        text = error.render()
        self.assertIn("How to fix:", text)
        self.assertIn("- ", text)

    def test_as_dict_is_json_serialisable(self) -> None:
        error = DataFormatError("bad", hints=["fix it"], context={"row": 3})
        payload = error.as_dict()
        self.assertEqual(payload["error"], "DataFormatError")
        self.assertEqual(payload["exit_code"], int(ExitCode.DATA_INVALID))
        json.dumps(payload)  # must not raise


class TranslateTests(unittest.TestCase):
    def test_imulab_errors_pass_through_unchanged(self) -> None:
        original = UsageError("nope")
        self.assertIs(translate(original), original)

    def test_file_not_found_becomes_input_missing(self) -> None:
        error = translate(FileNotFoundError(2, "No such file", "data/x.csv"))
        self.assertIsInstance(error, InputMissingError)
        self.assertEqual(exit_code_for(FileNotFoundError(2, "x", "y")), int(ExitCode.INPUT_MISSING))

    def test_is_a_directory_error_becomes_data_format_error(self) -> None:
        error = translate(IsADirectoryError(21, "Is a directory", "data"))
        self.assertIsInstance(error, DataFormatError)

    def test_permission_error_is_classified(self) -> None:
        error = translate(PermissionError(13, "Permission denied", "out.csv"))
        self.assertIsInstance(error, ImuLabError)
        self.assertIn("permission denied", error.message.lower())

    def test_unicode_error_becomes_data_format_error(self) -> None:
        error = translate(UnicodeDecodeError("utf-8", b"\xff", 0, 1, "invalid start byte"))
        self.assertIsInstance(error, DataFormatError)

    def test_plain_value_error_becomes_data_format_error(self) -> None:
        error = translate(ValueError("bad number"))
        self.assertIsInstance(error, DataFormatError)
        self.assertEqual(error.exit_code, ExitCode.DATA_INVALID)

    def test_unknown_error_becomes_internal_error(self) -> None:
        error = translate(RuntimeError("kaboom"))
        self.assertEqual(error.exit_code, ExitCode.INTERNAL)
        self.assertIn("RuntimeError", error.message)

    def test_render_failure_prefixes_the_message(self) -> None:
        text = render_failure("error", FileNotFoundError(2, "No such file", "x"))
        self.assertTrue(text.startswith("error: "))


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
