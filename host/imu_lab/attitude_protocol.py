"""Parser for the serial line emitted by the existing STM32 firmware.

Current firmware format: P:<pitch_deg>,R:<roll_deg>

Optional experimental format: P:<pitch_deg>,R:<roll_deg>,X:<x>,Y:<y>
Position values are experimental only and are not reliable inertial navigation.
"""
from __future__ import annotations

from dataclasses import dataclass
import math


@dataclass(frozen=True, slots=True)
class AttitudeSample:
    pitch_deg: float
    roll_deg: float
    x: float | None = None
    y: float | None = None


def parse_attitude_line(line: str) -> AttitudeSample:
    """Parse a single P/R serial line; raise ValueError on malformed input."""
    cleaned = line.strip()
    if not cleaned:
        raise ValueError("empty line")

    fields: dict[str, float] = {}
    for item in cleaned.split(","):
        if ":" not in item:
            raise ValueError("each field must contain ':'")
        key, raw_value = (part.strip() for part in item.split(":", 1))
        if key not in {"P", "R", "X", "Y"}:
            raise ValueError(f"unknown field: {key!r}")
        if key in fields:
            raise ValueError(f"duplicate field: {key!r}")
        try:
            value = float(raw_value)
        except ValueError as exc:
            raise ValueError(f"field {key!r} is not numeric") from exc
        if not math.isfinite(value):
            raise ValueError(f"field {key!r} must be finite")
        fields[key] = value

    if "P" not in fields or "R" not in fields:
        raise ValueError("required fields P and R are missing")
    if ("X" in fields) != ("Y" in fields):
        raise ValueError("experimental position requires both X and Y")

    return AttitudeSample(
        pitch_deg=fields["P"],
        roll_deg=fields["R"],
        x=fields.get("X"),
        y=fields.get("Y"),
    )
