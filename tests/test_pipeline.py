"""End-to-end tests for the one-command pipeline.

Everything here runs offline: ``--source simulated`` needs no hardware and no
serial port, which is exactly the property the project promises for P0.
"""
from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from host.imu_lab.errors import (
    DataFormatError,
    ExitCode,
    SourceUnavailableError,
    UsageError,
)
from host.pipeline import (
    ARTIFACT_ANALYSIS_JSON,
    ARTIFACT_LOG,
    ARTIFACT_NORMALIZED_CSV,
    ARTIFACT_PLOT_PNG,
    ARTIFACT_REPORT_MD,
    PipelineConfig,
    StageStatus,
    main,
    run_pipeline,
)

HEADER = "timestamp_utc,elapsed_s,pitch_deg,roll_deg,x,y,raw_line"


class PipelineTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def config(self, **overrides) -> PipelineConfig:
        base = dict(
            source="simulated",
            output_dir=self.tmp / "out",
            duration_s=1.0,
            hz=20,
            quiet=True,
        )
        base.update(overrides)
        return PipelineConfig(**base)

    def write(self, text: str, name: str = "log.csv") -> Path:
        path = self.tmp / name
        path.write_text(text, encoding="utf-8")
        return path


class SimulatedRunTests(PipelineTestCase):
    def test_simulated_run_succeeds_and_writes_every_artifact(self) -> None:
        result = run_pipeline(self.config())
        self.assertTrue(result.ok, msg=f"stages: {result.stages}")
        self.assertEqual(result.exit_code, int(ExitCode.OK))

        out = self.tmp / "out"
        for name in (ARTIFACT_NORMALIZED_CSV, ARTIFACT_ANALYSIS_JSON, ARTIFACT_REPORT_MD, ARTIFACT_LOG):
            self.assertTrue((out / name).is_file(), f"{name} was not written")
        self.assertEqual(result.outputs["normalized"], str(out / ARTIFACT_NORMALIZED_CSV))

    def test_stage_sequence_and_statuses(self) -> None:
        result = run_pipeline(self.config())
        names = [stage.name for stage in result.stages]
        self.assertEqual(
            names, ["input-check", "acquire", "validate", "analyze", "plot", "report"]
        )
        by_name = {stage.name: stage for stage in result.stages}
        self.assertEqual(by_name["input-check"].status, StageStatus.SKIPPED)
        for name in ("acquire", "validate", "analyze", "report"):
            self.assertEqual(by_name[name].status, StageStatus.OK, name)
        # plot is either OK (matplotlib present) or gracefully SKIPPED.
        self.assertIn(by_name["plot"].status, {StageStatus.OK, StageStatus.SKIPPED})
        for stage in result.stages:
            self.assertGreaterEqual(stage.duration_ms, 0.0)

    def test_normalised_csv_has_the_standard_schema(self) -> None:
        run_pipeline(self.config(duration_s=1.0, hz=10))
        text = (self.tmp / "out" / ARTIFACT_NORMALIZED_CSV).read_text(encoding="utf-8")
        header, *rows = text.splitlines()
        self.assertEqual(
            header, "timestamp_utc,elapsed_s,pitch_deg,roll_deg,x,y,raw_line"
        )
        self.assertEqual(len(rows), 10)

    def test_analysis_json_is_valid_and_marks_simulated_data(self) -> None:
        run_pipeline(self.config())
        payload = json.loads((self.tmp / "out" / ARTIFACT_ANALYSIS_JSON).read_text(encoding="utf-8"))
        self.assertEqual(payload["source"], "simulated")
        self.assertEqual(payload["data_kind"], "simulated")
        stats = payload["statistics"]
        self.assertEqual(stats["sample_count"], 20)
        self.assertIn("pitch", stats)
        self.assertIn("roll", stats)

    def test_report_warns_that_the_data_is_synthetic(self) -> None:
        run_pipeline(self.config())
        report = (self.tmp / "out" / ARTIFACT_REPORT_MD).read_text(encoding="utf-8")
        self.assertIn("## Pipeline summary", report)
        self.assertIn("## Input validation", report)
        self.assertIn("synthetic", report.lower())

    def test_pipeline_log_is_json_lines(self) -> None:
        run_pipeline(self.config())
        lines = (self.tmp / "out" / ARTIFACT_LOG).read_text(encoding="utf-8").splitlines()
        events = [json.loads(line) for line in lines]
        kinds = [event["event"] for event in events]
        self.assertIn("pipeline_start", kinds)
        self.assertIn("stage_start", kinds)
        self.assertIn("pipeline_end", kinds)
        self.assertTrue(events[-1]["ok"])

    def test_plot_png_is_produced_when_matplotlib_is_available(self) -> None:
        result = run_pipeline(self.config())
        if "plot" not in result.outputs:
            self.skipTest("matplotlib is not installed in this interpreter")
        self.assertTrue((self.tmp / "out" / ARTIFACT_PLOT_PNG).is_file())


class CsvSourceTests(PipelineTestCase):
    def _good_csv(self) -> Path:
        rows = [HEADER]
        for index in range(10):
            elapsed = index * 0.05
            rows.append(
                f'2026-01-01T00:00:00.{index:03d}Z,{elapsed:.6f},'
                f'{index * 0.5:.3f},{index * 0.25:.3f},,,"P:{index},R:{index}"'
            )
        return self.write("\n".join(rows) + "\n", "good.csv")

    def test_csv_source_runs_the_input_check_stage(self) -> None:
        path = self._good_csv()
        result = run_pipeline(self.config(source="csv", input_path=path))
        self.assertTrue(result.ok, msg=str([s.as_dict() for s in result.stages]))
        by_name = {stage.name: stage for stage in result.stages}
        self.assertEqual(by_name["input-check"].status, StageStatus.OK)
        self.assertIn("10/10 rows valid", by_name["input-check"].detail)

    def test_csv_source_keeps_partial_data_by_default(self) -> None:
        path = self.write("\n".join([
            HEADER,
            "2026-01-01T00:00:00Z,0.0,1,2,,,",
            "2026-01-01T00:00:00Z,not-a-number,1,2,,,",
            "2026-01-01T00:00:00Z,0.2,3,4,,,",
            "",
        ]), "partial.csv")
        result = run_pipeline(self.config(source="csv", input_path=path))
        self.assertTrue(result.ok)
        self.assertEqual(result.metrics["validation"]["valid_rows"], 2)

    def test_csv_source_fails_fast_in_strict_mode(self) -> None:
        path = self.write("\n".join([
            HEADER,
            "2026-01-01T00:00:00Z,0.0,1,2,,,",
            "2026-01-01T00:00:00Z,oops,1,2,,,",
            "",
        ]), "strict.csv")
        result = run_pipeline(self.config(source="csv", input_path=path, allow_partial=False))
        self.assertFalse(result.ok)
        self.assertEqual(result.failed_stage.name, "input-check")
        self.assertEqual(result.exit_code, int(ExitCode.DATA_INVALID))

    def test_missing_input_file_uses_the_input_missing_exit_code(self) -> None:
        result = run_pipeline(self.config(source="csv", input_path=self.tmp / "nope.csv"))
        self.assertFalse(result.ok)
        self.assertEqual(result.exit_code, int(ExitCode.INPUT_MISSING))

    def test_empty_input_file_uses_the_no_data_exit_code(self) -> None:
        path = self.write("", "empty.csv")
        result = run_pipeline(self.config(source="csv", input_path=path))
        self.assertFalse(result.ok)
        self.assertEqual(result.exit_code, int(ExitCode.NO_DATA))

    def test_wrong_columns_use_the_data_invalid_exit_code(self) -> None:
        path = self.write("a,b,c\n1,2,3\n", "wrong.csv")
        result = run_pipeline(self.config(source="csv", input_path=path))
        self.assertFalse(result.ok)
        self.assertEqual(result.exit_code, int(ExitCode.DATA_INVALID))
        self.assertEqual(result.failed_stage.name, "input-check")

    def test_input_equal_to_output_is_refused(self) -> None:
        # Point --input straight at the file the pipeline would write.
        out_dir = self.tmp / "collide"
        out_dir.mkdir()
        victim = out_dir / ARTIFACT_NORMALIZED_CSV
        victim.write_text(HEADER + "\n2026-01-01T00:00:00Z,0,1,2,,,\n", encoding="utf-8")
        with self.assertRaises(DataFormatError) as ctx:
            run_pipeline(self.config(source="csv", input_path=victim, output_dir=out_dir))
        self.assertIn("must differ", ctx.exception.message)
        # The original file must still be intact.
        self.assertIn(HEADER, victim.read_text(encoding="utf-8"))

    def test_csv_source_without_input_path_raises_usage_error(self) -> None:
        with self.assertRaises(UsageError):
            run_pipeline(self.config(source="csv", input_path=None))


class ErrorClassificationTests(PipelineTestCase):
    def test_unsupported_source_is_a_usage_error(self) -> None:
        with self.assertRaises(UsageError):
            run_pipeline(self.config(source="banana"))

    def test_serial_source_without_pyserial_is_classified(self) -> None:
        try:
            import serial  # noqa: F401
        except ImportError:
            result = run_pipeline(self.config(source="serial", duration_s=0.1, port="COM_DOES_NOT_EXIST"))
            self.assertFalse(result.ok)
            self.assertEqual(result.exit_code, int(ExitCode.SOURCE_UNAVAILABLE))
            return
        # pyserial is installed: pointing at a bogus port must still be a
        # classified source failure rather than a traceback.
        result = run_pipeline(self.config(source="serial", duration_s=0.1, port="COM_DOES_NOT_EXIST_9X"))
        self.assertFalse(result.ok)
        self.assertEqual(result.exit_code, int(ExitCode.SOURCE_UNAVAILABLE))
        self.assertIsInstance(SourceUnavailableError("x"), Exception)

    def test_bad_threshold_is_a_usage_error(self) -> None:
        with self.assertRaises(UsageError):
            run_pipeline(self.config(jump_threshold_deg=0))

    def test_bad_duration_is_a_usage_error(self) -> None:
        with self.assertRaises(UsageError):
            run_pipeline(self.config(duration_s=0))


class CommandLineTests(PipelineTestCase):
    def test_main_runs_simulated_pipeline(self) -> None:
        code = main([
            "--source", "simulated",
            "--duration", "1",
            "--hz", "10",
            "--output-dir", str(self.tmp / "cli"),
            "--quiet",
        ])
        self.assertEqual(code, int(ExitCode.OK))
        self.assertTrue((self.tmp / "cli" / ARTIFACT_REPORT_MD).is_file())

    def test_main_defaults_to_the_run_subcommand(self) -> None:
        code = main(["--output-dir", str(self.tmp / "cli2"), "--duration", "1", "--quiet"])
        self.assertEqual(code, int(ExitCode.OK))

    def test_validate_subcommand_reports_usable_file(self) -> None:
        path = self.write(HEADER + "\n2026-01-01T00:00:00Z,0,1,2,,,\n")
        self.assertEqual(main(["validate", str(path)]), 0)

    def test_validate_subcommand_reports_bad_file(self) -> None:
        path = self.write("a,b,c\n1,2,3\n", "bad.csv")
        self.assertEqual(main(["validate", str(path)]), int(ExitCode.DATA_INVALID))

    def test_validate_subcommand_supports_json(self) -> None:
        path = self.write(HEADER + "\n2026-01-01T00:00:00Z,0,1,2,,,\n")
        self.assertEqual(main(["validate", str(path), "--json"]), 0)

    def test_validate_subcommand_missing_file(self) -> None:
        self.assertEqual(
            main(["validate", str(self.tmp / "ghost.csv")]), int(ExitCode.INPUT_MISSING)
        )

    def test_cli_csv_without_input_is_a_usage_error(self) -> None:
        self.assertEqual(main(["--source", "csv", "--quiet"]), int(ExitCode.USAGE))


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
