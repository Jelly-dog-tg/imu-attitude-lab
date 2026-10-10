"""Summarize a recorded IMU CSV and flag unusually large angle steps."""
from __future__ import annotations

import argparse
import statistics
import sys

from host.imu_lab.csvlog import load_csv


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Analyze a recorded attitude CSV")
    parser.add_argument("csv_file", help="CSV path created by host.record")
    parser.add_argument("--jump-threshold-deg", type=float, default=30.0,
                        help="flag adjacent pitch/roll changes greater than this threshold")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.jump_threshold_deg <= 0:
        print("--jump-threshold-deg must be greater than zero", file=sys.stderr)
        return 2
    try:
        records, skipped = load_csv(args.csv_file)
    except (OSError, ValueError) as exc:
        print(f"Cannot read CSV: {exc}", file=sys.stderr)
        return 2
    if not records:
        print("No valid samples found.", file=sys.stderr)
        print(f"Skipped malformed rows: {skipped}")
        return 1

    pitch = [record.sample.pitch_deg for record in records]
    roll = [record.sample.roll_deg for record in records]
    elapsed = [record.elapsed_s for record in records]
    jumps = sum(
        1 for previous, current in zip(records, records[1:])
        if abs(current.sample.pitch_deg - previous.sample.pitch_deg) > args.jump_threshold_deg
        or abs(current.sample.roll_deg - previous.sample.roll_deg) > args.jump_threshold_deg
    )

    print(f"File: {args.csv_file}")
    print(f"Valid samples: {len(records)}")
    print(f"Skipped malformed rows: {skipped}")
    print(f"Duration: {max(elapsed) - min(elapsed):.3f} s")
    print(f"Approx. average sample rate: {((len(records) - 1) / (max(elapsed) - min(elapsed))):.2f} Hz" if len(records) > 1 and max(elapsed) > min(elapsed) else "Approx. average sample rate: N/A")
    _print_stats("Pitch", pitch)
    _print_stats("Roll", roll)
    print(f"Large angle steps (> {args.jump_threshold_deg:g} deg): {jumps}")
    print("Note: large steps are flags for review, not proof of sensor failure.")
    return 0


def _print_stats(name: str, values: list[float]) -> None:
    print(f"{name} (deg): min={min(values):.3f}, max={max(values):.3f}, mean={statistics.fmean(values):.3f}, std={statistics.pstdev(values):.3f}")


if __name__ == "__main__":
    raise SystemExit(main())
