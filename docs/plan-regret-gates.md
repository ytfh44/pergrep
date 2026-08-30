# M0.7: Plan-Regret and Performance Gates

**Version: 1.0** (Corresponds to Workload Matrix v2, M0.7 / Issue #18)

This document establishes the formal framework for **plan regret**, **shadow evaluation**, **performance regression gates**, and **rollback triggers** in `pergrep`.

---

## 1. Executive Summary & Core Principle

In query optimization, a cost model estimates candidate execution costs based on corpus statistics (such as q-gram and byte frequencies) and query selectivity. However, an unvalidated cost model can easily suffer from estimation drift, rank inversion, or degenerate into suboptimal fallback paths.

To prevent an estimated cost from becoming an unvalidated decision rule:
1. **Zero False Negatives Invariant**: Every execution strategy and pruning mechanism must remain conservative. Optimization must never alter match coordinates, match counts, or file selection.
2. **Deterministic Observability**: Candidate plan predictions and actual executed metrics are captured and compared deterministically.
3. **Plan Regret Accounting**: The engine quantifies the exact penalty incurred when the chosen plan deviates from the optimal candidate plan.
4. **Explicit Release Gates**: Performance improvements and regressions are systematically categorized by workload class, bounded by regression limits, fallback-rate caps, and automated rollback triggers.

---

## 2. Plan Candidates & Shadow Planner

### Candidate Execution Spectrum

For any given query and indexed corpus, `pergrep` considers a spectrum of execution strategies represented by `VerifierKind`:

| Candidate Plan | Verifier Strategy | Description | Typical Use Case |
| --- | --- | --- | --- |
| `FixedPositional` | Block-level positional Bloom | Evaluates positional q-gram Bloom filters across 256-byte blocks. | Short fixed literals ($\le 64$ bytes) without case folding or word/line anchors. |
| `FixedRareByte` | Chunk/file-level rare-byte anchor | Uses rarest byte anchor with `memchr` scanning. | Fixed literals longer than chunk overlap, case-insensitive literals, or word/line anchors. |
| `RegexChunk` | AST mandatory & branch pruning | Extracts mandatory/branch-mandatory literals from AST to prune 32 KiB chunks. | Regex queries with extractable literal prefixes or branch alternations. |
| `RegexBruteForce` | Full unpruned scan | Performs per-file Thompson NFA or PCRE2-compat scan across all chunks. | Conservative fallback when no pruning criteria can be extracted. |

### Shadow Evaluation Workflow

In shadow evaluation mode:
- The optimizer calculates the predicted cost $C_{\text{pred}}(P_i)$ for all valid candidate strategies $\{P_1, \dots, P_k\}$.
- The default scheduler selects the chosen plan $P_{\text{chosen}}$ without altering default dispatch or search semantics.
- During or following execution, actual metrics (observed latency, verified bytes, candidate chunks/blocks) are recorded.
- Counterfactual costs for alternative candidates are estimated or measured to evaluate if an alternate plan would have executed with lower actual cost.
### M1.6 lifecycle and observation status

M1.6 remains measurement-only: normal Searcher::find executes exactly the existing
chosen path and does not dispatch, replay, or mutate state for alternatives. A shadow
record is built after that search from the stable candidate enumeration and the chosen
operator's counters. PlanCandidateMetrics::ObservationStatus has three meanings:

- Observed: the operator actually executed and its measured cost/counters are valid.
- CounterfactualEstimate: a prediction for an eligible but unexecuted operator; it is
  useful for comparison but is excluded from regret and rank-inversion calculations.
- Unobserved: no measurement or counterfactual execution was recorded.

A chosen plan is always the observation anchor. A fallback is reported as a loss only when
the chosen plan and that fallback both have Observed measurements; an unmeasured
fallback cannot produce regret. Candidate records are sorted by verifier identity and
name, and workload groups are sorted by (workload_key, semantic_mode). The semantic
mode key is derived from the complete PlanKey, including fixed/regex, overlap, max,
invert, files, record separator, selector scope, index capabilities, and transformed-input
identity. Probe bytes/operations, verification bytes, and verifier CPU time are measured
when available; allocation and page-fault fields explicitly report unavailable rather than
pretending that an unavailable counter is zero.

### M1.3 PlanKey Construction
Plan selection and any future plan cache are keyed by an explicit `PlanKey` (`include/pergrep/pergrep.hpp`): `PatternOptions` (all 9 fields), `SearchOptions` (`overlapping`, `invert_match`, `files_with/without_match`, `max_matches`, `record_separator`, `include_binary`, `eligible_file_ids` sorted deduped), `IndexOptions` capabilities (`chunk_bytes`, `chunk_overlap`, `positional_block_bytes`, `positional_budget_ratio`, `planned_qgrams`, `include_hidden`, `follow_symlinks`, `persist_corpus`), and `transformed_input_identity`. `hash()` is deterministic FNV-64 with bitwise double handling; `operator==` compares all fields. `Searcher::find` routes through `make_plan_key` → `estimateCost(PlanKey, IndexData)`; distinct keys never reuse a cached plan (fallback to recompute). No path assumes default newline, non-overlap, or positive matching.
### M1.7 guarded fixed-dispatch policy

M1.7 changes the M1.6 measurement-only rule only for a narrow fixed-string
contract. The guard requires a 4--64 byte literal contained by chunk_overlap,
sensitive/default fixed matching, no word/line/multiline/dotall/crlf modifiers,
Unicode enabled, newline records, unlimited non-overlapping positive matching,
no invert/files wrapper, no binary inclusion, no eligible-file scope, and an
IndexOptions identity exactly equal to the live index. Planner statistics must be
ready, positional rows must exist, and the corpus must contain no binary files.
Short/empty/long or cross-chunk literals, custom/NUL separators, max-count,
overlap, selectors, unsupported statistics, and ambiguous option combinations
fall back to the established dispatch without cost-based selection.

The guarded selector compares calibrated M1.6 candidate-work estimates for
FixedPositional, chunk-level FixedRareByte, and whole-file FixedRareByte. Ties
prefer FixedPositional. Estimates may rank physical operators but never determine
candidate membership: every q-gram/Bloom candidate filter remains conservative,
and exact verification is unchanged. PlanKey fields are part of the guard so a
stale capability or semantic key cannot select a backend for a different contract.

SearchStats reports the concrete physical_operator and guarded_dispatch_used;
the stable verifier label remains FixedRareByte for both rare-byte shapes.
verifier_fallback is true for fixed searches outside the guard. Benchmark query
metrics include both fields, and the existing shadow observation/regret rules
continue to treat only the executed backend as Observed. Rollback is fail-closed:
disable guarded selection and execute the previous length/option branch; no result,
ordering, capture, offset, record, binary, or selector semantics are changed.

---


## 3. Mathematical Definition of Plan Regret

Let $C = \{P_1, P_2, \dots, P_k\}$ be the set of candidate execution plans for query $q$. Let $P_{\text{chosen}} \in C$ be the plan selected by the optimizer, and let $A(P)$ denote the actual execution cost (or execution latency) of plan $P$.

The **optimal plan** under observed execution is:
$$P_{\text{opt}} = \arg\min_{P \in C} A(P)$$

### 1. Absolute Plan Regret
$$\text{Regret}_{\text{abs}}(q) = \max\left(0, A(P_{\text{chosen}}) - A(P_{\text{opt}})\right)$$

### 2. Relative Plan Regret
$$\text{Regret}_{\text{rel}}(q) = \frac{A(P_{\text{chosen}}) - A(P_{\text{opt}})}{\max\left(A(P_{\text{opt}}), \epsilon\right)}$$
where $\epsilon = 10^{-9}$ prevents division by zero.

### 3. Prediction Error
$$\text{Error}_{\text{pred}}(q) = \frac{|C_{\text{pred}}(P_{\text{chosen}}) - A(P_{\text{chosen}})|}{\max\left(A(P_{\text{chosen}}), \epsilon\right)}$$

### 4. Rank Inversions
A rank inversion occurs for a pair of candidate plans $(P_i, P_j)$ when the optimizer predicts $P_i$ is cheaper than $P_j$, but actual execution proves $P_j$ is cheaper:
$$C_{\text{pred}}(P_i) < C_{\text{pred}}(P_j) \quad \text{and} \quad A(P_i) > A(P_j)$$

The query rank inversion count measures the monotonicity of the optimizer's cost surface.

### 5. Aggregate Shadow Metrics

Over a sequence or workload scenario of $N$ queries:
- **Fallback Rate**: $\frac{1}{N} \sum_{i=1}^N \mathbf{1}[P_{\text{chosen}}(q_i) \text{ is fallback}]$
- **Suboptimal Choice Rate**: $\frac{1}{N} \sum_{i=1}^N \mathbf{1}[\text{Regret}_{\text{abs}}(q_i) > 0]$
- **Mean Relative Regret**: $\frac{1}{N} \sum_{i=1}^N \text{Regret}_{\text{rel}}(q_i)$
- **P50 / P95 Relative Regret**: 50th and 95th percentiles of $\{\text{Regret}_{\text{rel}}(q_i)\}_{i=1}^N$
- **Total Excess Cost**: $\sum_{i=1}^N \text{Regret}_{\text{abs}}(q_i)$

---

## 4. Performance Gate Criteria & Thresholds

Performance gates enforce release readiness by comparing measured metrics against predefined thresholds and baseline releases.

### Standard Thresholds (`PerformanceGateThresholds`)

| Metric Dimension | Default Release Gate | Strict Release Gate | Enforcement Level |
| --- | --- | --- | --- |
| **Max $p50$ search latency** | $\le 100.0\text{ ms}$ | $\le 50.0\text{ ms}$ | Target threshold (always enforced) |
| **Max $p95$ search latency** | $\le 300.0\text{ ms}$ | $\le 150.0\text{ ms}$ | Target threshold (always enforced) |
| **Max search time per query** | $\le 50.0\text{ ms}$ | $\le 25.0\text{ ms}$ | Target threshold (always enforced) |
| **Min search throughput** | $\ge 1.0\text{ MB/s}$ | $\ge 5.0\text{ MB/s}$ | Target threshold (always enforced) |
| **Max $p50$ regression ratio** | $\le 1.05$ (+5%) | $\le 1.02$ (+2%) | Regression limit vs baseline |
| **Max $p95$ regression ratio** | $\le 1.10$ (+10%) | $\le 1.05$ (+5%) | Regression limit vs baseline |
| **Max total search time regression** | $\le 1.08$ (+8%) | $\le 1.03$ (+3%) | Regression limit vs baseline |
| **Max memory (RSS) regression** | $\le 1.15$ (+15%) | $\le 1.10$ (+10%) | Regression limit vs baseline |
| **Max workload fallback rate** | $\le 25\%$ | $\le 15\%$ | Optimization quality limit |
| **Max mean relative regret** | $\le 15\%$ | $\le 10\%$ | Cost model accuracy limit |
| **Max $p95$ relative regret** | $\le 30\%$ | $\le 20\%$ | Tail regret limit (FAIL if exceeded) |
| **Max suboptimal plan ratio** | $\le 20\%$ | $\le 10\%$ | Decision quality limit (FAIL if exceeded) |

---

## 5. Rollback Triggers

Any of the following conditions constitutes an immediate, non-negotiable **ROLLBACK** trigger:

1. **Correctness Parity Failure (Hard Invariant)**:
   - Any difference in match count, file IDs, byte start/end offsets, capture groups, overlapping semantics, or line terminators between indexed execution and reference oracle.
2. **Critical $p95$ Latency Regression**:
   - $p95$ search latency increases by $> 25\%$ ($\text{ratio} > 1.25$) against baseline.
3. **Critical $p50$ Latency Regression**:
   - $p50$ search latency increases by $> 15\%$ ($\text{ratio} > 1.15$) against baseline.
4. **Fallback Runaway**:
   - Workload fallback rate exceeds $50\%$ ($\text{rate} > 0.50$), indicating failure of pruning stages.
5. **Plan Regret Explosion**:
   - Mean relative plan regret exceeds $40\%$ ($\text{mean\_regret} > 0.40$), indicating severe cost model inversion.
6. **Memory Explosion**:
   - Peak RSS increases by $> 30\%$ ($\text{ratio} > 1.30$) without proportional corpus growth.

---

## 6. Workload Classification & Release Report

### Workload Classification

Every scenario in the workload matrix is evaluated and classified into one of three buckets:
- **`WIN`**: $\ge 5\%$ performance improvement ($p50 \text{ ratio} < 0.95$ and $p95 \text{ ratio} < 0.95$).
- **`NEUTRAL`**: Performance within expected variance ($\pm 5\%$ baseline range).
- **`REGRESSION`**: $> 5\%$ performance degradation ($p50 \text{ ratio} > 1.05$ or $p95 \text{ ratio} > 1.05$).

### Gate Status Levels

- **`PASS`**: All scenario and shadow thresholds satisfied; no regressions; zero rollback triggers (benchmark exits 0).
- **`WARN`**: Minor advisory threshold exceeded, but no hard failures (benchmark exits 0).
- **`FAIL`**: Regression limit, absolute threshold, suboptimal plan ratio, or p95 regret cap exceeded; release blocked (benchmark exits with nonzero code 2).
- **`ROLLBACK`**: Hard correctness parity failure or critical threshold violated; immediate revert required (benchmark exits with nonzero code 2).

---

## 7. Sample Release Gate Report

```markdown
# Performance & Plan Regret Release Gate Report

**Overall Status**: PASS
**Gate Passed**: YES
**Rollback Triggered**: NO

## Workload Overview
- **Total Scenarios**: 9
- **Wins**: 3
- **Neutral**: 6
- **Regressions**: 0

## Scenario Breakdown

| Scenario | Class | Status | Classif | p50 (ms) | p50 Base | Delta p50 | p95 (ms) | p95 Base | Delta p95 | Fallback % | Regret % |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `oneshot.cold.rare-short` | one-shot | **PASS** | WIN | 0.820 | 1.000 | -18.0% | 2.100 | 3.000 | -30.0% | 0.0% | 0.0% |
| `oneshot.filtered-scope.common-and-alternation` | one-shot | **PASS** | NEUTRAL | 1.950 | 2.000 | -2.5% | 4.800 | 5.000 | -4.0% | 0.0% | 2.1% |
| `warm-repeated.medium.rare-long-unicode` | warm-repeated | **PASS** | WIN | 0.650 | 0.800 | -18.8% | 2.050 | 2.500 | -18.0% | 0.0% | 0.0% |

## Shadow Planner & Plan Regret Summary
- **Total Queries Evaluated**: 54
- **Suboptimal Plan Selections**: 2
- **Fallback Invocations**: 0
- **Fallback Rate**: 0.00%
- **Mean Relative Regret**: 1.24%
- **P50 Relative Regret**: 0.00%
- **P95 Relative Regret**: 5.12%
- **Max Relative Regret**: 8.40%
- **P95 Prediction Error**: 4.30%
- **Total Excess Cost**: 45.2
```

---

## 8. Integration with Roadmap Milestones

- **M0.3 (Workload Matrix)**: Supplies versioned, deterministic scenario profiles.
- **M0.7 (Plan Regret & Gates)**: Provides the mathematical foundation and gating criteria to evaluate optimizer quality.
- **M1 (Literal & Branch Optimizations)**: Evaluated directly against the M0.7 release gate.
- **M9 (Corpus & Persistence Evolution)**: Evaluated to guarantee zero search regressions during storage decoupling.

## 9. Benchmark enforcement mode

`pergrep_bench` is report-only by default so CI smoke jobs can collect metrics
without treating machine-dependent baseline drift as a build failure. Pass
`--enforce-gate` to make the process status actionable: `0` means PASS/WARN,
`1` means FAIL, and `2` means ROLLBACK. Correctness failures always produce
ROLLBACK in the report and an enforcing invocation returns nonzero.
