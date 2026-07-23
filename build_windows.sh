#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

cmake -S . -B build \
  -DARCHSTREAMER_BUILD_HOST=OFF \
  -DCMAKE_TOOLCHAIN_FILE=/c/dev/vcpkg/scripts/buildsystems/vcpkg.cmake

cmake --build build --config Release -j2
