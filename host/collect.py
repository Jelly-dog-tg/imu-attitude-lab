"""Collect attitude data from simulated, CSV, or serial input into a standard CSV log."""
from __future__ import annotations

import argparse
from pathlib import Path
import sys

from host.imu_lab.csvlog import AttitudeCsvWriter
from host.imu_lab.sources import SourceError, SourceStats, iter_source_samples


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Normalize attitude data from simulated, CSV, or live serial input"
    )
    parser.add_argument("--source", choices=("simulated", "csv", "serial"), default="simulated")
    parser.add_argument("--input", help="input CSV path (required for --source csv)")
    parser.add_argument("--port", default="COM3", help="serial port, e.g. COM3")
    parser.add_argument("--baud", type=int, default=9600, help="serial baud rate matching firmware")
    parser.add_argument("--duration", type=float, default=5.0, help="simulated/serial capture duration in seconds")
    parser.add_argument("--hz", type=int, default=20, help="simulation sampling rate")
    parser.add_argument("--seed", type=int, default=7, help="simulation random seed")
    parser.add_argument("--output", default="data/collected.csv", help="output CSV path")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.source == "csv" and not args.input:
        print("--input is required when --source csv", file=sys.stderr)
        return 2
    if args.source in {"simulated", "serial"} and args.duration <= 0:
        print("--duration must be greater than zero", file=sys.stderr)
        return 2
    if args.source == "simulated" and args.hz <= 0:
        print("--hz must be greater than zero", file=sys.stderr)
        return 2
    if args.source == "serial" and args.baud <= 0:
        print("--baud must be greater than zero", file=sys.stderr)
        return 2

    output = Path(args.output)
    if args.source == "csv" and Path(args.input).resolve() == output.resolve():
        print("Input and output paths must be different; refusing to overwrite the source CSV.", file=sys.stderr)
        return 2

    stats = SourceStats()
    written = 0
    try:
        records = iter_source_samples(
            args.source,
            input_path=args.input,
            port=args.port,
            baud=args.baud,
            duration_s=args.duration,
            hz=args.hz,
            seed=args.seed,
            stats=stats,
        )
        with AttitudeCsvWriter(output) as writer:
            for record in records:
                writer.write(
                    record.sample,
                    record.raw_line,
                    elapsed_s=record.elapsed_s,
                    timestamp_utc=record.timestamp_utc,
                )
                written += 1
    except (OSError, ValueError, SourceError) as exc:
        print(f"Collection failed: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("\nCollection interrupted by user.")

    print(f"Source: {args.source}")
    print(f"Valid samples written: {written}")
    print(f"Invalid serial frames: {stats.invalid_frames}")
    print(f"Skipped malformed CSV rows: {stats.skipped_csv_rows}")
    print(f"Output: {output}")
    if written == 0:
        print("No valid samples were written.", file=sys.stderr)
        return 1
    if args.source == "simulated":
        print("Note: generated samples are synthetic and are not real sensor measurements.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
