#!/usr/bin/env bash
# Run the C++ unit tests and the integrity checks.
#
#   scripts/run_tests.sh              # ctest, all suites
#   scripts/run_tests.sh Pipeline     # only tests whose name matches "Pipeline"
#
# The script also drives the binaries directly because CTest alone would not
# show a failing assertion's value on some dashboards.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP_DIR="$(dirname "${SCRIPT_DIR}")"
BUILD_DIR="${CPP_DIR}/build"
BIN_DIR="${BUILD_DIR}/bin"

if [[ ! -d "${BUILD_DIR}" ]]; then
  echo "error: ${BUILD_DIR} does not exist; run scripts/build.sh first" >&2
  exit 2
fi

filter="${1:-}"

echo "==> ctest"
ctest --test-dir "${BUILD_DIR}" --output-on-failure

echo
echo "==> unit tests (verbose assertions)"
"${BIN_DIR}/imu_lab_tests" ${filter:+"${filter}"}

echo
echo "==> stability sweep (1/2/4 workers must agree)"
"${BIN_DIR}/imu_lab_bench" --frames 20000 --sweep --queue-capacity 128

echo
echo "==> queue integrity"
"${BIN_DIR}/imu_lab_bench_queue" --items 200000 --queue-capacity 1024

echo
echo "All C++ checks passed."
