"""Parser and formatter for the IMU Attitude Lab newline-delimited protocol."""

from __future__ import annotations

from dataclasses import dataclass
import math


@dataclass(frozen=True, slots=True)
class IMUSample:
    """One decoded IMU sample. Units are encoded in field names."""

    seq: int
    t_ms: int
    ax_g: float
    ay_g: float
    az_g: float
    gx_dps: float
    gy_dps: float
    gz_dps: float
    mx_uT: float
    my_uT: float
    mz_uT: float
    roll_deg: float
    pitch_deg: float
    yaw_deg: float


FIELD_NAMES = (
    "seq", "t_ms", "ax_g", "ay_g", "az_g",
    "gx_dps", "gy_dps", "gz_dps",
    "mx_uT", "my_uT", "mz_uT",
    "roll_deg", "pitch_deg", "yaw_deg",
)


def parse_line(line: str) -> IMUSample:
    """Decode one wire-protocol line or raise ValueError with a useful reason."""
    parts = line.strip().split(",")
    if not parts or parts[0] != "IMU":
        raise ValueError("invalid record prefix: expected 'IMU'")
    if len(parts) != 15:
        raise ValueError(f"invalid field count: expected 15, got {len(parts)}")

    try:
        seq = int(parts[1])
        t_ms = int(parts[2])
        values = [float(value) for value in parts[3:]]
    except ValueError as exc:
        raise ValueError("record contains a non-numeric field") from exc

    if seq < 0 or t_ms < 0:
        raise ValueError("seq and t_ms must be non-negative")
    if not all(math.isfinite(value) for value in values):
        raise ValueError("numeric fields must be finite")

    return IMUSample(seq=seq, t_ms=t_ms, **dict(zip(FIELD_NAMES[2:], values)))


def format_sample(sample: IMUSample) -> str:
    """Encode a sample into one comma-separated line without the newline."""
    numeric = [
        sample.ax_g, sample.ay_g, sample.az_g,
        sample.gx_dps, sample.gy_dps, sample.gz_dps,
        sample.mx_uT, sample.my_uT, sample.mz_uT,
        sample.roll_deg, sample.pitch_deg, sample.yaw_deg,
    ]
    if sample.seq < 0 or sample.t_ms < 0:
        raise ValueError("seq and t_ms must be non-negative")
    if not all(math.isfinite(value) for value in numeric):
        raise ValueError("numeric fields must be finite")
    fields = ["IMU", str(sample.seq), str(sample.t_ms)]
    fields.extend(f"{value:.6f}" for value in numeric)
    return ",".join(fields)
