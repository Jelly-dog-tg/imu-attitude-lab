"""Record live STM32 pitch/roll serial frames to a CSV experiment log."""
from __future__ import annotations

import argparse
import sys
import time

from host.imu_lab.attitude_protocol import parse_attitude_line
from host.imu_lab.csvlog import AttitudeCsvWriter


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Record STM32 IMU attitude frames to CSV")
    parser.add_argument("--port", default="COM3", help="serial port, e.g. COM3")
    parser.add_argument("--baud", type=int, default=9600, help="baud rate matching firmware")
    parser.add_argument("--output", default="data/session.csv", help="output CSV path")
    parser.add_argument("--duration", type=float, default=30.0, help="recording duration in seconds")
    parser.add_argument("--max-samples", type=int, default=0, help="stop after this many valid samples; 0 means no limit")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.duration <= 0:
        print("--duration must be greater than zero", file=sys.stderr)
        return 2
    if args.max_samples < 0:
        print("--max-samples cannot be negative", file=sys.stderr)
        return 2

    try:
        import serial
    except ImportError:
        print("Missing pyserial. Install dependencies with: python -m pip install -r requirements.txt", file=sys.stderr)
        return 2

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.2)
    except serial.SerialException as exc:
        print(f"Cannot open serial port {args.port}: {exc}", file=sys.stderr)
        print("Check Device Manager, CH340, baud rate, and whether another program owns the port.", file=sys.stderr)
        return 2

    received = 0
    invalid = 0
    started = time.monotonic()
    print(f"Recording {args.port} @ {args.baud} baud for up to {args.duration:g}s -> {args.output}")
    try:
        with ser, AttitudeCsvWriter(args.output) as writer:
            while time.monotonic() - started < args.duration:
                if args.max_samples and received >= args.max_samples:
                    break
                raw = ser.readline()
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
        print(f"Cannot write output file: {exc}", file=sys.stderr)
        return 2
    print(f"Finished: {received} valid samples, {invalid} invalid frames, saved to {args.output}")
    return 0 if received > 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
