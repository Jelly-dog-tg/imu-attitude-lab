"""Summarize a recorded IMU CSV and flag unusually large angle steps."""
from __future__ import annotations

import argparse
import statistics

from host.imu_lab.csvlog import load_csv
from host.imu_lab.errors import EmptyDataError, guarded, require
from host.imu_lab.validation import ensure_valid, validate_csv


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Analyze a recorded attitude CSV")
    parser.add_argument("csv_file", help="CSV path created by host.record or host.pipeline")
    parser.add_argument("--jump-threshold-deg", type=float, default=30.0,
                        help="flag adjacent pitch/roll changes greater than this threshold")
    parser.add_argument("--allow-partial", action="store_true",
                        help="analyse the valid rows instead of failing on malformed ones")
    return parser


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    return build_parser().parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    return guarded("host.analyze", lambda: _analyze(args))


def _analyze(args: argparse.Namespace) -> int:
    require(args.jump_threshold_deg > 0, "--jump-threshold-deg must be greater than zero",
            hints=["a threshold of 30 degrees is a sensible default for hand-held tests"])

    report = validate_csv(args.csv_file)
    if not args.allow_partial and report.invalid_rows:
        from host.imu_lab.errors import DataFormatError

        raise DataFormatError(
            f"{args.csv_file}: {report.invalid_rows} of {report.total_rows} row(s) are malformed",
            hints=["re-run with --allow-partial to analyse only the valid rows"],
            context={"issues": [i.as_dict() for i in report.error_issues[:10]]},
        )
    ensure_valid(report)

    records, skipped = load_csv(args.csv_file)
    if not records:
        raise EmptyDataError.for_source(args.csv_file, skipped_rows=skipped)

    pitch = [record.sample.pitch_deg for record in records]
    roll = [record.sample.roll_deg for record in records]
    elapsed = [record.elapsed_s for record in records]
    jumps = sum(
        1 for previous, current in zip(records, records[1:])
        if abs(current.sample.pitch_deg - previous.sample.pitch_deg) > args.jump_threshold_deg
        or abs(current.sample.roll_deg - previous.sample.roll_deg) > args.jump_threshold_deg
    )

    span = max(elapsed) - min(elapsed)
    print(f"File: {args.csv_file}")
    print(f"Valid samples: {len(records)}")
    print(f"Skipped malformed rows: {skipped}")
    print(f"Duration: {span:.3f} s")
    if len(records) > 1 and span > 0:
        print(f"Approx. average sample rate: {((len(records) - 1) / span):.2f} Hz")
    else:
        print("Approx. average sample rate: N/A")
    _print_stats("Pitch", pitch)
    _print_stats("Roll", roll)
    print(f"Large angle steps (> {args.jump_threshold_deg:g} deg): {jumps}")
    print("Note: large steps are flags for review, not proof of sensor failure.")
    return 0


def _print_stats(name: str, values: list[float]) -> None:
    print(f"{name} (deg): min={min(values):.3f}, max={max(values):.3f}, "
          f"mean={statistics.fmean(values):.3f}, std={statistics.pstdev(values):.3f}")


if __name__ == "__main__":
    raise SystemExit(main())
