# Design notes

## Goals

1. Keep the on-device wire format small and parseable by a terminal.
2. Separate data protocol, data source, visualization, and logging concerns.
3. Allow the PC-side code to be developed before the physical board is ready.
4. Make each milestone demonstrable and testable.

## Current data flow

`synthetic sample generator -> protocol formatter -> newline-delimited stream file`

The next host-side milestone will reverse this path (`serial input -> parser -> rolling plot + CSV logger`). The embedded milestone will implement the same protocol on STM32 after raw sensor acquisition and attitude estimation are integrated.

## Reliability considerations to add

- Sequence gap detection and timestamp monotonicity checks
- Invalid-line counters and bounded reconnect retries
- Sensor calibration metadata and configuration file
- Unit tests for angle wrapping, filter behavior, and malformed input
- Benchmarks for sampling rate and serial throughput
