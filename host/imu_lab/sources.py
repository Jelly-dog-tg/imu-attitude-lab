"""Common data-source adapters for simulated, CSV, and live serial samples.

All adapters yield SourceRecord objects, so downstream code (logging, analysis,
replay, or visualization) does not need to know where a sample came from.
"""
from __future__ import annotations

from dataclasses import dataclass
import math
from pathlib import Path
import random
import time
from typing import Iterator, Literal

from .attitude_protocol import AttitudeSample, parse_attitude_line
from .csvlog import load_csv

SourceName = Literal["simulated", "csv", "serial"]


@dataclass(frozen=True, slots=True)
class SourceRecord:
    """Normalized attitude record emitted by any input adapter."""

    elapsed_s: float
    sample: AttitudeSample
    raw_line: str
    timestamp_utc: str | None = None


@dataclass(slots=True)
class SourceStats:
    """Counters accumulated while reading a source."""

    invalid_frames: int = 0
    skipped_csv_rows: int = 0


class SourceError(RuntimeError):
    """Raised when a requested data source cannot be opened or read."""


def iter_simulated_samples(
    duration_s: float = 5.0,
    hz: int = 20,
    seed: int = 7,
) -> Iterator[SourceRecord]:
    """Yield deterministic, synthetic Pitch/Roll samples at a fixed rate."""
    _validate_duration(duration_s)
    if hz <= 0:
        raise ValueError("hz must be greater than zero")

    rng = random.Random(seed)
    count = int(duration_s * hz)
    for seq in range(count):
        elapsed = seq / hz
        pitch = 8.0 * math.sin(2.0 * math.pi * 0.2 * elapsed) + rng.gauss(0, 0.03)
        roll = 15.0 * math.sin(2.0 * math.pi * 0.3 * elapsed + 0.2) + rng.gauss(0, 0.03)
        raw_line = f"P:{pitch:.3f},R:{roll:.3f}"
        yield SourceRecord(
            elapsed_s=elapsed,
            sample=AttitudeSample(pitch_deg=pitch, roll_deg=roll),
            raw_line=raw_line,
        )


def iter_csv_samples(
    path: str | Path,
    stats: SourceStats | None = None,
) -> Iterator[SourceRecord]:
    """Yield valid records from an existing experiment CSV file."""
    records, skipped = load_csv(path)
    if stats is not None:
        stats.skipped_csv_rows += skipped
    for record in records:
        raw_line = record.raw_line or (
            f"P:{record.sample.pitch_deg:.6f},R:{record.sample.roll_deg:.6f}"
        )
        yield SourceRecord(
            elapsed_s=record.elapsed_s,
            sample=record.sample,
            raw_line=raw_line,
            timestamp_utc=record.timestamp_utc or None,
        )


def iter_serial_samples(
    port: str,
    baud: int = 9600,
    duration_s: float = 30.0,
    stats: SourceStats | None = None,
) -> Iterator[SourceRecord]:
    """Yield parsed frames from the existing STM32 P/R serial protocol.

    pyserial is imported only when the serial source is selected, so offline
    workflows and --help work even on computers without pyserial or hardware.
    """
    _validate_duration(duration_s)
    if not port.strip():
        raise ValueError("port must not be empty")
    if baud <= 0:
        raise ValueError("baud must be greater than zero")

    try:
        import serial
    except ImportError as exc:
        raise SourceError(
            "pyserial is not installed; run: python -m pip install -r requirements.txt"
        ) from exc

    try:
        with serial.Serial(port, baud, timeout=0.2) as connection:
            started = time.monotonic()
            while time.monotonic() - started < duration_s:
                raw = connection.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").strip()
                try:
                    sample = parse_attitude_line(line)
                except ValueError:
                    if stats is not None:
                        stats.invalid_frames += 1
                    continue
                yield SourceRecord(
                    elapsed_s=time.monotonic() - started,
                    sample=sample,
                    raw_line=line,
                )
    except serial.SerialException as exc:
        raise SourceError(
            f"cannot open/read serial port {port}: {exc}. Check the port, cable, driver, and whether another program uses it."
        ) from exc


def iter_source_samples(
    source: SourceName,
    *,
    input_path: str | Path | None = None,
    port: str = "COM3",
    baud: int = 9600,
    duration_s: float = 5.0,
    hz: int = 20,
    seed: int = 7,
    stats: SourceStats | None = None,
) -> Iterator[SourceRecord]:
    """Return a normalized iterable for one of the supported data sources."""
    counters = stats if stats is not None else SourceStats()
    if source == "simulated":
        yield from iter_simulated_samples(duration_s=duration_s, hz=hz, seed=seed)
    elif source == "csv":
        if input_path is None:
            raise ValueError("input_path is required when source='csv'")
        yield from iter_csv_samples(input_path, stats=counters)
    elif source == "serial":
        yield from iter_serial_samples(port=port, baud=baud, duration_s=duration_s, stats=counters)
    else:
        raise ValueError(f"unsupported source: {source!r}")


def _validate_duration(duration_s: float) -> None:
    if not math.isfinite(duration_s) or duration_s <= 0:
        raise ValueError("duration_s must be a finite number greater than zero")
