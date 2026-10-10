"""Record live STM32 pitch/roll serial frames to a CSV experiment log."""
from __future__ import annotations

import argparse
import time

from host.imu_lab.attitude_protocol import parse_attitude_line
from host.imu_lab.csvlog import AttitudeCsvWriter
from host.imu_lab.errors import (
    EmptyDataError,
    SourceUnavailableError,
    guarded,
    require,
    translate,
)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Record STM32 IMU attitude frames to CSV")
    parser.add_argument("--port", default="COM3", help="serial port, e.g. COM3")
    parser.add_argument("--baud", type=int, default=9600, help="baud rate matching firmware")
    parser.add_argument("--output", default="data/session.csv", help="output CSV path")
    parser.add_argument("--duration", type=float, default=30.0, help="recording duration in seconds")
    parser.add_argument("--max-samples", type=int, default=0, help="stop after this many valid samples; 0 means no limit")
    return parser


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    return build_parser().parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    return guarded("host.record", lambda: _record(args))


def _record(args: argparse.Namespace) -> int:
    require(args.duration > 0, "--duration must be greater than zero",
            hints=["use --duration 30 for a typical 30 second capture"])
    require(args.max_samples >= 0, "--max-samples cannot be negative")

    try:
        import serial
    except ImportError as exc:
        raise SourceUnavailableError.for_missing_dependency("pyserial", exc) from exc

    try:
        connection = serial.Serial(args.port, args.baud, timeout=0.2)
    except serial.SerialException as exc:
        raise SourceUnavailableError.for_serial(args.port, exc) from exc

    received = 0
    invalid = 0
    started = time.monotonic()
    print(f"Recording {args.port} @ {args.baud} baud for up to {args.duration:g}s -> {args.output}")
    try:
        with connection, AttitudeCsvWriter(args.output) as writer:
            while time.monotonic() - started < args.duration:
                if args.max_samples and received >= args.max_samples:
                    break
                raw = connection.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").strip()
                try:
                    sample = parse_attitude_line(line)
                except ValueError as exc:
                    invalid += 1
                    if invalid <= 5 or invalid % 50 == 0:
                        print(f"Skip invalid frame ({invalid}): {line!r} [{exc}]")
                    continue
                writer.write(sample, line, time.monotonic() - started)
                received += 1
                if received % 100 == 0:
                    print(f"Recorded {received} valid samples; invalid frames: {invalid}")
    except KeyboardInterrupt:
        print("\nRecording stopped by user.")
    except OSError as exc:
        raise translate(exc) from exc

    print(f"Finished: {received} valid samples, {invalid} invalid frames, saved to {args.output}")
    if received == 0:
        raise EmptyDataError.for_source(args.port, skipped_rows=invalid)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
