"""Pure analysis helpers for CSV-based attitude experiment reports."""
from __future__ import annotations

from dataclasses import dataclass
import math
import statistics
from typing import Sequence

from .csvlog import CsvRecord


@dataclass(frozen=True, slots=True)
class AngleStats:
    minimum: float
    maximum: float
    mean: float
    stddev: float


@dataclass(frozen=True, slots=True)
class JumpEvent:
    sample_index: int  # zero-based index of the later sample
    elapsed_s: float
    pitch_delta_deg: float
    roll_delta_deg: float
    pitch_jump: bool
    roll_jump: bool


@dataclass(frozen=True, slots=True)
class ExperimentSummary:
    sample_count: int
    skipped_rows: int
    duration_s: float
    estimated_hz: float | None
    pitch: AngleStats
    roll: AngleStats
    jumps: tuple[JumpEvent, ...]
    jump_threshold_deg: float


def analyze_records(
    records: Sequence[CsvRecord],
    *,
    skipped_rows: int = 0,
    jump_threshold_deg: float = 30.0,
) -> ExperimentSummary:
    """Calculate summary statistics and flag adjacent angle changes above threshold."""
    if not records:
        raise ValueError("at least one valid sample is required")
    if not math.isfinite(jump_threshold_deg) or jump_threshold_deg <= 0:
        raise ValueError("jump_threshold_deg must be finite and greater than zero")
    if skipped_rows < 0:
        raise ValueError("skipped_rows must be non-negative")

    pitch_values = [item.sample.pitch_deg for item in records]
    roll_values = [item.sample.roll_deg for item in records]
    elapsed_values = [item.elapsed_s for item in records]
    duration = max(elapsed_values) - min(elapsed_values)
    estimated_hz = (len(records) - 1) / duration if len(records) > 1 and duration > 0 else None

    jumps: list[JumpEvent] = []
    for index in range(1, len(records)):
        pitch_delta = records[index].sample.pitch_deg - records[index - 1].sample.pitch_deg
        roll_delta = records[index].sample.roll_deg - records[index - 1].sample.roll_deg
        pitch_jump = abs(pitch_delta) > jump_threshold_deg
        roll_jump = abs(roll_delta) > jump_threshold_deg
        if pitch_jump or roll_jump:
            jumps.append(
                JumpEvent(
                    sample_index=index,
                    elapsed_s=records[index].elapsed_s,
                    pitch_delta_deg=pitch_delta,
                    roll_delta_deg=roll_delta,
                    pitch_jump=pitch_jump,
                    roll_jump=roll_jump,
                )
            )

    return ExperimentSummary(
        sample_count=len(records),
        skipped_rows=skipped_rows,
        duration_s=duration,
        estimated_hz=estimated_hz,
        pitch=_stats(pitch_values),
        roll=_stats(roll_values),
        jumps=tuple(jumps),
        jump_threshold_deg=jump_threshold_deg,
    )


def _stats(values: list[float]) -> AngleStats:
    return AngleStats(
        minimum=min(values),
        maximum=max(values),
        mean=statistics.fmean(values),
        stddev=statistics.pstdev(values),
    )
