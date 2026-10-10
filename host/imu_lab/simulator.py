"""Deterministic synthetic IMU stream for development without target hardware."""

from __future__ import annotations

import math
import random
from collections.abc import Iterator

from .protocol import IMUSample


def iter_samples(seconds: float = 5.0, hz: int = 20, seed: int = 7) -> Iterator[IMUSample]:
    """Yield plausible, reproducible sample records for testing the host pipeline."""
    if seconds <= 0:
        raise ValueError("seconds must be > 0")
    if hz <= 0:
        raise ValueError("hz must be > 0")

    rng = random.Random(seed)
    count = int(seconds * hz)
    dt = 1.0 / hz
    for seq in range(count):
        t = seq * dt
        roll = 20.0 * math.sin(2.0 * math.pi * 0.25 * t)
        pitch = 10.0 * math.sin(2.0 * math.pi * 0.15 * t + 0.4)
        yaw = (15.0 * t) % 360.0

        roll_rad = math.radians(roll)
        pitch_rad = math.radians(pitch)
        ax = math.sin(pitch_rad) + rng.gauss(0.0, 0.008)
        ay = -math.sin(roll_rad) * math.cos(pitch_rad) + rng.gauss(0.0, 0.008)
        az = math.cos(roll_rad) * math.cos(pitch_rad) + rng.gauss(0.0, 0.008)

        # These are synthetic signals, not a physics-accurate IMU model.
        gx = 20.0 * 2.0 * math.pi * 0.25 * math.cos(2.0 * math.pi * 0.25 * t)
        gy = 10.0 * 2.0 * math.pi * 0.15 * math.cos(2.0 * math.pi * 0.15 * t + 0.4)
        gz = 15.0 + rng.gauss(0.0, 0.2)
        mx = 30.0 * math.cos(math.radians(yaw)) + rng.gauss(0.0, 0.2)
        my = 30.0 * math.sin(math.radians(yaw)) + rng.gauss(0.0, 0.2)
        mz = 40.0 + rng.gauss(0.0, 0.2)

        yield IMUSample(
            seq=seq,
            t_ms=round(t * 1000),
            ax_g=ax, ay_g=ay, az_g=az,
            gx_dps=gx, gy_dps=gy, gz_dps=gz,
            mx_uT=mx, my_uT=my, mz_uT=mz,
            roll_deg=roll, pitch_deg=pitch, yaw_deg=yaw,
        )
