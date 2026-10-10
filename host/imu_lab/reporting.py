"""Pure analysis helpers for CSV-based attitude experiment reports."""
from __future__ import annotations

from dataclasses import dataclass
import math
from pathlib import Path
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


# --------------------------------------------------------------------------- #
# Report rendering (shared by host.report and host.pipeline)
# --------------------------------------------------------------------------- #
#: Provenance labels accepted by the renderer and by ``--data-kind``.
DATA_KINDS: tuple[str, ...] = ("real", "simulated", "historical", "unknown")


def resolve_data_kind(requested: str, input_path: str | Path) -> str:
    """Turn ``--data-kind auto`` into a concrete provenance label.

    Filenames containing ``simulat``/``demo``/``mock`` are treated as synthetic
    data so that simulated results are never silently presented as measurements
    from the real sensor.
    """
    if requested != "auto":
        return requested
    lowered = str(input_path).lower()
    if "simulat" in lowered or "demo" in lowered or "mock" in lowered:
        return "simulated"
    return "unknown"


def render_markdown(
    summary: ExperimentSummary,
    input_path: str | Path,
    data_kind: str,
    plot_name: str,
    *,
    extra_sections: Sequence[tuple[str, str]] = (),
) -> str:
    """Render the experiment report as Markdown.

    ``extra_sections`` lets the pipeline append its own sections (for example a
    validation summary) without duplicating the whole template.
    """
    input_path = Path(input_path)
    hz_text = f"{summary.estimated_hz:.2f} Hz" if summary.estimated_hz is not None else "N/A"
    lines = [
        f"# IMU Attitude Experiment Report: {input_path.name}",
        "",
        f"- **Input file:** `{input_path}`",
        f"- **Data provenance:** `{data_kind}`",
        f"- **Valid samples:** {summary.sample_count}",
        f"- **Malformed rows skipped:** {summary.skipped_rows}",
        f"- **Recorded duration:** {summary.duration_s:.3f} s",
        f"- **Estimated average sample rate:** {hz_text}",
        f"- **Large-step threshold:** {summary.jump_threshold_deg:.2f} deg",
        f"- **Large-step events:** {len(summary.jumps)}",
        "",
        f"![Pitch and Roll plots]({plot_name})",
        "",
        "## Angle statistics",
        "",
        "| Metric (deg) | Pitch | Roll |",
        "|---|---:|---:|",
        f"| Minimum | {summary.pitch.minimum:.3f} | {summary.roll.minimum:.3f} |",
        f"| Maximum | {summary.pitch.maximum:.3f} | {summary.roll.maximum:.3f} |",
        f"| Mean | {summary.pitch.mean:.3f} | {summary.roll.mean:.3f} |",
        f"| Population standard deviation | {summary.pitch.stddev:.3f} | {summary.roll.stddev:.3f} |",
        "",
        "## Large angle steps",
        "",
    ]
    if not summary.jumps:
        lines.append("No adjacent Pitch/Roll changes exceeded the configured threshold.")
    else:
        lines.extend([
            "A flagged step is a review hint, not proof of sensor failure. Real motion can also produce large changes.",
            "",
            "| Sample index (0-based) | Elapsed (s) | Pitch delta (deg) | Roll delta (deg) | Flagged channel(s) |",
            "|---:|---:|---:|---:|---|",
        ])
        for event in summary.jumps[:50]:
            flagged = ", ".join(name for name, active in (
                ("Pitch", event.pitch_jump), ("Roll", event.roll_jump)
            ) if active)
            lines.append(
                f"| {event.sample_index} | {event.elapsed_s:.3f} | {event.pitch_delta_deg:.3f} | "
                f"{event.roll_delta_deg:.3f} | {flagged} |"
            )
        if len(summary.jumps) > 50:
            lines.append("")
            lines.append(f"Only the first 50 of {len(summary.jumps)} events are listed.")

    for title, body in extra_sections:
        lines.extend(["", f"## {title}", "", body])

    lines.extend([
        "",
        "## Interpretation notes",
        "",
        "- Average sample rate is estimated from elapsed timestamps in this CSV; it is not a measurement of the MCU timer itself.",
        "- Standard deviation describes the observed sample spread, not absolute sensor accuracy.",
        "- If data provenance is simulated, all metrics describe generated test data only and must not be presented as hardware measurements.",
        "",
    ])
    return "\n".join(lines)
