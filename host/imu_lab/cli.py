"""Command-line entry point for IMU Attitude Lab."""

from __future__ import annotations

import argparse
from pathlib import Path

from .protocol import format_sample
from .simulator import iter_samples


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="IMU Attitude Lab host utilities")
    subparsers = parser.add_subparsers(dest="command", required=True)

    sim = subparsers.add_parser("simulate", help="write a synthetic IMU stream to a text file")
    sim.add_argument("--seconds", type=float, default=5.0, help="simulation duration")
    sim.add_argument("--hz", type=int, default=20, help="sampling frequency")
    sim.add_argument("--seed", type=int, default=7, help="random seed for repeatable data")
    sim.add_argument("--output", type=Path, default=Path("data/demo_stream.txt"), help="output file")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if args.command == "simulate":
        if args.seconds <= 0 or args.hz <= 0:
            raise SystemExit("error: --seconds and --hz must be positive")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        count = 0
        with args.output.open("w", encoding="utf-8", newline="\n") as stream:
            for sample in iter_samples(args.seconds, args.hz, args.seed):
                stream.write(format_sample(sample) + "\n")
                count += 1
        print(f"Wrote {count} records to {args.output}")
        print("Note: records are synthetic and do not represent real sensor accuracy.")
        return 0
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
