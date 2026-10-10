"""Generate a plot and Markdown report from an IMU attitude CSV log.

This module is a thin command-line wrapper. The reusable pieces live in
``host.imu_lab.reporting`` (analysis + Markdown) and ``host.imu_lab.plotting``
(figure), so ``host.pipeline`` can share exactly the same output.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import sys

from host.imu_lab.csvlog import load_csv
from host.imu_lab.errors import (
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
from host.imu_lab.validation import ensure_valid, validate_csv

__all__ = ["build_parser", "main", "render_markdown", "resolve_data_kind"]


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Generate Pitch/Roll plots and a Markdown report from a CSV attitude log"
    )
    parser.add_argument("csv_file", help="CSV path produced by host.record, host.collect or host.pipeline")
    parser.add_argument("--output-dir", default="reports/latest", help="directory for the PNG and Markdown report")
    parser.add_argument("--jump-threshold-deg", type=float, default=30.0,
                        help="flag adjacent Pitch/Roll changes greater than this threshold")
    parser.add_argument("--data-kind", choices=("auto",) + DATA_KINDS, default="auto",
                        help="declare data provenance for the report")
    parser.add_argument("--allow-partial", action="store_true",
                        help="tolerate malformed rows as long as at least one row is valid")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    try:
        summary, records, input_path, output_dir, data_kind, validation = _prepare(args)
    except ImuLabError as exc:
        print(render_failure("error", exc), file=sys.stderr)
        return int(exc.exit_code)
    except Exception as exc:  # noqa: BLE001 - single boundary conversion point
        failure = translate(exc)
        print(render_failure("unexpected error", failure), file=sys.stderr)
        return int(failure.exit_code)

    output_dir.mkdir(parents=True, exist_ok=True)
    plot_path = output_dir / "attitude_plot.png"
    report_path = output_dir / "report.md"

    plot_name = plot_path.name
    try:
        create_plot(records, summary, input_path, plot_path)
    except PlottingUnavailableError as exc:
        print(f"warning: {exc.message}", file=sys.stderr)
        print("         the report will be written without an embedded figure", file=sys.stderr)
        plot_name = ""

    try:
        report_path.write_text(
            render_markdown(summary, input_path, data_kind, plot_name),
            encoding="utf-8",
        )
    except OSError as exc:
        failure = translate(exc)
        print(render_failure("error", failure), file=sys.stderr)
        return int(failure.exit_code)

    print(f"Valid samples: {summary.sample_count}")
    print(f"Skipped malformed rows: {summary.skipped_rows}")
    if validation.invalid_rows:
        print(f"Rows rejected by validation: {validation.invalid_rows}")
    if plot_name:
        print(f"Plot: {plot_path}")
    print(f"Report: {report_path}")
    if data_kind == "simulated":
        print("Data kind: simulated (not a real sensor accuracy result)")
    return int(ExitCode.OK)


def _prepare(args: argparse.Namespace):
    """Validate and analyse the input, returning everything ``main`` needs."""
    if args.jump_threshold_deg <= 0:
        raise UsageError(
            "--jump-threshold-deg must be greater than zero",
            context={"jump_threshold_deg": args.jump_threshold_deg},
        )

    input_path = Path(args.csv_file)

    # Validate explicitly first so a bad file yields a precise explanation
    # instead of the loader's generic "no valid samples" outcome.
    validation = validate_csv(input_path)
    ensure_valid(validation)

    if not args.allow_partial and validation.invalid_rows:
        from host.imu_lab.errors import DataFormatError

        raise DataFormatError(
            f"{input_path}: {validation.invalid_rows} of {validation.total_rows} row(s) are malformed",
            hints=[
                "re-run with --allow-partial to analyse only the valid rows",
                "run `python -m host.pipeline validate <file>` for a row-by-row diagnosis",
            ],
            context={"issues": [i.as_dict() for i in validation.error_issues[:10]]},
        )

    try:
        records, skipped = load_csv(input_path)
    except OSError as exc:
        raise translate(exc) from exc

    if not records:
        raise EmptyDataError.for_source(input_path, skipped_rows=skipped)

    summary = analyze_records(
        records, skipped_rows=skipped, jump_threshold_deg=args.jump_threshold_deg
    )
    data_kind = resolve_data_kind(args.data_kind, input_path)
    return summary, records, input_path, Path(args.output_dir), data_kind, validation


if __name__ == "__main__":
    raise SystemExit(main())
