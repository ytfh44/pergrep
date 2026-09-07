# Index Size and Memory Regression Gates (M8.7)

**Status:** Accepted contract
**Scope:** M8.7 / GitHub issue #84
**Owner:** M8; M9 consumes these gates for release decisions

## Purpose

Prevent performance optimizations from introducing regressions in index size, memory footprint (RSS), or build times.
Enforce workload-specific budgets across all index components and build stages.

## Gate Metrics & Budgets

The release gate enforces both absolute bounds and relative regression ratios against established baselines:

| Metric | Budget / Baseline (M0) | Regression Threshold | Rollback Threshold |
|---|---|---|---|
| **Search p50 Latency** | Workload-dependent (0.8 - 2.0 ms) | +5% | +15% |
| **Search p95 Latency** | Workload-dependent (2.5 - 5.0 ms) | +10% | +25% |
| **Search Time** | Workload-dependent | +8% | N/A |
| **Steady RSS** | 8 - 32 MB | +15% | +30% |
| **Build Peak RSS** | 16 - 64 MB | +15% | +30% |
| **Filter Size** | Calculated per workload | +15% | +30% |
| **Persisted Size** | Calculated per workload | +15% | +30% |
| **Build Time** | 500 - 2000 ms | +8% | N/A |
| **Fallback Rate** | Baseline: 0% | 25% | 50% |
| **Plan Regret** | Baseline: 0% | 15% | 40% |

## Measurement & Enforcement

1. **Separation of Concerns:**
   - Corpus memory is reported separately from index memory structures.
   - Filter structures (`groups`, `folded_groups`, `positional`) are budgeted separately from overhead.
   - Resident and persistent snapshots are tracked independently.

2. **Workload Scenarios:**
   Each scenario defines its own baselines based on corpus characteristics and query patterns:
   - `oneshot.cold.rare-short`: Baseline for small, cold searches.
   - `warm-repeated.medium.rare-long-unicode`: Tests steady-state repeated query performance.
   - `interactive.large-repository.filtered`: Tests large-scale indexing and scoped querying.
   - `batch.multi-pattern.mixed`: Tests multi-query throughput and memory scaling.

3. **Gate Status Classifications:**
   - **Pass:** All metrics within acceptable thresholds.
   - **Warn:** Non-critical metrics show minor regression.
   - **Fail:** One or more metrics exceed regression threshold.
   - **Rollback:** Critical regression detected; representation change must be rejected.

## Integration with CI

The regression gates run as part of the `bench-bounds` CI job and during release validation. Any representation or CPU optimization that violates these gates will block the release.

## Evidence Anchors

- [`include/pergrep/pergrep.hpp`](../include/pergrep/pergrep.hpp): `ScenarioBaseline` and `PerformanceGateThresholds`.
- [`bench/workload_matrix.hpp`](../bench/workload_matrix.hpp): Gate evaluation logic and default baselines.
- [`bench/bench.cpp`](../bench/bench.cpp): Metric collection and scenario measurement.
- [`tests/test.cpp`](../tests/test.cpp): `test_m87_regression_gates`.
