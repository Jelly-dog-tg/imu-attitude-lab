# Unified data input pipeline

The host-side application now normalizes data from three sources into the same
`SourceRecord` structure:

- `simulated`: reproducible synthetic pitch/roll values (no hardware needed)
- `csv`: valid records from a previous experiment log
- `serial`: the current STM32 firmware line format, `P:<pitch>,R:<roll>`

The `host.collect` command writes each source to the existing CSV format. This
keeps later analysis/replay code independent of where records originated.

## Commands

Generate a simulated test recording:

```powershell
python -m host.collect --source simulated --duration 5 --hz 20 --seed 7 --output data/simulated.csv
```

Normalize a previous CSV into a new file (source and output must differ):

```powershell
python -m host.collect --source csv --input data/demo_attitude_offline.csv --output data/normalized.csv
```

Record from a real serial port when the device is available:

```powershell
python -m host.collect --source serial --port COM3 --baud 9600 --duration 30 --output data/session.csv
```

Analyze or replay the output with the existing tools:

```powershell
python -m host.analyze data/simulated.csv
python -m host.replay data/simulated.csv --no-wait --limit 10
```

## What is and is not verified

Unit tests verify normalization, deterministic simulation, CSV compatibility, and
malformed-row handling. The serial adapter is not hardware-tested in an offline
environment; real serial stability still requires connecting the STM32 board.
Synthetic values are for software testing only and must not be reported as sensor
accuracy measurements.
