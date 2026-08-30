# M0.3/M1.5/M1.6 Workload and Lifecycle Matrix

**Matrix version: 4** (kWorkloadMatrixVersion = 4 in bench/workload_matrix.hpp)

This is the stable workload contract for benchmark and release-gate work. Later milestones
may add query or corpus cases to a class, but may not silently redefine a class, phase, or
measurement. A matrix-version change requires updating this document and the benchmark
consumer together.

## Product classes

| Class | Product question | Corpus shape | Query shape | Required phase |
| --- | --- | --- | --- | --- |
| `one-shot` | Is index construction plus one search competitive with direct grep replacement? | Small repository, 8 deterministic files, 64 KiB target per file | Rare/common short literals, alternation, bounded regex | `cold` |
| `warm-repeated` | Does an already-built index amortize repeated searches? | Medium repository, 12 deterministic files, 128 KiB target per file | Rare long literal, Unicode/case-insensitive, bounded regex | `warm` with measured iterations |
| `interactive-large-repository` | Does repository-scale narrowing keep interactive searches responsive? | Large deterministic repository, 32 files, 256 KiB target per file | Prefix, unbounded regex, Unicode/case-insensitive, multiline/CRLF | `repeated` with 3 measured iterations and a `**/*.cpp` selector |
| `batch-multi-pattern` | Does one warm index support a batch of heterogeneous patterns? | Medium repository | Six independent public-API patterns, including overlap/max | `repeated` with 2 measured iterations |

The corpus is generated in memory; no external repository, network download, or unbounded
fixture is used by CI. Seeds, file count, target size, file names, dictionary, and injected
sentinels are versioned in `bench/workload_matrix.hpp`.

## Storage backends and explicit scenarios

| Scenario | Class | Phase | Storage | Index build timed? | Scope/input variation |
| --- | --- | --- | --- | --- | --- |
| `oneshot.cold.rare-short` | one-shot | cold | in-memory | Yes, once per iteration | all files |
| `oneshot.filtered-scope.common-and-alternation` | one-shot | filtered-scope | in-memory | Yes | deterministic `glob:**/*.cpp` selection |
| `oneshot.transformed.crlf` | one-shot | transformed-input | in-memory | Yes | deterministic CRLF transformation |
| `oneshot.transformed.nul` | one-shot | transformed-input | in-memory | Yes | deterministic NUL-record transformation |
| `warm-repeated.medium.rare-long-unicode` | warm-repeated | warm | in-memory | No (build reported separately) | same index, measured repeated searches |
| `interactive.large-repository.filtered` | interactive-large-repository | repeated | in-memory | No | large corpus plus `glob:**/*.cpp` selection |
| `batch.multi-pattern.mixed` | batch-multi-pattern | repeated | in-memory | No | same index, six independent patterns |
| `filesystem.cold.roundtrip` | one-shot | cold | filesystem | Yes | `Index::build`, `save`, `load`, `fresh` lifecycle |
| `filesystem.warm-repeated.medium` | warm-repeated | warm | filesystem | No | filesystem index with repeated searches |

A cold scenario measures index construction and search separately. Warm/repeated scenarios
build and warm the index outside the measured interval, then report the repeated-search
interval. Filesystem-backed scenarios test the complete persistence lifecycle (`Index::build`,
`Index::save`, `Index::load`, `Index::fresh`) on disk. Correctness is always run before timing
and is never folded into performance numbers.

## Query families

The query profiles intentionally cover the cases currently representable by the C++ API:

- `rare-short-fixed`: fixed `RARE_TOKEN_X9`.
- `common-short-regex`: common `error` literal-equivalent regex.
- `rare-long-fixed`: fixed long injected literal.
- `common-alternation`: `error|warning|critical|panic`.
- `bounded-regex`: bounded repetition, `ID_[0-9]{4,6}`.
- `unbounded-regex`: unbounded `.*timeout.*`.
- `unicode-case-insensitive`: Greek text with insensitive matching.
- `multiline-crlf-line`: `^timeout=.*$` with multiline and CRLF options.
- `nul-record`: NUL record separator (`SearchOptions::record_separator = '\0'`) with `include_binary = true`, so the embedded NUL fixture is actually searched.
- `overlap-with-max`: overlapping fixed `aba` with `max_matches = 4`.
- `interactive-prefix`: anchored-prefix style `connection_[a-z_]+`.

Batch multi-pattern behavior is represented as a vector of independent `Pattern` values
because the library's public `Searcher::find` API accepts one compiled pattern per call.
The benchmark must not invent a library-level OR or alter CLI multi-pattern semantics; a
future batch implementation can consume these profiles without changing this contract.

## Measurement dimensions

Every scenario emits a `SCENARIO` descriptor, a `METRIC scenario=...` line, and per-query
`METRIC query=...` lines with these dimensions:

- **Identity**: matrix version, scenario name, class, phase, storage backend (`in-memory` vs `filesystem`), corpus profile, transform, selector;
- **Workload**: `corpus_bytes`, `index_bytes`, query count, iteration count;
- **Lifecycle timings**: `index_build_ms`, `index_save_ms` (filesystem), `index_load_ms` (filesystem), `freshness_check_ms` (filesystem), `cold_search_ms` (unwarmed first query pass), `warm_search_ms` (warmed query pass), `repeated_search_ms`, `search_time_ms`, and `search_ms_per_query`;
- **Latency distribution**: `search_p50_ms` (median) and `search_p95_ms` (95th percentile) calculated across all search executions for each query and scenario;
- **Throughput**: `throughput_mb_s` (bytes searched divided by measured search time);
- **Process & memory resources**: `rss_kb` (current resident set size), `peak_rss_kb` (peak resident set size), and `page_faults` (process page faults where supported by OS counters);
- **Optimizer evidence**: `logical_unique_kb` (deduplicated source ranges), `physically_touched_kb`
  (verifier slices, including overlap; legacy `verified_kb` alias), `index_probe_kb`,
  and `index_probe_operations`;
- **Candidate counts**: `candidate_chunks`, `candidate_blocks`, and `candidate_files`;
  these are observed filter outputs.
- **M1.5 planner comparison**: `predicted_candidate_chunks`,
  `predicted_candidate_blocks`, and `predicted_verified_kb` are deterministic
  estimates from exact q-gram chunk/document statistics, widened by legacy
  hash-bucket collisions. `prediction_error_bound_chunks`,
  `prediction_error_bound_blocks`, and `prediction_error_bound_kb` report the
  conservative remaining-work bounds used by the planner. Compare predicted
  and observed fields per query; overlap and Bloom false positives can make
  observed work lower, never invalidate an exact match.
- **Verifier/resource timing**: `verifier_cpu_ms` (process CPU time after plan selection);
- **Result invariant**: `matches` and `correctness=pass` from indexed-vs-reference comparison.
- **M1.6 shadow planner**: each query also emits the chosen operator, canonical
  plan_key_hash/semantic_mode, predicted work for every enumerated candidate, and
  observed work only for operators that actually executed. SHADOW_CANDIDATE records
  carry predicted_cost, actual_cost, verification bytes, index probes, and an explicit
  observation status; unexecuted candidates are never counted as regret.
- **Optional resources**: allocation counts/bytes and per-search page faults are emitted
  as available or unavailable; unavailable counters are not interpreted as zero.
- **Regret grouping**: aggregate shadow reports group by workload key and semantic mode
  (which includes all PlanKey semantics: fixed/regex, overlap, max, invert, files, NUL/CRLF,
  and selector scope), with stable candidate and group ordering.

Release gates should retain the per-scenario lines rather than relying only on aggregate
metrics. Aggregate fields remain for compatibility with existing benchmark consumers.
See `docs/plan-regret-gates.md` for full specification of plan regret metrics, shadow plan
evaluation, target workload thresholds, regression limits, fallback-rate caps, and rollback triggers.
## Execution versus storage decision rule

1. **Choose execution work first** when the warm/repeated or interactive scenarios show
   high `search_ms_per_query`, low `throughput_mb_s`, or excessive `verified_kb` while
   index build time and index size are acceptable. Inspect candidate chunks/blocks and
   query-family breakdowns before changing storage.
2. **Choose storage work first** when cold scenarios are dominated by
   `index_build_ms`, or when repeated search is fast but rebuilding/loading the corpus
   dominates the product path. Compare the cold build cost with the warm search interval;
   do not infer a storage win from query latency alone.
3. **Choose scope/input work** when filtered-scope or transformed-input scenarios regress
   relative to their native counterparts. Fix selector or transformation accounting
   before attributing the regression to the query executor.
4. **Require correctness parity** for every change: an indexed result must match the
   reference result (including file selection, offsets, overlap, max, CRLF, and NUL
   records). A faster result with a mismatch is a failed gate, not an optimization.

This rule keeps execution optimizations (M0.3/QO-1..QO-4) distinct from storage and
freshness work (M9/QO-5), while allowing later milestones to consume the same deterministic
profiles and release-gate output.
