# Pareto Selection and Workload Weights (M9.4)

**Status:** Accepted contract
**Scope:** M9.4 / GitHub issue #88
**Owner:** M9 (Offline autotuning & release gates); product owners approve changes to hard limits

## Purpose

Select tuned index representations and planner execution configurations using explicit multi-criteria product priorities rather than an opaque scalar score.
Ensure that performance improvements on one dimension (e.g. search speed) cannot hide violations of critical constraints on other dimensions (e.g. index size or peak RSS).

## Multi-Criteria Evaluation Metrics

Candidate evaluations measure eight key dimensions:

| Metric | Unit | Optimization Direction | Role |
|---|---|---|---|
| `build_time_ms` | Milliseconds | Minimize | Hard limit & weighted loss |
| `index_size_bytes` | Bytes | Minimize | Hard limit & weighted loss |
| `peak_rss_bytes` | Bytes | Minimize | Hard limit & weighted loss |
| `cold_p50_ms` | Milliseconds | Minimize | Hard limit & weighted loss |
| `warm_p50_ms` | Milliseconds | Minimize | Hard limit & primary latency loss |
| `p95_ms` | Milliseconds | Minimize | Hard limit & tail latency loss |
| `verified_bytes` | Bytes | Minimize | Throughput / efficiency metric |
| `fallback_rate` | Fraction $[0, 1]$ | Minimize | Reliability & correctness guard |

## Inviolable Hard Constraints

Before Pareto dominance or loss ranking is evaluated, candidates are strictly checked against `HardConstraintBounds`:
- Candidates exceeding any hard limit (such as `max_index_size_bytes` or `max_fallback_rate`) are unconditionally rejected and recorded in `rejected_hard_limits`.
- No improvement in search latency—no matter how large—can permit exceeding an index size or RSS budget.

## Pareto Dominance

For candidates satisfying all hard constraints:
A candidate $A$ Pareto-dominates candidate $B$ ($A \succ B$) if and only if:
1. $A$ is no worse than $B$ across all eight metrics ($\forall i, m_i(A) \le m_i(B)$), AND
2. $A$ is strictly better than $B$ in at least one metric ($\exists j, m_j(A) < m_j(B)$).

Dominated candidates are filtered out, leaving the non-dominated **Pareto frontier**.

## Product Tuning Profiles

When one global choice is not defensible across all operating contexts, distinct profiles codify specific trade-offs:

1. **`balanced-standard`:** Balanced compromise between search latency and resource usage for general deployments.
2. **`interactive-low-latency`:** Prioritizes minimum p50 and p95 query latency ($w_{\text{warm}}=0.50, w_{\text{p95}}=0.25$), tolerating higher index footprint.
3. **`embedded-low-memory`:** Enforces strict memory caps (32 MB index, 64 MB RSS), prioritizing compact storage over raw speed.
4. **`batch-high-throughput`:** Optimized for large-scale multi-query processing and verification throughput.

## Selection Rationale & Audit Trail

Every invocation of `select_optimal_configuration()` produces an audit rationale report:
- Full enumeration of candidates evaluated.
- Explicit list of hard limit rejections with exact violation reasons.
- Filtered dominated candidates.
- Full Pareto frontier with calculated weighted losses.
- Clear statement justifying why the winner was selected over alternatives.

## Evidence Anchors

- [`include/pergrep/pareto.hpp`](../include/pergrep/pareto.hpp): Public interface (`WorkloadMetrics`, `HardConstraintBounds`, `ObjectiveWeights`, `ProductTuningProfile`, `ParetoSelectionResult`, `select_optimal_configuration()`).
- [`src/pareto.cpp`](../src/pareto.cpp): Hard limit validation, Pareto dominance checking, frontier extraction, weighted loss scoring, and audit report generation.
- [`tests/test.cpp`](../tests/test.cpp): `test_m94_pareto_selection()`.
