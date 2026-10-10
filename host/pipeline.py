"""One-command experiment pipeline for IMU Attitude Lab.

``python -m host.pipeline`` turns a single invocation into a complete, auditable
experiment run:

    input-check -> acquire -> validate -> analyze -> plot -> report

Each stage is timed, logged to ``pipeline.log`` (JSON lines) and reported to the
console. Failures are classified through :mod:`host.imu_lab.errors`, so the
process exits with a stable code that scripts and CI can branch on.

Examples
--------
Fully offline demo (no hardware, no serial port needed)::

    python -m host.pipeline --output-dir reports/demo

Re-analyse an existing experiment log::

    python -m host.pipeline --source csv --input data/normalized.csv --output-dir reports/run2

Diagnose a suspect file without running the pipeline::

    python -m host.pipeline validate data/normalized.csv
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import json
from pathlib import Path
import sys
import time

from host.imu_lab.csvlog import AttitudeCsvWriter, load_csv
from host.imu_lab.errors import (
    DataFormatError,
    EmptyDataError,
    ExitCode,
    ImuLabError,
    UsageError,
    render_failure,
    translate,
)
from host.imu_lab.plotting import PlottingUnavailableError, create_plot
from host.imu_lab.reporting import (
    DATA_KINDS,
    ExperimentSummary,
    analyze_records,
    render_markdown,
    resolve_data_kind,
)
from host.imu_lab.sources import SourceStats, iter_source_samples
from host.imu_lab.validation import ValidationReport, ensure_valid, validate_csv

#: Filenames written into the output directory.
ARTIFACT_NORMALIZED_CSV = "normalized.csv"
ARTIFACT_ANALYSIS_JSON = "analysis.json"
ARTIFACT_PLOT_PNG = "attitude_plot.png"
ARTIFACT_REPORT_MD = "report.md"
ARTIFACT_LOG = "pipeline.log"

STAGE_INPUT_CHECK = "input-check"
STAGE_ACQUIRE = "acquire"
STAGE_VALIDATE = "validate"
STAGE_ANALYZE = "analyze"
STAGE_PLOT = "plot"
STAGE_REPORT = "report"


class StageStatus:
    """String constants describing how a stage finished.

    Kept as plain strings (rather than an ``Enum``) because they are written
    straight into the Markdown/JSON artifacts.
    """

    OK = "ok"
    SKIPPED = "skipped"
    FAILED = "failed"


# --------------------------------------------------------------------------- #
# Configuration and results
# --------------------------------------------------------------------------- #
@dataclass(frozen=True)
class PipelineConfig:
    """Everything needed to reproduce one pipeline run."""

    source: str = "simulated"
    input_path: Path | None = None
    output_dir: Path = Path("reports/latest")
    duration_s: float = 5.0
    hz: int = 20
    seed: int = 7
    port: str = "COM3"
    baud: int = 9600
    jump_threshold_deg: float = 30.0
    data_kind: str = "auto"
    allow_partial: bool = True
    require_plot: bool = False
    quiet: bool = False
    log_limit: int = 200

    def as_dict(self) -> dict[str, object]:
        return {
            "source": self.source,
            "input_path": str(self.input_path) if self.input_path else None,
            "output_dir": str(self.output_dir),
            "duration_s": self.duration_s,
            "hz": self.hz,
            "seed": self.seed,
            "port": self.port,
            "baud": self.baud,
            "jump_threshold_deg": self.jump_threshold_deg,
            "data_kind": self.data_kind,
            "allow_partial": self.allow_partial,
            "require_plot": self.require_plot,
        }


@dataclass
class StageResult:
    """Outcome of a single pipeline stage."""

    name: str
    status: str
    duration_ms: float = 0.0
    detail: str = ""
    error: str | None = None
    #: Exit code dictated by the failure that ended the stage. ``None`` means
    #: "not a failure" or "let the runner fall back to its per-stage default".
    exit_code: int | None = None

    @property
    def ok(self) -> bool:
        return self.status != StageStatus.FAILED

    def as_dict(self) -> dict[str, object]:
        payload = {
            "stage": self.name,
            "status": self.status,
            "duration_ms": round(self.duration_ms, 3),
            "detail": self.detail,
        }
        if self.error:
            payload["error"] = self.error
        if self.exit_code is not None:
            payload["exit_code"] = self.exit_code
        return payload


@dataclass
class PipelineResult:
    """Outcome of a whole pipeline run."""

    config: PipelineConfig
    stages: list[StageResult] = field(default_factory=list)
    outputs: dict[str, str] = field(default_factory=dict)
    metrics: dict[str, object] = field(default_factory=dict)
    exit_code: int = int(ExitCode.OK)

    @property
    def ok(self) -> bool:
        return self.exit_code == int(ExitCode.OK)

    @property
    def failed_stage(self) -> StageResult | None:
        for stage in self.stages:
            if stage.status == StageStatus.FAILED:
                return stage
        return None

    def as_dict(self) -> dict[str, object]:
        return {
            "config": self.config.as_dict(),
            "stages": [s.as_dict() for s in self.stages],
            "outputs": dict(self.outputs),
            "metrics": dict(self.metrics),
            "ok": self.ok,
            "exit_code": self.exit_code,
        }

    def render(self) -> str:
        lines = ["Pipeline stages:"]
        for stage in self.stages:
            marker = {"ok": "OK  ", "skipped": "SKIP", "failed": "FAIL"}[stage.status]
            detail = f" - {stage.detail}" if stage.detail else ""
            lines.append(f"  [{marker}] {stage.name:<12} {stage.duration_ms:7.1f} ms{detail}")
            if stage.error:
                lines.append(f"         {stage.error}")
        if self.outputs:
            lines.append("Artifacts:")
            for name, path in self.outputs.items():
                lines.append(f"  {name:<12} {path}")
        lines.append(f"Result: {'SUCCESS' if self.ok else 'FAILED'} (exit {self.exit_code})")
        return "\n".join(lines)


# --------------------------------------------------------------------------- #
# Runner
# --------------------------------------------------------------------------- #
class PipelineRunner:
    """Executes the fixed stage sequence and records what happened."""

    def __init__(self, config: PipelineConfig) -> None:
        self.config = config
        self.output_dir = Path(config.output_dir)
        self.result = PipelineResult(config=config)
        self._log_lines: list[dict[str, object]] = []
        self._validation: ValidationReport | None = None
        self._input_validation: ValidationReport | None = None
        self._normalized_path = self.output_dir / ARTIFACT_NORMALIZED_CSV
        self._stats = SourceStats()
        self._acquired = 0
        self._summary: ExperimentSummary | None = None
        self._records: list = []
        self._data_kind: str = "unknown"
        #: Exit code to use when a stage fails, so a failure is classified even
        #: when the stage never got far enough to refine it itself.
        self._stage_exit_codes: dict[str, int] = {
            STAGE_INPUT_CHECK: int(ExitCode.DATA_INVALID),
            STAGE_ACQUIRE: int(ExitCode.SOURCE_UNAVAILABLE),
            STAGE_VALIDATE: int(ExitCode.DATA_INVALID),
            STAGE_ANALYZE: int(ExitCode.DATA_INVALID),
            STAGE_PLOT: int(ExitCode.SOURCE_UNAVAILABLE),
            STAGE_REPORT: int(ExitCode.INTERNAL),
        }

    # -- public ---------------------------------------------------------- #
    def run(self) -> PipelineResult:
        self._preflight()
        self.output_dir.mkdir(parents=True, exist_ok=True)

        self._log({"event": "pipeline_start", "config": self.config.as_dict()})

        steps = (
            (STAGE_INPUT_CHECK, self._stage_input_check),
            (STAGE_ACQUIRE, self._stage_acquire),
            (STAGE_VALIDATE, self._stage_validate),
            (STAGE_ANALYZE, self._stage_analyze),
            (STAGE_PLOT, self._stage_plot),
            (STAGE_REPORT, self._stage_report),
        )

        for name, function in steps:
            outcome = self._run_stage(name, function)
            if outcome.status == StageStatus.FAILED:
                self.result.exit_code = self._exit_code_from(outcome)
                break

        self._log({"event": "pipeline_end", "ok": self.result.ok, "exit_code": self.result.exit_code})
        self._write_log()
        return self.result

    # -- preflight ------------------------------------------------------- #
    def _preflight(self) -> None:
        config = self.config
        if config.source not in {"simulated", "csv", "serial"}:
            raise UsageError(
                f"unsupported source: {config.source!r}",
                hints=["choose one of: simulated, csv, serial"],
            )
        if config.source == "csv":
            if config.input_path is None:
                raise UsageError("--input is required when --source csv")
            input_path = Path(config.input_path)
            if input_path.exists() and input_path.is_dir():
                raise DataFormatError(
                    f"--input must be a file, not a directory: {input_path}",
                    hints=["pass a CSV file such as data/normalized.csv"],
                )
            if input_path.resolve() == self._normalized_path.resolve():
                raise DataFormatError(
                    "input and output must differ",
                    hints=[f"--input points at the pipeline output {self._normalized_path}",
                           "choose a different --output-dir"],
                )
        if config.jump_threshold_deg <= 0:
            raise UsageError("--jump-threshold-deg must be greater than zero")
        if config.source in {"simulated", "serial"} and config.duration_s <= 0:
            raise UsageError("--duration must be greater than zero")
        if config.source == "simulated" and config.hz <= 0:
            raise UsageError("--hz must be greater than zero")
        if config.source == "serial" and config.baud <= 0:
            raise UsageError("--baud must be greater than zero")

    # -- stage plumbing -------------------------------------------------- #
    def _run_stage(self, name: str, function) -> StageResult:
        started = time.perf_counter()
        self._log({"event": "stage_start", "stage": name})
        try:
            detail = function() or ""
            stage = StageResult(
                name=name,
                status=StageStatus.OK,
                duration_ms=(time.perf_counter() - started) * 1000.0,
                detail=detail,
            )
        except _SkipStage as skip:
            stage = StageResult(
                name=name,
                status=StageStatus.SKIPPED,
                duration_ms=(time.perf_counter() - started) * 1000.0,
                detail=skip.reason,
            )
        except KeyboardInterrupt:
            failure = ImuLabError(
                f"stage {name!r} interrupted by the user (Ctrl+C)",
                hints=["partial artifacts were left in the output directory"],
            )
            stage = StageResult(
                name=name,
                status=StageStatus.FAILED,
                duration_ms=(time.perf_counter() - started) * 1000.0,
                detail=failure.message,
            )
            self._log({"event": "stage_failed", "stage": name, "error": failure.as_dict()})
            if not self.config.quiet:
                print(render_failure(f"error in stage {name!r}", failure), file=sys.stderr)
            self.result.stages.append(stage)
            self.result.exit_code = int(ExitCode.NO_DATA)
            return stage
        except BaseException as exc:  # noqa: BLE001 - converted below
            failure = translate(exc)
            stage = StageResult(
                name=name,
                status=StageStatus.FAILED,
                duration_ms=(time.perf_counter() - started) * 1000.0,
                detail=failure.message,
                error="\n".join(failure.hints),
                exit_code=int(failure.exit_code),
            )
            self._log({"event": "stage_failed", "stage": name, "error": failure.as_dict()})
            if not self.config.quiet:
                print(render_failure(f"error in stage {name!r}", failure), file=sys.stderr)
            self.result.stages.append(stage)
            return stage

        self.result.stages.append(stage)
        self._log({"event": "stage_end", **stage.as_dict()})
        if not self.config.quiet:
            print(f"  [{stage.status:<7}] {stage.name:<12} {stage.duration_ms:7.1f} ms"
                  f"{' - ' + stage.detail if stage.detail else ''}")
        return stage

    def _exit_code_from(self, stage: StageResult) -> int:
        """Prefer the code implied by the error itself over the per-stage guess."""
        if stage.exit_code is not None:
            return stage.exit_code
        match = self._stage_exit_codes.get(stage.name)
        return int(match) if match is not None else int(ExitCode.INTERNAL)

    # -- stages ---------------------------------------------------------- #
    def _stage_input_check(self) -> str:
        if self.config.source != "csv":
            raise _SkipStage(f"not needed for source={self.config.source}")

        input_path = Path(self.config.input_path)  # type: ignore[arg-type]
        report = validate_csv(input_path, max_issues=self.config.log_limit)
        self._input_validation = report

        if not report.ok:
            ensure_valid(report)  # raises with a precise message
        if not self.config.allow_partial and report.invalid_rows:
            raise DataFormatError(
                f"{input_path}: {report.invalid_rows} malformed row(s) found",
                hints=[
                    "re-run with --allow-partial to keep only the valid rows",
                    f"run `python -m host.pipeline validate {input_path}` for the full list",
                ],
                context={"invalid_rows": report.invalid_rows, "total_rows": report.total_rows},
            )
        # Only reached when the input is usable, so this is a genuine fallback.
        self._stage_exit_codes[STAGE_INPUT_CHECK] = _exit_code_for_report(report)
        return (
            f"{report.valid_rows}/{report.total_rows} rows valid, "
            f"{len(report.warning_issues)} warning(s)"
        )

    def _stage_acquire(self) -> str:
        config = self.config
        self._stage_exit_codes[STAGE_ACQUIRE] = int(ExitCode.SOURCE_UNAVAILABLE)

        stream = iter_source_samples(
            config.source,  # type: ignore[arg-type]
            input_path=config.input_path,
            port=config.port,
            baud=config.baud,
            duration_s=config.duration_s,
            hz=config.hz,
            seed=config.seed,
            stats=self._stats,
        )
        written = 0
        try:
            with AttitudeCsvWriter(self._normalized_path) as writer:
                for record in stream:
                    writer.write(
                        record.sample,
                        record.raw_line,
                        elapsed_s=record.elapsed_s,
                        timestamp_utc=record.timestamp_utc,
                    )
                    written += 1
                    if written > 5_000_000:  # guard against an endless serial feed
                        break
        except KeyboardInterrupt:
            raise EmptyDataError.for_source(config.source, skipped_rows=self._stats.skipped_csv_rows)

        self._acquired = written
        if written == 0:
            raise EmptyDataError.for_source(
                config.source, skipped_rows=self._stats.skipped_csv_rows
            )
        self.result.outputs["normalized"] = str(self._normalized_path)
        return (
            f"{written} samples from {config.source}, "
            f"{self._stats.skipped_csv_rows} csv row(s) dropped, "
            f"{self._stats.invalid_frames} invalid frame(s)"
        )

    def _stage_validate(self) -> str:
        report = validate_csv(self._normalized_path, max_issues=self.config.log_limit)
        self._validation = report
        self._stage_exit_codes[STAGE_VALIDATE] = _exit_code_for_report(report)
        if not report.ok:
            ensure_valid(report)
        self.result.metrics["validation"] = report.summary()
        return f"{report.valid_rows} valid row(s), {report.invalid_rows} rejected"

    def _stage_analyze(self) -> str:
        records, skipped = load_csv(self._normalized_path)
        if not records:
            raise EmptyDataError.for_source(self._normalized_path, skipped_rows=skipped)

        summary = analyze_records(
            records,
            skipped_rows=skipped,
            jump_threshold_deg=self.config.jump_threshold_deg,
        )
        self._summary = summary
        self._records = records
        self._data_kind = self._resolve_data_kind()

        payload = self._analysis_payload(summary)
        analysis_path = self.output_dir / ARTIFACT_ANALYSIS_JSON
        analysis_path.write_text(
            json.dumps(payload, indent=2, ensure_ascii=False), encoding="utf-8"
        )
        self.result.outputs["analysis"] = str(analysis_path)
        self.result.metrics["analysis"] = payload["statistics"]
        return (
            f"{summary.sample_count} samples, {summary.duration_s:.3f} s, "
            f"{len(summary.jumps)} large step(s)"
        )

    def _stage_plot(self) -> str:
        plot_path = self.output_dir / ARTIFACT_PLOT_PNG
        try:
            create_plot(
                self._records,
                self._summary,
                self._normalized_path,
                plot_path,
                title=f"IMU attitude experiment ({self._data_kind}): {self._normalized_path.name}",
            )
        except PlottingUnavailableError as exc:
            if self.config.require_plot:
                raise
            raise _SkipStage(f"matplotlib unavailable ({exc.message})") from exc
        self.result.outputs["plot"] = str(plot_path)
        return f"{plot_path.name}"

    def _stage_report(self) -> str:
        report_path = self.output_dir / ARTIFACT_REPORT_MD
        plot_name = (self.output_dir / ARTIFACT_PLOT_PNG).name
        if "plot" not in self.result.outputs:
            plot_name = ""

        sections = [("Pipeline summary", self._render_pipeline_section())]
        validation_section = self._render_validation_section()
        if validation_section:
            sections.append(("Input validation", validation_section))

        report_path.write_text(
            render_markdown(
                self._summary,
                self._normalized_path,
                self._data_kind,
                plot_name,
                extra_sections=sections,
            ),
            encoding="utf-8",
        )
        self.result.outputs["report"] = str(report_path)
        return report_path.name

    # -- helpers --------------------------------------------------------- #
    def _resolve_data_kind(self) -> str:
        """Decide the provenance label recorded in every artifact.

        ``auto`` is resolved from the *source* first, because the normalised
        file is always named ``normalized.csv`` and would therefore never match
        the filename heuristics on its own. Getting this wrong would let a
        simulated run masquerade as a hardware measurement.
        """
        if self.config.data_kind != "auto":
            return self.config.data_kind
        if self.config.source == "simulated":
            return "simulated"
        if self.config.source == "serial":
            return "real"
        return resolve_data_kind("auto", self.config.input_path or self._normalized_path)

    def _analysis_payload(self, summary: ExperimentSummary) -> dict[str, object]:
        return {
            "source": self.config.source,
            "input_path": str(self.config.input_path) if self.config.input_path else None,
            "normalized_path": str(self._normalized_path),
            "data_kind": self._data_kind,
            "statistics": {
                "sample_count": summary.sample_count,
                "skipped_rows": summary.skipped_rows,
                "duration_s": round(summary.duration_s, 6),
                "estimated_hz": (
                    round(summary.estimated_hz, 6) if summary.estimated_hz is not None else None
                ),
                "jump_threshold_deg": summary.jump_threshold_deg,
                "jump_count": len(summary.jumps),
                "pitch": _stats_dict(summary.pitch),
                "roll": _stats_dict(summary.roll),
            },
            "large_steps": [
                {
                    "sample_index": event.sample_index,
                    "elapsed_s": round(event.elapsed_s, 6),
                    "pitch_delta_deg": round(event.pitch_delta_deg, 6),
                    "roll_delta_deg": round(event.roll_delta_deg, 6),
                    "pitch_jump": event.pitch_jump,
                    "roll_jump": event.roll_jump,
                }
                for event in summary.jumps[: self.config.log_limit]
            ],
        }

    def _render_pipeline_section(self) -> str:
        rows = ["| Stage | Status | Duration (ms) | Detail |", "|---|---|---:|---|"]
        for stage in self.result.stages:
            detail = stage.detail.replace("|", "\\|")
            rows.append(f"| {stage.name} | {stage.status} | {stage.duration_ms:.1f} | {detail} |")
        if self._data_kind == "simulated":
            rows.extend([
                "",
                "> This run used **synthetic** data. The numbers below describe generated "
                "test data and must not be reported as hardware measurements.",
            ])
        return "\n".join(rows)

    def _render_validation_section(self) -> str:
        report = self._validation
        if report is None:
            return ""
        summary = report.summary()
        lines = [
            f"- total rows: {summary['total_rows']}",
            f"- valid rows: {summary['valid_rows']}",
            f"- rejected rows: {summary['invalid_rows']}",
            f"- fatal findings: {summary['fatal']}",
            f"- errors: {summary['errors']}",
            f"- warnings: {summary['warnings']}",
        ]
        if report.issues:
            lines.extend(["", "| Severity | Location | Finding |", "|---|---|---|"])
            for issue in report.issues[:20]:
                location = "file" if issue.row_number is None else f"row {issue.row_number}"
                if issue.field:
                    location += f" / {issue.field}"
                lines.append(
                    f"| {issue.severity.value} | {location} | {issue.message.replace('|', chr(92) + '|')} |"
                )
            remaining = len(report.issues) - 20 + report.truncated_issues
            if remaining > 0:
                lines.append("")
                lines.append(f"_{remaining} further finding(s) omitted._")
        return "\n".join(lines)

    def _log(self, payload: dict[str, object]) -> None:
        entry = {"ts_ms": round(time.time() * 1000, 3), **payload}
        self._log_lines.append(entry)

    def _write_log(self) -> None:
        path = self.output_dir / ARTIFACT_LOG
        try:
            with path.open("w", encoding="utf-8") as stream:
                for entry in self._log_lines:
                    stream.write(json.dumps(entry, ensure_ascii=False) + "\n")
            self.result.outputs["log"] = str(path)
        except OSError:
            # Logging must never turn a successful run into a failed one.
            pass


class _SkipStage(Exception):
    """Internal signal that a stage was legitimately skipped."""

    def __init__(self, reason: str) -> None:
        super().__init__(reason)
        self.reason = reason


def _stats_dict(stats) -> dict[str, float]:
    return {
        "min": round(stats.minimum, 6),
        "max": round(stats.maximum, 6),
        "mean": round(stats.mean, 6),
        "stddev": round(stats.stddev, 6),
    }


def _exit_code_for_report(report: ValidationReport) -> int:
    if not report.file_exists:
        return int(ExitCode.INPUT_MISSING)
    if report.fatal_issues:
        codes = {issue.code for issue in report.fatal_issues}
        if "missing_columns" in codes or "not_a_file" in codes:
            return int(ExitCode.DATA_INVALID)
        return int(ExitCode.NO_DATA)
    if report.valid_rows == 0:
        return int(ExitCode.NO_DATA)
    return int(ExitCode.OK)


def run_pipeline(config: PipelineConfig) -> PipelineResult:
    """Run the pipeline and return its result instead of exiting.

    This is the entry point used by the unit tests; :func:`main` wraps it for
    the command line.
    """
    return PipelineRunner(config).run()


# --------------------------------------------------------------------------- #
# Command line
# --------------------------------------------------------------------------- #
def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m host.pipeline",
        description="One-command IMU experiment pipeline: validate, analyse, plot, report",
    )
    subparsers = parser.add_subparsers(dest="command")

    run = subparsers.add_parser("run", help="run the full pipeline (default)")
    _add_common_options(run)
    run.add_argument("--allow-partial", action="store_true", default=True,
                     help="keep valid rows when some rows are malformed (default)")
    run.add_argument("--strict", action="store_true",
                     help="fail when any row is malformed instead of keeping the rest")
    run.add_argument("--require-plot", action="store_true",
                     help="fail when matplotlib is unavailable instead of skipping the figure")
    run.add_argument("--quiet", action="store_true", help="only print the final summary")

    validate = subparsers.add_parser("validate", help="validate a CSV and print a report")
    validate.add_argument("csv_file", help="CSV file to validate")
    validate.add_argument("--json", action="store_true", help="print the report as JSON")
    validate.add_argument("--max-issues", type=int, default=200, help="cap on stored findings")
    return parser


def _add_common_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--source", choices=("simulated", "csv", "serial"), default="simulated")
    parser.add_argument("--input", help="input CSV path (required for --source csv)")
    parser.add_argument("--output-dir", default="reports/latest", help="artifact directory")
    parser.add_argument("--duration", type=float, default=5.0, help="capture seconds (simulated/serial)")
    parser.add_argument("--hz", type=int, default=20, help="simulated sample rate")
    parser.add_argument("--seed", type=int, default=7, help="simulated random seed")
    parser.add_argument("--port", default="COM3", help="serial port for --source serial")
    parser.add_argument("--baud", type=int, default=9600, help="serial baud rate")
    parser.add_argument("--jump-threshold-deg", type=float, default=30.0,
                        help="flag adjacent angle changes above this value")
    parser.add_argument("--data-kind", choices=("auto",) + DATA_KINDS, default="auto",
                        help="provenance recorded in the report")


def _config_from_args(args: argparse.Namespace) -> PipelineConfig:
    return PipelineConfig(
        source=args.source,
        input_path=Path(args.input) if args.input else None,
        output_dir=Path(args.output_dir),
        duration_s=args.duration,
        hz=args.hz,
        seed=args.seed,
        port=args.port,
        baud=args.baud,
        jump_threshold_deg=args.jump_threshold_deg,
        data_kind=args.data_kind,
        allow_partial=not getattr(args, "strict", False),
        require_plot=getattr(args, "require_plot", False),
        quiet=getattr(args, "quiet", False),
    )


def _normalize_argv(argv: list[str]) -> list[str]:
    """Default to the ``run`` subcommand when the user omits it."""
    if not argv:
        return ["run"]
    if argv[0] in {"run", "validate"}:
        return list(argv)
    if argv[0] in {"-h", "--help"}:
        return list(argv)
    return ["run", *argv]


def main(argv: list[str] | None = None) -> int:
    raw = list(sys.argv[1:] if argv is None else argv)
    parser = build_parser()
    args = parser.parse_args(_normalize_argv(raw))

    if args.command == "validate":
        return _run_validate(args)
    if args.command is None:
        parser.print_help(sys.stderr)
        return int(ExitCode.USAGE)
    return _run_pipeline_command(args)


def _run_validate(args: argparse.Namespace) -> int:
    try:
        report = validate_csv(args.csv_file, max_issues=args.max_issues)
    except ImuLabError as exc:
        print(render_failure("error", exc), file=sys.stderr)
        return int(exc.exit_code)
    except Exception as exc:  # noqa: BLE001
        failure = translate(exc)
        print(render_failure("unexpected error", failure), file=sys.stderr)
        return int(failure.exit_code)

    if args.json:
        print(json.dumps(report.as_dict(), indent=2, ensure_ascii=False))
    else:
        print(report.render())
    return _exit_code_for_report(report)


def _run_pipeline_command(args: argparse.Namespace) -> int:
    config = _config_from_args(args)
    if not config.quiet:
        print(f"IMU Attitude Lab pipeline -> {config.output_dir}")
    try:
        result = run_pipeline(config)
    except ImuLabError as exc:
        print(render_failure("error", exc), file=sys.stderr)
        return int(exc.exit_code)
    except Exception as exc:  # noqa: BLE001
        failure = translate(exc)
        print(render_failure("unexpected error", failure), file=sys.stderr)
        return int(failure.exit_code)

    print()
    print(result.render())
    if result.ok:
        report = result.outputs.get("report")
        if report:
            print(f"\nOpen the report: {report}")
    else:
        failed = result.failed_stage
        if failed:
            print(f"\nHint: re-run with --source simulated to verify the toolchain without hardware.",
                  file=sys.stderr)
    return result.exit_code


if __name__ == "__main__":
    raise SystemExit(main())
