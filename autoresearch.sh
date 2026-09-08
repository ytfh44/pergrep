#!/usr/bin/env bash
set -euo pipefail

readonly script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly repo_root="$script_dir"
readonly bench_exe="$repo_root/build/windows-clang/pergrep_bench.exe"
readonly target_scenario='warm-repeated.medium.rare-long-unicode'

tmp_output=''
build_output=''
cleanup() {
    [[ -z "${tmp_output:-}" ]] || rm -f -- "$tmp_output"
    [[ -z "${build_output:-}" ]] || rm -f -- "$build_output"
}
trap cleanup EXIT

tmp_output="$(mktemp "${TMPDIR:-/tmp}/pergrep-bench.XXXXXX")"
build_output="$(mktemp "${TMPDIR:-/tmp}/pergrep-build.XXXXXX")"

cd -- "$repo_root"
if command -v pwsh.exe >/dev/null 2>&1; then
    bridge=pwsh
elif command -v powershell.exe >/dev/null 2>&1; then
    bridge=powershell
elif command -v cmd.exe >/dev/null 2>&1; then
    bridge=cmd
else
    bridge=posix
fi

run_build() {
    case "$bridge" in
        pwsh)
            pwsh.exe -NoProfile -NonInteractive -Command '& cmake --build --preset windows-clang --target pergrep_bench'
            ;;
        powershell)
            powershell.exe -NoProfile -NonInteractive -Command '& cmake --build --preset windows-clang --target pergrep_bench'
            ;;
        cmd)
            cmd.exe /d /c "cmake --build --preset windows-clang --target pergrep_bench"
            ;;
        posix)
            cmake --build --preset windows-clang --target pergrep_bench
            ;;
    esac
}

run_benchmark() {
    case "$bridge" in
        pwsh)
            pwsh.exe -NoProfile -NonInteractive -Command '& .\build\windows-clang\pergrep_bench.exe'
            ;;
        powershell)
            powershell.exe -NoProfile -NonInteractive -Command '& .\build\windows-clang\pergrep_bench.exe'
            ;;
        cmd)
            cmd.exe /d /c "build\windows-clang\pergrep_bench.exe"
            ;;
        posix)
            "$bench_exe"
            ;;
    esac
}

if run_build >"$build_output" 2>&1; then
    :
else
    status=$?
    cat "$build_output" >&2
    printf 'autoresearch: benchmark build failed (status %s)\n' "$status" >&2
    exit "$status"
fi

if [[ ! -f "$bench_exe" ]]; then
    printf 'autoresearch: benchmark executable not found: %s\n' "$bench_exe" >&2
    exit 1
fi

if run_benchmark >"$tmp_output" 2>&1; then
    :
else
    status=$?
    cat "$tmp_output" >&2
    printf 'autoresearch: benchmark run failed (status %s)\n' "$status" >&2
    exit "$status"
fi

parsed_metrics="$(awk -v target="$target_scenario" '
BEGIN {
    number = "^[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)([eE][+-]?[0-9]+)?$"
    scenario_count = 0
    metric_count = 0
    aggregate_gate_count = 0
    aggregate_pass_count = 0
    target_count = 0
    bad = 0
}

function clear_values(   key) {
    for (key in values) delete values[key]
}

function read_token(token,   equals, key, value) {
    sub(/\r$/, "", token)
    equals = index(token, "=")
    if (equals <= 1) return 0
    key = substr(token, 1, equals - 1)
    value = substr(token, equals + 1)
    if (key in values) return 0
    values[key] = value
    return 1
}

$1 == "SCENARIO" {
    name = ""
    for (i = 2; i <= NF; ++i) {
        if ($i ~ /^name=/) {
            name = substr($i, 6)
            sub(/\r$/, "", name)
            break
        }
    }
    ++scenario_count
    if (name == "" || name in scenarios) bad = 1
    else scenarios[name] = 1
    next
}

$1 == "METRIC" && $2 ~ /^scenario=/ {
    name = substr($2, 10)
    sub(/\r$/, "", name)
    ++metric_count
    if (name == "" || name in scenario_metrics) bad = 1
    else scenario_metrics[name] = 1

    clear_values()
    for (i = 2; i <= NF; ++i) {
        if (!read_token($i)) bad = 1
    }
    if (!("gate_status" in values)) bad = 1
    if (!("correctness" in values) || values["correctness"] != "pass") bad = 1

    if (name == target) {
        ++target_count
        required_target[1] = "search_ms_per_query"
        required_target[2] = "search_p95_ms"
        required_target[3] = "index_bytes"
        required_target[4] = "matches"
        required_target[5] = "candidate_chunks"
        for (key in required_target) {
            field = required_target[key]
            if (!(field in values) || values[field] !~ number) bad = 1
        }
        if ("search_ms_per_query" in values) target_search = values["search_ms_per_query"]
        if ("search_p95_ms" in values) target_p95 = values["search_p95_ms"]
        if ("index_bytes" in values) target_index = values["index_bytes"]
        if ("matches" in values) target_matches = values["matches"]
        if ("candidate_chunks" in values) target_chunks = values["candidate_chunks"]
    }
    next
}

$1 == "METRIC" && $2 ~ /^gate_status=/ {
    ++aggregate_gate_count
    aggregate_gate = substr($2, 13)
    sub(/\r$/, "", aggregate_gate)
    next
}

$1 == "METRIC" && $2 ~ /^gate_passed=/ {
    ++aggregate_pass_count
    aggregate_passed = substr($2, 13)
    sub(/\r$/, "", aggregate_passed)
    next
}

END {
    gate_status = tolower(aggregate_gate)
    if (gate_status == "pass") release_gate_status = 1
    else if (gate_status == "fail" || gate_status == "neutral" || gate_status == "rollback") release_gate_status = 0
    else bad = 1

    gate_passed = tolower(aggregate_passed)
    if (gate_passed == "true") release_gate_passed = 1
    else if (gate_passed == "false") release_gate_passed = 0
    else bad = 1

    if (scenario_count == 0 || metric_count != scenario_count || target_count != 1 ||
        aggregate_gate_count != 1 || aggregate_pass_count != 1) bad = 1
    for (name in scenarios) if (!(name in scenario_metrics)) bad = 1
    for (name in scenario_metrics) if (!(name in scenarios)) bad = 1
    if (bad) exit 1
    printf "%s\t%s\t%s\t%s\t%s\t1\t%d\t%d\n", target_search, target_p95,
           target_index, target_matches, target_chunks, release_gate_status,
           release_gate_passed
}
' "$tmp_output")" || {
    printf 'autoresearch: invalid benchmark output\n' >&2
    cat "$tmp_output" >&2
    exit 1
}

IFS=$'\t' read -r search_ms_per_query p95_ms index_bytes matches candidate_chunks correctness_guard release_gate_status release_gate_passed <<<"$parsed_metrics"
printf 'METRIC warm_repeated_medium_search_ms_per_query=%s\n' "$search_ms_per_query"
printf 'METRIC warm_repeated_medium_search_p95_ms=%s\n' "$p95_ms"
printf 'METRIC warm_repeated_medium_index_bytes=%s\n' "$index_bytes"
printf 'METRIC warm_repeated_medium_matches=%s\n' "$matches"
printf 'METRIC warm_repeated_medium_candidate_chunks=%s\n' "$candidate_chunks"
printf 'METRIC correctness_guard=%s\n' "$correctness_guard"
printf 'METRIC release_gate_status=%s\n' "$release_gate_status"
printf 'METRIC release_gate_passed=%s\n' "$release_gate_passed"
