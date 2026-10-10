"""CSV logging and loading utilities for attitude experiments."""
from __future__ import annotations

import csv
from dataclasses import dataclass
from datetime import datetime, timezone
import math
from pathlib import Path
from typing import Iterable, TextIO

from host.imu_lab.attitude_protocol import AttitudeSample
from host.imu_lab.errors import DataFormatError, UsageError

CSV_FIELDS = (
    "timestamp_utc",
    "elapsed_s",
    "pitch_deg",
    "roll_deg",
    "x",
    "y",
    "raw_line",
)


@dataclass(frozen=True, slots=True)
class CsvRecord:
    timestamp_utc: str
    elapsed_s: float
    sample: AttitudeSample
    raw_line: str


class AttitudeCsvWriter:
    """Write validated attitude samples as one CSV row per sample."""

    def __init__(self, path: str | Path) -> None:
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._file: TextIO = self.path.open("w", newline="", encoding="utf-8")
        self._writer = csv.DictWriter(self._file, fieldnames=CSV_FIELDS)
        self._writer.writeheader()
        self._file.flush()

    def write(
        self,
        sample: AttitudeSample,
        raw_line: str,
        elapsed_s: float,
        timestamp_utc: str | None = None,
    ) -> None:
        if not math.isfinite(elapsed_s) or elapsed_s < 0:
            raise UsageError(
                "elapsed_s must be a finite non-negative number",
                context={"elapsed_s": elapsed_s},
            )
        timestamp = timestamp_utc or datetime.now(timezone.utc).isoformat(timespec="milliseconds")
        self._writer.writerow(
            {
                "timestamp_utc": timestamp,
                "elapsed_s": f"{elapsed_s:.6f}",
                "pitch_deg": f"{sample.pitch_deg:.6f}",
                "roll_deg": f"{sample.roll_deg:.6f}",
                "x": "" if sample.x is None else f"{sample.x:.6f}",
                "y": "" if sample.y is None else f"{sample.y:.6f}",
                "raw_line": raw_line,
            }
        )
        self._file.flush()

    def close(self) -> None:
        if not self._file.closed:
            self._file.close()

    def __enter__(self) -> "AttitudeCsvWriter":
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        self.close()


def load_csv(path: str | Path) -> tuple[list[CsvRecord], int]:
    """Load valid rows and return (records, skipped_rows).

    Raises ValueError when the file is missing required columns. Malformed data
    rows are counted and skipped so analysis/replay can continue on partial logs.
    """
    records: list[CsvRecord] = []
    skipped = 0
    required = {"timestamp_utc", "elapsed_s", "pitch_deg", "roll_deg"}
    with Path(path).open("r", newline="", encoding="utf-8-sig") as file:
        reader = csv.DictReader(file)
        if reader.fieldnames is None or not required.issubset(set(reader.fieldnames)):
            found = ", ".join(reader.fieldnames or []) or "<empty header>"
            raise DataFormatError(
                f"CSV must contain columns: {', '.join(sorted(required))}",
                hints=[
                    "log the data with `python -m host.collect --source ...` to get the standard schema",
                    f"columns found in this file: {found}",
                ],
                context={"required": sorted(required), "found": list(reader.fieldnames or [])},
            )
        for row in reader:
            try:
                elapsed_s = float(row["elapsed_s"])
                pitch = float(row["pitch_deg"])
                roll = float(row["roll_deg"])
                if not all(math.isfinite(v) for v in (elapsed_s, pitch, roll)) or elapsed_s < 0:
                    raise ValueError("non-finite or negative value")
                x = _optional_float(row.get("x"))
                y = _optional_float(row.get("y"))
                if (x is None) != (y is None):
                    raise ValueError("X and Y must both be empty or both be present")
                records.append(
                    CsvRecord(
                        timestamp_utc=(row.get("timestamp_utc") or "").strip(),
                        elapsed_s=elapsed_s,
                        sample=AttitudeSample(pitch, roll, x, y),
                        raw_line=row.get("raw_line") or "",
                    )
                )
            except (TypeError, ValueError, KeyError):
                skipped += 1
    return records, skipped


def _optional_float(raw_value: str | None) -> float | None:
    if raw_value is None or not raw_value.strip():
        return None
    value = float(raw_value)
    if not math.isfinite(value):
        raise ValueError("optional value must be finite")
    return value
