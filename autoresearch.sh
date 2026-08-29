#!/usr/bin/env bash
set -euo pipefail

# Build the benchmark binary
cmake --build --preset windows-clang --target pergrep_bench > /dev/null 2>&1

# Run the benchmark binary
./build/windows-clang/pergrep_bench.exe
