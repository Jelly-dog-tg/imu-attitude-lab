"""Generate a plot and Markdown report from an IMU attitude CSV log."""
from __future__ import annotations

import argparse
from pathlib import Path
import sys

from host.imu_lab.csvlog import CsvRecord, load_csv
from host.imu_lab.reporting import ExperimentSummary, analyze_records


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate Pitch/Roll plots and a Markdown report from a CSV attitude log"
    )
    parser.add_argument("csv_file", help="CSV path produced by host.record or host.collect")
    parser.add_argument("--output-dir", default="reports/latest", help="directory for the PNG and Markdown report")
    parser.add_argument("--jump-threshold-deg", type=float, default=30.0,
                        help="flag adjacent Pitch/Roll changes greater than this threshold")
    parser.add_argument("--data-kind", choices=("auto", "real", "simulated", "historical", "unknown"),
                        default="auto", help="declare data provenance for the report")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    input_path = Path(args.csv_file)
    out_dir = Path(args.output_dir)
    if args.jump_threshold_deg <= 0:
        print("--jump-threshold-deg must be greater than zero", file=sys.stderr)
        return 2
    try:
        records, skipped = load_csv(input_path)
        if not records:
            print("No valid samples found; report was not created.", file=sys.stderr)
            print(f"Skipped malformed rows: {skipped}")
            return 1
        summary = analyze_records(
            records, skipped_rows=skipped, jump_threshold_deg=args.jump_threshold_deg
        )
        data_kind = _resolve_data_kind(args.data_kind, input_path)
        out_dir.mkdir(parents=True, exist_ok=True)
        plot_path = out_dir / "attitude_plot.png"
        report_path = out_dir / "report.md"
        create_plot(records, summary, input_path, plot_path)
        report_path.write_text(
            render_markdown(summary, input_path, data_kind, plot_path.name),
            encoding="utf-8",
        )
    except (OSError, ValueError) as exc:
        print(f"Cannot generate report: {exc}", file=sys.stderr)
        return 2

    print(f"Valid samples: {summary.sample_count}")
    print(f"Skipped malformed rows: {summary.skipped_rows}")
    print(f"Plot: {plot_path}")
    print(f"Report: {report_path}")
    if data_kind == "simulated":
        print("Data kind: simulated (not a real sensor accuracy result)")
    return 0


def _resolve_data_kind(requested: str, input_path: Path) -> str:
    if requested != "auto":
        return requested
    lowered = str(input_path).lower()
    if "simulat" in lowered or "demo" in lowered or "mock" in lowered:
        return "simulated"
    return "unknown"


def create_plot(
    records: list[CsvRecord],
    summary: ExperimentSummary,
    input_path: Path,
    output_path: Path,
) -> None:
    # Select a non-interactive backend so reports can be generated on headless systems.
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    times = [record.elapsed_s for record in records]
    pitch = [record.sample.pitch_deg for record in records]
    roll = [record.sample.roll_deg for record in records]
    jump_indices = [event.sample_index for event in summary.jumps]

    figure, axes = plt.subplots(2, 1, figsize=(11, 7), sharex=True, constrained_layout=True)
    figure.suptitle(f"IMU attitude experiment: {input_path.name}")
    axes[0].plot(times, pitch, label="Pitch", linewidth=1.5)
    axes[0].set_ylabel("Pitch (deg)")
    axes[0].grid(True, alpha=0.3)
    axes[0].legend(loc="best")
    axes[1].plot(times, roll, label="Roll", linewidth=1.5)
    axes[1].set_ylabel("Roll (deg)")
    axes[1].set_xlabel("Elapsed time (s)")
    axes[1].grid(True, alpha=0.3)
    axes[1].legend(loc="best")

    pitch_jump_indices = [idx for idx in jump_indices if summary.jumps and any(
        event.sample_index == idx and event.pitch_jump for event in summary.jumps
    )]
    roll_jump_indices = [idx for idx in jump_indices if summary.jumps and any(
        event.sample_index == idx and event.roll_jump for event in summary.jumps
    )]
    if pitch_jump_indices:
        axes[0].scatter([times[i] for i in pitch_jump_indices], [pitch[i] for i in pitch_jump_indices],
                        marker="x", s=65, label="Large step")
        axes[0].legend(loc="best")
    if roll_jump_indices:
        axes[1].scatter([times[i] for i in roll_jump_indices], [roll[i] for i in roll_jump_indices],
                        marker="x", s=65, label="Large step")
        axes[1].legend(loc="best")

    figure.savefig(output_path, dpi=160)
    plt.close(figure)


def render_markdown(
    summary: ExperimentSummary,
    input_path: Path,
    data_kind: str,
    plot_name: str,
) -> str:
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


if __name__ == "__main__":
    raise SystemExit(main())
