#!/usr/bin/env bash
# Configure and build the C++ core.
#
# Works on Ubuntu/WSL2 (GCC or Clang) and on Windows under Git Bash with
# MinGW-w64 on PATH. No third-party dependency is downloaded.
#
# Usage:
#   scripts/build.sh                 # Release, warnings as errors
#   scripts/build.sh Debug           # Debug (needed for GDB line stepping)
#   scripts/build.sh Release address+undefined
set -euo pipefail

BUILD_TYPE="${1:-Release}"
SANITIZER="${2:-}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP_DIR="$(dirname "${SCRIPT_DIR}")"
BUILD_DIR="${CPP_DIR}/build"

CMAKE_ARGS=(
  -S "${CPP_DIR}"
  -B "${BUILD_DIR}"
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
  -DIMU_LAB_WERROR=ON
  -DIMU_LAB_BUILD_BENCH=ON
)

if [[ -n "${SANITIZER}" ]]; then
  CMAKE_ARGS+=(-DIMU_LAB_SANITIZER="${SANITIZER}")
fi

# Prefer Ninja when present; fall back to Make.
if command -v ninja >/dev/null 2>&1; then
  CMAKE_ARGS+=(-G Ninja)
fi

echo "==> cmake ${CMAKE_ARGS[*]}"
cmake "${CMAKE_ARGS[@]}"

echo "==> building"
cmake --build "${BUILD_DIR}" --parallel

echo
echo "Binaries in ${BUILD_DIR}/bin:"
ls -1 "${BUILD_DIR}/bin" 2>/dev/null || true
