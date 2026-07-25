#!/usr/bin/env bash
# Incremental Windows CLIENT build (Git Bash / MSYS). Prefer build_windows.ps1 on PowerShell.
# No Linux host deps (gamescope/WSI/Yuzu). See deploy/windows/README.md.
# Usage:
#   ./build_windows.sh              # incremental
#   ./build_windows.sh --reconfigure
#   ./build_windows.sh --clean
set -euo pipefail
cd "$(dirname "$0")"

VCPKG_ROOT="${VCPKG_ROOT:-/c/dev/vcpkg}"
TOOLCHAIN="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
RECONFIGURE=0
CLEAN=0
for arg in "$@"; do
  case "$arg" in
    --reconfigure) RECONFIGURE=1 ;;
    --clean) CLEAN=1 ;;
  esac
done

if [[ ! -f "$TOOLCHAIN" ]]; then
  echo "vcpkg toolchain not found: $TOOLCHAIN" >&2
  echo "Set VCPKG_ROOT or install vcpkg under C:\\dev\\vcpkg" >&2
  exit 1
fi

if [[ "$CLEAN" -eq 1 && -d build ]]; then
  echo "Cleaning build/"
  rm -rf build
fi

if [[ "$RECONFIGURE" -eq 1 || "$CLEAN" -eq 1 || ! -f build/CMakeCache.txt ]]; then
  echo "Configuring CMake..."
  cmake -S . -B build \
    -DARCHSTREAMER_BUILD_HOST=OFF \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN"
else
  echo "Reusing existing build/CMakeCache.txt (pass --reconfigure to refresh)."
fi

JOBS="${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
echo "Building Release (-j${JOBS})..."
cmake --build build --config Release --parallel "$JOBS"
