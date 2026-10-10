"""Pitch/Roll plotting for experiment reports.

matplotlib is an *optional* dependency at runtime: it is imported lazily inside
:func:`create_plot` so that ``--help``, validation and analysis all work on a
machine (or CI runner) that only installed the core requirements. Callers catch
:class:`PlottingUnavailableError` and degrade gracefully.
"""

from __future__ import annotations

from pathlib import Path
from typing import Sequence

from .csvlog import CsvRecord
from .errors import SourceUnavailableError
from .reporting import ExperimentSummary


class PlottingUnavailableError(SourceUnavailableError):
    """matplotlib is not installed in the active interpreter."""


def create_plot(
    records: Sequence[CsvRecord],
    summary: ExperimentSummary,
    input_path: Path,
    output_path: Path,
    *,
    title: str | None = None,
) -> Path:
    """Render a two-panel Pitch/Roll figure and return the written path.

    Large-step events flagged by :func:`host.imu_lab.reporting.analyze_records`
    are marked with an ``x`` so the plot and the Markdown table agree.
    """
    if not records:
        raise ValueError("at least one record is required to plot")

    try:
        import matplotlib
    except ImportError as exc:  # pragma: no cover - depends on environment
        raise PlottingUnavailableError.for_missing_dependency("matplotlib", exc) from exc

    # A non-interactive backend keeps report generation working on headless CI.
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    times = [record.elapsed_s for record in records]
    pitch = [record.sample.pitch_deg for record in records]
    roll = [record.sample.roll_deg for record in records]

    pitch_jump_times, pitch_jump_values = [], []
    roll_jump_times, roll_jump_values = [], []
    for event in summary.jumps:
        index = event.sample_index
        if not 0 <= index < len(records):
            continue
        if event.pitch_jump:
            pitch_jump_times.append(times[index])
            pitch_jump_values.append(pitch[index])
        if event.roll_jump:
            roll_jump_times.append(times[index])
            roll_jump_values.append(roll[index])

    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    figure, axes = plt.subplots(2, 1, figsize=(11, 7), sharex=True, constrained_layout=True)
    figure.suptitle(title or f"IMU attitude experiment: {input_path.name}")

    axes[0].plot(times, pitch, label="Pitch", linewidth=1.5, color="#1f77b4")
    axes[0].set_ylabel("Pitch (deg)")
    axes[0].grid(True, alpha=0.3)
    if pitch_jump_times:
        axes[0].scatter(pitch_jump_times, pitch_jump_values, marker="x", s=65,
                        color="#d62728", label="Large step")
    axes[0].legend(loc="best")

    axes[1].plot(times, roll, label="Roll", linewidth=1.5, color="#2ca02c")
    axes[1].set_ylabel("Roll (deg)")
    axes[1].set_xlabel("Elapsed time (s)")
    axes[1].grid(True, alpha=0.3)
    if roll_jump_times:
        axes[1].scatter(roll_jump_times, roll_jump_values, marker="x", s=65,
                        color="#d62728", label="Large step")
    axes[1].legend(loc="best")

    figure.savefig(output_path, dpi=160)
    plt.close(figure)
    return output_path
