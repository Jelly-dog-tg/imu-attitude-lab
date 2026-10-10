"""Replay a recorded CSV experiment in timestamp order in the terminal."""
from __future__ import annotations

import argparse
import sys
import time

from host.imu_lab.csvlog import load_csv


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Replay an IMU attitude CSV in the terminal")
    parser.add_argument("csv_file", help="CSV path created by host.record")
    parser.add_argument("--speed", type=float, default=1.0,
                        help="playback speed multiplier (e.g. 2 means twice as fast)")
    parser.add_argument("--no-wait", action="store_true", help="print all rows immediately")
    parser.add_argument("--limit", type=int, default=0, help="maximum rows to replay; 0 means all")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.speed <= 0:
        print("--speed must be greater than zero", file=sys.stderr)
        return 2
    if args.limit < 0:
        print("--limit cannot be negative", file=sys.stderr)
        return 2
    try:
        records, skipped = load_csv(args.csv_file)
    except (OSError, ValueError) as exc:
        print(f"Cannot read CSV: {exc}", file=sys.stderr)
        return 2
    if not records:
        print("No valid samples to replay.", file=sys.stderr)
        return 1

    records_to_play = records[:args.limit] if args.limit else records
    previous_elapsed: float | None = None
    print(f"Replaying {len(records_to_play)} samples from {args.csv_file} (skipped {skipped} malformed rows)")
    try:
        for index, record in enumerate(records_to_play, start=1):
            if previous_elapsed is not None and not args.no_wait:
                delay = max(0.0, record.elapsed_s - previous_elapsed) / args.speed
                time.sleep(min(delay, 10.0))
            sample = record.sample
            print(
                f"[{index:06d}] t={record.elapsed_s:9.3f}s "
                f"Pitch={sample.pitch_deg:8.3f} deg Roll={sample.roll_deg:8.3f} deg"
            )
            previous_elapsed = record.elapsed_s
    except KeyboardInterrupt:
        print("\nReplay stopped by user.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
