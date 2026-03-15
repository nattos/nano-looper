#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" "$@"
