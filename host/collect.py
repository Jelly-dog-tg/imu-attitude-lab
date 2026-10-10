"""Collect attitude data from simulated, CSV, or serial input into a standard CSV log."""
from __future__ import annotations

import argparse
from pathlib import Path

from host.imu_lab.csvlog import AttitudeCsvWriter
from host.imu_lab.errors import (
    EmptyDataError,
    UsageError,
    guarded,
    require,
    translate,
)
from host.imu_lab.sources import SourceStats, iter_source_samples


def build_parser() -> argparse.ArgumentParser:
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
    return parser


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    return build_parser().parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    return guarded("host.collect", lambda: _collect(args))


def _collect(args: argparse.Namespace) -> int:
    if args.source == "csv" and not args.input:
        raise UsageError(
            "--input is required when --source csv",
            hints=["example: python -m host.collect --source csv --input data/normalized.csv"],
        )
    if args.source in {"simulated", "serial"}:
        require(args.duration > 0, "--duration must be greater than zero")
    if args.source == "simulated":
        require(args.hz > 0, "--hz must be greater than zero")
    if args.source == "serial":
        require(args.baud > 0, "--baud must be greater than zero")

    output = Path(args.output)
    if args.source == "csv" and Path(args.input).resolve() == output.resolve():
        raise UsageError(
            "input and output paths must differ; refusing to overwrite the source CSV",
            hints=["pass a different --output path"],
        )

    stats = SourceStats()
    written = 0
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
    try:
        with AttitudeCsvWriter(output) as writer:
            for record in records:
                writer.write(
                    record.sample,
                    record.raw_line,
                    elapsed_s=record.elapsed_s,
                    timestamp_utc=record.timestamp_utc,
                )
                written += 1
    except KeyboardInterrupt:
        print("\nCollection interrupted by user.")
    except OSError as exc:
        raise translate(exc) from exc

    print(f"Source: {args.source}")
    print(f"Valid samples written: {written}")
    print(f"Invalid serial frames: {stats.invalid_frames}")
    print(f"Skipped malformed CSV rows: {stats.skipped_csv_rows}")
    print(f"Output: {output}")
    if written == 0:
        raise EmptyDataError.for_source(args.source, skipped_rows=stats.skipped_csv_rows)
    if args.source == "simulated":
        print("Note: generated samples are synthetic and are not real sensor measurements.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
