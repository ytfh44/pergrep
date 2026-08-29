#!/usr/bin/env bash
set -euo pipefail

if command -v cmd.exe > /dev/null 2>&1; then
    cmd.exe /c "cd /d D:\\PROJECTS\\pergrep && cmake --build --preset windows-clang --target pergrep_bench > NUL 2>&1 && .\\build\\windows-clang\\pergrep_bench.exe"
else
    cmake --build --preset windows-clang --target pergrep_bench > /dev/null 2>&1
    ./build/windows-clang/pergrep_bench.exe
fi
