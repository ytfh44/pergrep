# Rollout, Rollback, and Fallback Profiles (M9.7)

**Status:** Accepted contract
**Scope:** M9.7 / GitHub issue #91
**Owner:** M9 (Offline autotuning & release gates); module owners maintain their baseline fallbacks

## Purpose

Ship experimental operators and tuned configurations without making them irreversible defaults.
Provide fine-grained runtime feature toggles, automated telemetry-driven safety guards, non-destructive rollback mechanisms, and operational runbooks.

## Deployment Lifecycle States

Every tuned profile and operator cluster transitions through a formal lifecycle:

| State | Description | Safety Invariant |
|---|---|---|
| `DefaultBaseline` | Fully qualified, conservative baseline configuration | Zero risk; scalar and reference paths only |
| `CanaryExperimental` | Opt-in experimental evaluation | Telemetry active; monitored against fallback thresholds |
| `RolloutActive` | Promoted production candidate | Subject to automated circuit-breaker rollback |
| `RollbackEnforced` | Emergency demotion to baseline defaults | Experimental features disabled; no data loss |

## Feature Flags & Granular Disablement

`RolloutFeatureFlags` permits disabling individual performance optimizations at runtime:
- `enable_simd_bitmaps`: Disable AVX2/SIMD bitmap intersection (reverts to scalar loop)
- `enable_positional_encoding`: Disable positional block filtering (reverts to full document verifier)
- `enable_sparse_postings`: Disable sparse posting lists (reverts to dense bitsets)
- `enable_aho_corasick_prefilter`: Disable multi-pattern Aho-Corasick scan (reverts to sequential scan)
- `enable_autotuned_layout`: Disable tuned chunk/block parameters (reverts to conservative 16KB/128B)
- `enable_parallel_build`: Disable multi-threaded indexing (reverts to deterministic serial build)
- `enable_lazy_dfa`: Disable lazy DFA execution (reverts to PikeVM NFA)

## Automated Safety Guards & Circuit Breakers

The `RolloutTelemetry` monitor tracks:
1. `fallback_ratio`: Fraction of queries requiring fallback to slower or safer operators.
2. `max_allowed_fallbacks`: Hard ceiling on absolute fallback incidents.

If `fallback_ratio > 5%` or total fallbacks exceed thresholds, `RolloutPolicyController::evaluate_safety_guard()` automatically:
- Transitions state to `RollbackEnforced`
- Invokes `disable_all_experimental()`
- Logs the exact triggering condition in `rollback_reason`
- Keeps all indexes readable and all queries functioning via baseline paths.

## Evidence Anchors

- [`include/pergrep/rollout.hpp`](../include/pergrep/rollout.hpp): Public interface (`RolloutState`, `RolloutFeatureFlags`, `FallbackEvent`, `RolloutTelemetry`, `RolloutPolicyController`).
- [`src/rollout.cpp`](../src/rollout.cpp): Rollout controller logic, circuit-breaker evaluation, and release notes generator.
- [`tests/test.cpp`](../tests/test.cpp): `test_m97_rollout_and_rollback()`.
