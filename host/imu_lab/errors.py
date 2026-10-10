"""Unified error types and process exit codes for IMU Attitude Lab.

Why this module exists
----------------------
Before this module each entry point caught ``(OSError, ValueError)`` and printed
its own ad-hoc message, which produced inconsistent wording and made it
impossible for scripts or CI to branch on the failure kind.

Every user-facing failure is now an :class:`ImuLabError` that carries:

* ``exit_code``  - a stable process exit code (see :class:`ExitCode`)
* ``message``    - a one-line human summary
* ``hints``      - concrete next steps the user can try
* ``context``    - machine-readable details for logs/JSON

Low-level helpers (``host.imu_lab.protocol``, ``csvlog``, ``reporting``) keep
raising plain ``ValueError`` because they are library code. The boundary layer
(CLI entry points, the pipeline) converts them with :func:`translate` so the
library stays easy to unit test while the CLI stays friendly.
"""

from __future__ import annotations

from enum import IntEnum
from typing import Callable, Iterable, Mapping


class ExitCode(IntEnum):
    """Stable exit codes shared by every command-line entry point."""

    OK = 0
    #: Command ran correctly but there was nothing usable to report on.
    NO_DATA = 1
    #: Bad arguments or an invalid configuration; nothing was attempted.
    USAGE = 2
    #: A required file exists but its content is malformed.
    DATA_INVALID = 3
    #: A required file, directory or sample dataset is missing.
    INPUT_MISSING = 4
    #: A device (serial port, sensor, COM port) cannot be opened or read.
    SOURCE_UNAVAILABLE = 5
    #: An unexpected internal failure; a bug rather than bad user input.
    INTERNAL = 10


class ImuLabError(Exception):
    """Base class for every expected, user-facing failure."""

    exit_code: ExitCode = ExitCode.INTERNAL

    def __init__(
        self,
        message: str,
        *,
        hints: Iterable[str] | None = None,
        context: Mapping[str, object] | None = None,
    ) -> None:
        super().__init__(message)
        self.message = message
        self.hints: list[str] = [str(hint) for hint in (hints or [])]
        self.context: dict[str, object] = dict(context or {})

    def __str__(self) -> str:  # pragma: no cover - trivial
        return self.message

    def render(self, *, indent: str = "  ") -> str:
        """Return a multi-line, user-facing description of this failure."""
        lines = [f"{self.message}"]
        if self.hints:
            lines.append("How to fix:")
            lines.extend(f"{indent}- {hint}" for hint in self.hints)
        return "\n".join(lines)

    def as_dict(self) -> dict[str, object]:
        """Return a JSON-serialisable description for structured logs."""
        return {
            "error": type(self).__name__,
            "message": self.message,
            "exit_code": int(self.exit_code),
            "hints": list(self.hints),
            "context": dict(self.context),
        }


class UsageError(ImuLabError, ValueError):
    """The user passed arguments that cannot produce a meaningful run.

    Also inherits :class:`ValueError` so existing library call sites and tests
    that raise/catch ``ValueError`` for bad arguments keep working unchanged.
    """

    exit_code = ExitCode.USAGE


class InputMissingError(ImuLabError):
    """A required input file, directory or dataset does not exist."""

    exit_code = ExitCode.INPUT_MISSING

    @classmethod
    def for_path(cls, path: object, *, role: str = "input file") -> "InputMissingError":
        return cls(
            f"{role} not found: {path}",
            hints=[
                "check the path for typos and use forward slashes or quotes on Windows",
                f'generate a sample dataset first: python -m host.pipeline --source simulated --output-dir reports/demo',
            ],
            context={"path": str(path), "role": role},
        )


class DataFormatError(ImuLabError, ValueError):
    """A file was found but its content does not match the expected schema.

    Also inherits :class:`ValueError` for backwards compatibility with callers
    that already catch ``ValueError`` around parsing helpers.
    """

    exit_code = ExitCode.DATA_INVALID

    def __init__(
        self,
        message: str,
        *,
        hints: Iterable[str] | None = None,
        context: Mapping[str, object] | None = None,
        row_number: int | None = None,
    ) -> None:
        super().__init__(message, hints=hints, context=context)
        self.row_number = row_number
        if row_number is not None:
            self.context.setdefault("row_number", row_number)


class EmptyDataError(ImuLabError):
    """The input was structurally valid but contained no usable samples."""

    exit_code = ExitCode.NO_DATA

    @classmethod
    def for_source(cls, source: object, *, skipped_rows: int = 0) -> "EmptyDataError":
        hints = [
            "verify the MCU is powered, wired to the correct RX/TX pins and printing P:/R: lines",
            "run with --source simulated to confirm the rest of the pipeline works",
        ]
        if skipped_rows:
            hints.insert(0, f"{skipped_rows} row(s) were dropped as malformed; inspect the raw log")
        return cls(
            f"no valid samples were produced from {source}",
            hints=hints,
            context={"source": str(source), "skipped_rows": skipped_rows},
        )


class SourceUnavailableError(ImuLabError):
    """A data source (serial port, sensor, remote feed) cannot be used."""

    exit_code = ExitCode.SOURCE_UNAVAILABLE

    @classmethod
    def for_serial(cls, port: str, reason: object) -> "SourceUnavailableError":
        return cls(
            f"cannot open or read serial port {port!r}: {reason}",
            hints=[
                "close other programs that may hold the port (Arduino IDE, PuTTY, another recorder)",
                "confirm the USB-serial driver is installed and the port name matches (COM3, /dev/ttyUSB0)",
                f"list available ports: python -c \"import serial.tools.list_ports as p; print([x.device for x in p.comports()])\"",
                "run with --source simulated to continue without hardware",
            ],
            context={"port": port, "reason": str(reason)},
        )

    @classmethod
    def for_missing_dependency(cls, package: str, reason: object) -> "SourceUnavailableError":
        return cls(
            f"optional dependency {package!r} is not available: {reason}",
            hints=[
                "install it with: python -m pip install -r requirements.txt",
                "use --source simulated or --source csv to work fully offline",
            ],
            context={"package": package, "reason": str(reason)},
        )


#: Backwards-compatible alias. Older modules import ``SourceError`` from
#: ``host.imu_lab.sources``; that name now points at this class so the error
#: taxonomy stays in one place.
SourceError = SourceUnavailableError


class StageError(ImuLabError):
    """A named pipeline stage failed.

    Wraps the original cause so the pipeline can report *which* step broke while
    preserving the original exception for debugging (``__cause__``).
    """

    exit_code = ExitCode.INTERNAL

    def __init__(self, stage: str, cause: BaseException) -> None:
        translated = translate(cause)
        super().__init__(
            f"stage {stage!r} failed: {translated.message}",
            hints=translated.hints,
            context={"stage": stage, **translated.context},
        )
        self.stage = stage
        self.exit_code = translated.exit_code
        if isinstance(cause, BaseException):
            self.__cause__ = cause

    def as_dict(self) -> dict[str, object]:
        payload = super().as_dict()
        payload["stage"] = self.stage
        return payload


def translate(exc: BaseException) -> ImuLabError:
    """Normalise any exception into an :class:`ImuLabError`.

    Already-classified errors pass through unchanged, which makes the function
    safe to call at multiple layers.
    """
    if isinstance(exc, ImuLabError):
        return exc
    if isinstance(exc, FileNotFoundError):
        return InputMissingError(
            f"file not found: {exc.filename or exc}",
            hints=["check the path and working directory"],
            context={"path": str(exc.filename or "")},
        )
    if isinstance(exc, IsADirectoryError):
        return DataFormatError(
            f"expected a file but found a directory: {exc.filename or exc}",
            hints=["pass a file path such as data/normalized.csv"],
        )
    if isinstance(exc, PermissionError):
        return ImuLabError(
            f"permission denied: {exc.filename or exc}",
            hints=["close the file if it is open in Excel or an editor", "choose an output path you can write to"],
            context={"path": str(exc.filename or "")},
        )
    if isinstance(exc, UnicodeDecodeError):
        return DataFormatError(
            f"file is not valid UTF-8 text: {exc}",
            hints=["re-export the log as UTF-8 CSV", "the loader also accepts UTF-8 with BOM"],
        )
    if isinstance(exc, OSError):
        return ImuLabError(
            f"operating-system error: {exc}",
            hints=["check that the path exists and is writable"],
        )
    if isinstance(exc, ValueError):
        return DataFormatError(
            str(exc) or "invalid value",
            hints=["run `python -m host.pipeline validate <file>` to see row-level problems"],
        )
    return ImuLabError(f"unexpected {type(exc).__name__}: {exc}")


def exit_code_for(exc: BaseException) -> int:
    """Return the process exit code appropriate for ``exc``."""
    return int(translate(exc).exit_code)


def render_failure(prefix: str, exc: BaseException) -> str:
    """Render ``exc`` as a labelled, multi-line CLI message."""
    return f"{prefix}: {translate(exc).render()}"


def guarded(prefix: str, action: "Callable[[], int]") -> int:
    """Run ``action`` and convert any failure into a message plus exit code.

    Every command-line entry point funnels through this so that behaviour is
    identical across tools: classified errors print their hints and return the
    matching code, unexpected errors are labelled as such, and
    ``KeyboardInterrupt`` still propagates so Ctrl+C stays responsive.

    Example::

        def main(argv=None) -> int:
            args = build_parser().parse_args(argv)
            return guarded("host.analyze", lambda: _run(args))
    """
    try:
        return int(action())
    except ImuLabError as exc:
        print(render_failure(prefix, exc), file=_stderr())
        return int(exc.exit_code)
    except Exception as exc:  # noqa: BLE001 - deliberate boundary conversion
        failure = translate(exc)
        print(render_failure(f"unexpected error in {prefix}", failure), file=_stderr())
        return int(failure.exit_code)


def _stderr():
    import sys

    return sys.stderr


def require(condition: bool, message: str, *, hints: Iterable[str] | None = None, error=UsageError) -> None:
    """Raise ``error(message)`` unless ``condition`` holds.

    Small readability helper for argument checks::

        require(speed > 0, "--speed must be greater than zero")
    """
    if not condition:
        raise error(message, hints=hints)
