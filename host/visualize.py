"""Robust VPython visualizer for the existing STM32 P/R serial output.

Install dependencies with: python -m pip install -r requirements.txt
Example: python -m host.visualize --port COM3 --baud 9600
"""
from __future__ import annotations

import argparse
import sys

import serial
from vpython import box, color, compound, cylinder, rate, radians, scene, vec

from host.imu_lab.attitude_protocol import parse_attitude_line


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Visualize STM32 IMU pitch/roll in 3D")
    parser.add_argument("--port", default="COM3", help="serial port, e.g. COM3")
    parser.add_argument("--baud", type=int, default=9600, help="baud rate (must match firmware)")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.1)
    except serial.SerialException as exc:
        print(f"Cannot open serial port {args.port}: {exc}", file=sys.stderr)
        print("Check Device Manager, the CH340 connection, and whether another app owns the port.", file=sys.stderr)
        return 2

    scene.title = "STM32F103 + MPU9250 | Real-time Attitude"
    scene.caption = "Waiting for P/R lines from STM32..."
    scene.width = 900
    scene.height = 650
    scene.background = color.gray(0.12)

    fuselage = cylinder(pos=vec(0, 0, 0), axis=vec(4, 0, 0), radius=0.4, color=color.white)
    wing = box(pos=vec(1.5, 0, 0), size=vec(0.2, 0.1, 5), color=color.red)
    tail = box(pos=vec(3.8, 0.2, 0), size=vec(0.2, 1, 0.1), color=color.orange)
    airplane = compound([fuselage, wing, tail])

    received = 0
    malformed = 0
    print(f"Connected to {args.port} @ {args.baud} baud. Press Ctrl+C to stop.")
    try:
        while True:
            raw = ser.readline()
            if not raw:
                rate(100)
                continue
            line = raw.decode("utf-8", errors="replace").strip()
            try:
                sample = parse_attitude_line(line)
            except ValueError as exc:
                malformed += 1
                if malformed <= 5 or malformed % 50 == 0:
                    print(f"Skip malformed line ({malformed}): {line!r} [{exc}]")
                rate(100)
                continue

            received += 1
            scene.caption = (
                f"Pitch: {sample.pitch_deg:.2f} deg | Roll: {sample.roll_deg:.2f} deg"
                f" | Valid frames: {received} | Invalid lines: {malformed}"
            )

            # Reset to the chosen neutral orientation, then apply the currently measured angles.
            airplane.axis = vec(1, 0, 0)
            airplane.up = vec(0, 1, 0)
            airplane.rotate(angle=radians(sample.roll_deg), axis=vec(1, 0, 0))
            airplane.rotate(angle=-radians(sample.pitch_deg), axis=vec(0, 0, 1))
            if sample.x is not None and sample.y is not None:
                airplane.pos = vec(sample.x, sample.y, 0)
            rate(100)
    except KeyboardInterrupt:
        print("\nStopped by user.")
    finally:
        ser.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
