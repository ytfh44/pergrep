# Extended-VM resource telemetry (M6.7)

**Status:** Accepted (measured before optimizer shortcuts)
**Scope:** M6.7 / GitHub issue #69

## What

Per-search counters for the extended VM (lookaround/backreference evaluation),
reported through `SearchStats.vm_*` whenever stats are requested. The general
NFA path does not use `eval`, so non-extended queries correctly report zeros.
Failure reasons surface through exceptions with a fixed taxonomy (below).

## Fields (`SearchStats`)

- `vm_max_depth`: deepest `eval` recursion depth reached.
- `vm_repeat_iterations`: total bounded-repeat iterations executed.
- `vm_repeat_capped`: times a repeat hit its 10000-iteration cap (matches
  returned are valid prefixes, never silently dropped).
- `vm_lookbehind_evals`: lookbehind evaluations performed.
- `vm_max_lookbehind_window`: widest `s.pos - lo` window scanned.
- `vm_lookbehind_capped`: evaluations where `s.pos > 8192` clamped the window.
- `vm_state_expansions`: total output states produced across `eval` calls.
- Bytes scanned maps to the existing `verified_bytes`
  (`physically_touched_bytes`); no duplicate counter is kept.

## Bounds (all documented, all visible)

- Lookbehind window: 8192 bytes/code-units (`lo = max(floor, s.pos-8192)`).
- Repeat: 10000 iterations (`min(max,10000)`, `min(hard,10000)`).
- Recursion depth: throws past 10000.
- VM states: throws past 50000 per collection.
- Implementation: `detail::VmTelemetry` aggregated per search via
  `VerifierContext::tm` (null when stats unrequested — zero hot-path cost);
  copied out in `Searcher::find`'s stats finalization. Per-file sub-searches
  under parallel execution carry independent accumulators; parallel mode
  requires null stats (M5.4), so no cross-thread aggregation exists.

## Failure taxonomy (deterministic, surfaced, never silent)

- `recursion-depth`: `depth > 10000` throws `runtime_error`. Pinned by
  `test_eval_depth_guard` boundary tests (10001 throws, 10000 does not).
- `vm-state-limit`: any collection past 50000 states throws. Loud, never a
  truncated result.
- Repeat/lookbehind caps do not throw: repeat returns the valid bounded
  prefix; lookbehind clamping is counted (`vm_lookbehind_capped`,
  `vm_repeat_capped`) instead of silent.
- **Known residual risk (visible, not silent):** a lookbehind whose required
  start precedes the clamped 8192 window (need wider than 8192 back) can miss
  a true match. The cap-hit counter exposes every occurrence; the bounded
  planner path already excludes limit-applied patterns. Exact wide-lookbehind
  evaluation is future work — the bound is now measured, not assumed.

## Planner eligibility

`bounded_regex_eligible` excludes lookahead, lookbehind, backreference,
unbounded repeats, and limit-applied analyses, so the one-pass path never
touches unbounded context (pinned per construct in `test_m67_vm_telemetry`).
M2 owns region fallback policy.

## Evidence anchors

- [`src/internal.hpp`](../src/internal.hpp): `VmTelemetry`, context field.
- [`src/regex.cpp`](../src/regex.cpp): `eval` instrumentation, throw sites.
- [`src/search.cpp`](../src/search.cpp): attach points, stats copyout.
- [`tests/test.cpp`](../tests/test.cpp): `test_m67_vm_telemetry`.