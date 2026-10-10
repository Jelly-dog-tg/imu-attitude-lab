"""Structural and row-level validation for attitude CSV logs.

The loader in :mod:`host.imu_lab.csvlog` is deliberately forgiving: it silently
counts and skips bad rows so that analysis can still run on a partially broken
log. That behaviour is convenient but hides problems, so this module adds an
*explicit* validation pass that explains exactly what is wrong, where, and how
bad it is.

Two usage modes are supported:

* pre-flight check before a pipeline run (:func:`validate_csv`,
  :func:`ensure_valid`);
* ad-hoc diagnosis from the CLI (``python -m host.pipeline validate <file>``).

Nothing in this module touches the network or the serial port, so it runs
identically on Windows, WSL2 and CI runners.
"""

from __future__ import annotations

import csv
import math
from dataclasses import dataclass, replace
from enum import Enum
from pathlib import Path
from typing import Iterable, Iterator

from .errors import DataFormatError, EmptyDataError, InputMissingError, UsageError

#: Columns that every attitude log must provide.
REQUIRED_COLUMNS: tuple[str, ...] = ("timestamp_utc", "elapsed_s", "pitch_deg", "roll_deg")

#: Required columns that must parse as finite numbers.
REQUIRED_NUMERIC_COLUMNS: tuple[str, ...] = ("elapsed_s", "pitch_deg", "roll_deg")

#: Required columns that only need to be present and non-empty (free text).
REQUIRED_TEXT_COLUMNS: tuple[str, ...] = ("timestamp_utc",)

#: Columns recognised when present; anything else is reported as unknown.
KNOWN_COLUMNS: tuple[str, ...] = REQUIRED_COLUMNS + ("x", "y", "raw_line")

#: Hard cap on stored issue objects so a fully corrupt 10M-row file cannot
#: exhaust memory while we produce a report about it.
DEFAULT_MAX_ISSUES = 200


class Severity(str, Enum):
    """How badly a finding affects a downstream run."""

    WARNING = "warning"   #: usable, but worth a look
    ERROR = "error"       #: the row is unusable and will be dropped
    FATAL = "fatal"       #: the whole file is unusable

    def __str__(self) -> str:  # pragma: no cover - trivial
        return self.value


@dataclass(frozen=True)
class ValidationIssue:
    """One concrete problem found in the input."""

    severity: Severity
    code: str
    message: str
    row_number: int | None = None
    field: str | None = None
    value: str | None = None

    def render(self) -> str:
        location = "file" if self.row_number is None else f"row {self.row_number}"
        if self.field:
            location = f"{location}, column {self.field!r}"
        return f"[{self.severity.value}] {location}: {self.message}"

    def as_dict(self) -> dict[str, object]:
        return {
            "severity": self.severity.value,
            "code": self.code,
            "message": self.message,
            "row_number": self.row_number,
            "field": self.field,
            "value": self.value,
        }


@dataclass(frozen=True)
class ValidationReport:
    """Aggregated result of validating one input file."""

    path: Path
    file_exists: bool
    file_size_bytes: int = 0
    header: tuple[str, ...] = ()
    total_rows: int = 0
    valid_rows: int = 0
    invalid_rows: int = 0
    issues: tuple[ValidationIssue, ...] = ()
    truncated_issues: int = 0

    # ---------------------------------------------------------------- queries
    @property
    def ok(self) -> bool:
        """True when the file is usable as-is (warnings allowed)."""
        return self.file_exists and self.valid_rows > 0 and not self.fatal_issues

    @property
    def fatal_issues(self) -> tuple[ValidationIssue, ...]:
        return tuple(i for i in self.issues if i.severity is Severity.FATAL)

    @property
    def error_issues(self) -> tuple[ValidationIssue, ...]:
        return tuple(i for i in self.issues if i.severity is Severity.ERROR)

    @property
    def warning_issues(self) -> tuple[ValidationIssue, ...]:
        return tuple(i for i in self.issues if i.severity is Severity.WARNING)

    @property
    def invalid_ratio(self) -> float:
        return self.invalid_rows / self.total_rows if self.total_rows else 0.0

    @property
    def exceeds_issue_cap(self) -> bool:
        return self.truncated_issues > 0

    # ------------------------------------------------------------ presentation
    def summary(self) -> dict[str, object]:
        return {
            "path": str(self.path),
            "exists": self.file_exists,
            "size_bytes": self.file_size_bytes,
            "columns": list(self.header),
            "total_rows": self.total_rows,
            "valid_rows": self.valid_rows,
            "invalid_rows": self.invalid_rows,
            "invalid_ratio": round(self.invalid_ratio, 6),
            "fatal": len(self.fatal_issues),
            "errors": len(self.error_issues),
            "warnings": len(self.warning_issues),
            "ok": self.ok,
        }

    def render(self, *, limit: int = 20) -> str:
        lines = [f"Validation report for {self.path}"]
        if not self.file_exists:
            lines.append("  file does not exist")
            return "\n".join(lines)
        lines.append(f"  size: {self.file_size_bytes} bytes")
        lines.append(f"  columns: {', '.join(self.header) or '<none>'}")
        lines.append(
            f"  rows: {self.total_rows} total, {self.valid_rows} valid, {self.invalid_rows} invalid"
        )
        lines.append(
            f"  findings: {len(self.fatal_issues)} fatal, "
            f"{len(self.error_issues)} error, {len(self.warning_issues)} warning"
        )
        if self.issues:
            lines.append("  details:")
            for issue in self.issues[:limit]:
                lines.append(f"    {issue.render()}")
            remaining = len(self.issues) - limit + self.truncated_issues
            if remaining > 0:
                lines.append(f"    ... and {remaining} more finding(s) suppressed")
        lines.append(f"  verdict: {'USABLE' if self.ok else 'NOT USABLE'}")
        return "\n".join(lines)

    def as_dict(self) -> dict[str, object]:
        payload = self.summary()
        payload["issues"] = [issue.as_dict() for issue in self.issues]
        payload["truncated_issues"] = self.truncated_issues
        return payload


class _IssueCollector:
    """Accumulates issues while respecting a hard cap."""

    def __init__(self, max_issues: int) -> None:
        self._issues: list[ValidationIssue] = []
        self._max = max_issues
        self.truncated = 0

    def add(self, issue: ValidationIssue) -> None:
        if len(self._issues) < self._max:
            self._issues.append(issue)
        else:
            self.truncated += 1

    @property
    def issues(self) -> tuple[ValidationIssue, ...]:
        return tuple(self._issues)


# --------------------------------------------------------------------------- #
# Public API
# --------------------------------------------------------------------------- #
def validate_csv(
    path: str | Path,
    *,
    max_issues: int = DEFAULT_MAX_ISSUES,
    max_rows: int | None = None,
) -> ValidationReport:
    """Validate a CSV log and return a :class:`ValidationReport`.

    Never raises for bad *content*: every finding is captured in the report so
    callers can choose whether to continue. Raises only for genuinely
    unusable *arguments* (``max_issues``/``max_rows`` out of range).
    """
    if max_issues <= 0:
        raise UsageError("max_issues must be greater than zero", context={"max_issues": max_issues})
    if max_rows is not None and max_rows <= 0:
        raise UsageError("max_rows must be greater than zero", context={"max_rows": max_rows})

    target = Path(path)
    collector = _IssueCollector(max_issues)

    if not target.exists():
        collector.add(
            ValidationIssue(Severity.FATAL, "file_missing", f"file not found: {target}")
        )
        return ValidationReport(path=target, file_exists=False, issues=collector.issues)

    if target.is_dir():
        collector.add(
            ValidationIssue(Severity.FATAL, "not_a_file", "path is a directory, not a CSV file")
        )
        return ValidationReport(path=target, file_exists=True, issues=collector.issues)

    size = target.stat().st_size
    if size == 0:
        collector.add(ValidationIssue(Severity.FATAL, "empty_file", "file is empty (0 bytes)"))
        return ValidationReport(
            path=target, file_exists=True, file_size_bytes=0, issues=collector.issues
        )

    header, total, valid, invalid, truncated_by_rows = _scan_rows(
        target, collector, max_rows=max_rows
    )

    if not header:
        collector.add(
            ValidationIssue(Severity.FATAL, "empty_header", "file has no header line")
        )
    else:
        missing = [c for c in REQUIRED_COLUMNS if c not in header]
        if missing:
            collector.add(
                ValidationIssue(
                    Severity.FATAL,
                    "missing_columns",
                    f"required column(s) absent: {', '.join(missing)}",
                )
            )
        duplicates = sorted({c for c in header if header.count(c) > 1})
        if duplicates:
            collector.add(
                ValidationIssue(
                    Severity.ERROR,
                    "duplicate_columns",
                    f"header repeats column(s): {', '.join(duplicates)}",
                )
            )
        unknown = [c for c in header if c not in KNOWN_COLUMNS]
        if unknown:
            collector.add(
                ValidationIssue(
                    Severity.WARNING,
                    "unknown_columns",
                    f"extra column(s) will be ignored: {', '.join(unknown)}",
                )
            )

    if total == 0 and not collector.issues:
        collector.add(
            ValidationIssue(
                Severity.FATAL,
                "no_data_rows",
                "file contains a header but no data rows",
            )
        )
    elif valid == 0 and total > 0:
        collector.add(
            ValidationIssue(
                Severity.FATAL,
                "no_valid_rows",
                f"all {total} data row(s) were rejected",
            )
        )
    if truncated_by_rows:
        collector.add(
            ValidationIssue(
                Severity.WARNING,
                "row_limit_reached",
                f"stopped after {max_rows} row(s); later rows were not validated",
            )
        )

    return ValidationReport(
        path=target,
        file_exists=True,
        file_size_bytes=size,
        header=tuple(header),
        total_rows=total,
        valid_rows=valid,
        invalid_rows=invalid,
        issues=collector.issues,
        truncated_issues=collector.truncated,
    )


def ensure_valid(report: ValidationReport, *, allow_warnings: bool = True) -> ValidationReport:
    """Raise a classified :class:`ImuLabError` when ``report`` is not usable.

    Returns the report unchanged when it is usable, so it can be used inline::

        report = ensure_valid(validate_csv(path))
    """
    if not report.file_exists:
        raise InputMissingError.for_path(report.path, role="input CSV")

    fatal = report.fatal_issues
    if fatal:
        codes = {issue.code for issue in fatal}
        if codes == {"missing_columns"} or "missing_columns" in codes:
            raise DataFormatError(
                f"{report.path}: missing required column(s)",
                hints=[issue.message for issue in fatal],
                context={"issues": [i.as_dict() for i in fatal]},
            )
        if codes & {"no_valid_rows", "no_data_rows", "empty_file", "empty_header"}:
            raise EmptyDataError.for_source(report.path, skipped_rows=report.invalid_rows)
        raise DataFormatError(
            f"{report.path}: unusable input",
            hints=[issue.message for issue in fatal],
            context={"issues": [i.as_dict() for i in fatal]},
        )

    if not allow_warnings and report.warning_issues:
        raise DataFormatError(
            f"{report.path}: {len(report.warning_issues)} warning(s) found in strict mode",
            hints=[issue.message for issue in report.warning_issues],
        )
    return report


def iter_valid_rows(
    path: str | Path,
) -> Iterator[tuple[int, dict[str, float]]]:
    """Yield ``(row_number, values)`` for rows that pass every check.

    ``values`` always contains ``elapsed_s``, ``pitch_deg``, ``roll_deg`` and
    optionally ``x``/``y``. Invalid rows are skipped silently; use
    :func:`validate_csv` first if you need to explain the rejections.
    """
    for row_number, row, _reason in _iter_decoded_rows(Path(path)):
        if row is not None:
            yield row_number, row


# --------------------------------------------------------------------------- #
# Internals
# --------------------------------------------------------------------------- #
def _scan_rows(
    path: Path,
    collector: _IssueCollector,
    *,
    max_rows: int | None,
) -> tuple[list[str], int, int, int, bool]:
    header: list[str] = []
    total = valid = invalid = 0
    truncated = False
    previous_elapsed: float | None = None

    for row_number, row, reason in _iter_decoded_rows(path):
        total += 1
        if max_rows is not None and total > max_rows:
            total -= 1
            truncated = True
            break

        if row is None:
            invalid += 1
            if reason is not None:
                collector.add(replace(reason, row_number=row_number))
            continue

        if previous_elapsed is not None:
            if row["elapsed_s"] < previous_elapsed:
                collector.add(
                    ValidationIssue(
                        Severity.WARNING,
                        "time_not_monotonic",
                        f"elapsed_s went backwards ({row['elapsed_s']} < {previous_elapsed})",
                        row_number=row_number,
                        field="elapsed_s",
                    )
                )
            elif row["elapsed_s"] == previous_elapsed:
                collector.add(
                    ValidationIssue(
                        Severity.WARNING,
                        "duplicate_timestamp",
                        f"elapsed_s repeats the previous value ({row['elapsed_s']})",
                        row_number=row_number,
                        field="elapsed_s",
                    )
                )
        previous_elapsed = row["elapsed_s"]
        valid += 1

    # The header is read inside _iter_decoded_rows; re-read it cheaply for the
    # report because the generator has no way to hand it back.
    try:
        with path.open("r", newline="", encoding="utf-8-sig") as stream:
            header = next(csv.reader(stream), [])
    except OSError:
        header = []

    return header, total, valid, invalid, truncated


def _iter_decoded_rows(
    path: Path,
) -> Iterator[tuple[int, dict[str, float] | None, ValidationIssue | None]]:
    """Yield ``(row_number, values_or_None, issue_or_None)`` for each data row.

    ``row_number`` is 1-based and counts the header, matching what a user sees
    in Excel or an editor.
    """
    try:
        stream = path.open("r", newline="", encoding="utf-8-sig")
    except FileNotFoundError:
        return
    with stream:
        reader = csv.DictReader(stream)
        for index, raw in enumerate(reader, start=2):
            yield index, *_decode_row(raw)


def _decode_row(raw: dict[str | None, object]) -> tuple[dict[str, float] | None, ValidationIssue | None]:
    # DictReader parks overflow values under the ``None`` key.
    overflow = raw.get(None)
    if overflow:
        extra = overflow if isinstance(overflow, list) else [overflow]
        return None, ValidationIssue(
            Severity.ERROR,
            "too_many_fields",
            f"row has {len(extra)} more field(s) than the header declares "
            "(an unquoted comma inside raw_line is the usual cause)",
        )

    # --- text columns: present and non-empty, no numeric parsing -------------
    for column in REQUIRED_TEXT_COLUMNS:
        cell = raw.get(column)
        if cell is None:
            return None, ValidationIssue(
                Severity.ERROR,
                "missing_column_in_row",
                f"column {column!r} is absent from this row "
                f"(the row is shorter than the header, or the header uses different names)",
                field=column,
            )
        if not str(cell).strip():
            return None, ValidationIssue(
                Severity.ERROR, "empty_field", f"{column!r} is empty", field=column
            )

    # --- numeric columns: finite numbers, then the elapsed_s sign rule -------
    values: dict[str, float] = {}
    for column in REQUIRED_NUMERIC_COLUMNS:
        cell = raw.get(column)
        if cell is None:
            return None, ValidationIssue(
                Severity.ERROR,
                "missing_column_in_row",
                f"column {column!r} is absent from this row "
                f"(the row is shorter than the header, or the header uses different names)",
                field=column,
            )
        if not str(cell).strip():
            return None, ValidationIssue(
                Severity.ERROR, "empty_field", f"{column!r} is empty", field=column
            )
        try:
            number = float(str(cell).strip())
        except ValueError:
            return None, ValidationIssue(
                Severity.ERROR,
                "not_numeric",
                f"{column!r} is not a number",
                field=column,
                value=str(cell).strip(),
            )
        if not math.isfinite(number):
            return None, ValidationIssue(
                Severity.ERROR,
                "not_finite",
                f"{column!r} is NaN or infinite",
                field=column,
                value=str(cell).strip(),
            )
        values[column] = number

    if values["elapsed_s"] < 0:
        return None, ValidationIssue(
            Severity.ERROR,
            "negative_elapsed",
            f"elapsed_s is negative ({values['elapsed_s']})",
            field="elapsed_s",
            value=str(values["elapsed_s"]),
        )

    optional: dict[str, float] = {}
    for column in ("x", "y"):
        cell = raw.get(column)
        if cell is None or not str(cell).strip():
            continue
        try:
            number = float(str(cell).strip())
        except ValueError:
            return None, ValidationIssue(
                Severity.ERROR,
                "not_numeric",
                f"optional column {column!r} is not a number",
                field=column,
                value=str(cell),
            )
        if not math.isfinite(number):
            return None, ValidationIssue(
                Severity.ERROR,
                "not_finite",
                f"optional column {column!r} is NaN or infinite",
                field=column,
                value=str(cell),
            )
        optional[column] = number

    if ("x" in optional) != ("y" in optional):
        return None, ValidationIssue(
            Severity.ERROR,
            "incomplete_position_pair",
            "experimental position requires both x and y, or neither",
        )

    values.update(optional)
    return values, None


def summarize_issues(issues: Iterable[ValidationIssue], *, limit: int = 5) -> list[str]:
    """Return up to ``limit`` short human-readable strings for ``issues``."""
    return [issue.render() for issue in list(issues)[:limit]]
