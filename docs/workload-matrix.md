# M0.1 Workload and Product-Mode Matrix

**Matrix version: 1** (`kWorkloadMatrixVersion = 1` in `bench/workload_matrix.hpp`)

This is the stable workload contract for benchmark and release-gate work. Later milestones
may add query or corpus cases to a class, but may not silently redefine a class, phase, or
measurement. A matrix-version change requires updating this document and the benchmark
consumer together.

## Product classes

| Class | Product question | Corpus shape | Query shape | Required phase |
| --- | --- | --- | --- | --- |
| `one-shot` | Is index construction plus one search competitive with direct grep replacement? | Small repository, 8 deterministic files, 64 KiB target per file | Rare/common short literals, alternation, bounded regex | `cold` |
| `warm-repeated` | Does an already-built index amortize repeated searches? | Medium repository, 12 deterministic files, 128 KiB target per file | Rare long literal, Unicode/case-insensitive, bounded regex | `warm` with 8 measured iterations |
| `interactive-large-repository` | Does repository-scale narrowing keep interactive searches responsive? | Large deterministic repository, 32 files, 256 KiB target per file | Prefix, unbounded regex, Unicode/case-insensitive, multiline/CRLF | `repeated` with 3 measured iterations and a `**/*.cpp` selector |
| `batch-multi-pattern` | Does one warm index support a batch of heterogeneous patterns? | Medium repository | Six independent public-API patterns, including overlap/max | `repeated` with 2 measured iterations |

The corpus is generated in memory; no external repository, network download, or unbounded
fixture is used by CI. Seeds, file count, target size, file names, dictionary, and injected
sentinels are versioned in `bench/workload_matrix.hpp`.

## Phases and explicit scenarios

| Scenario | Class | Phase | Index build timed? | Scope/input variation |
| --- | --- | --- | --- | --- |
| `oneshot.cold.rare-short` | one-shot | cold | Yes, once per iteration | all files |
| `oneshot.filtered-scope.common-and-alternation` | one-shot | filtered-scope | Yes | deterministic `glob:**/*.cpp` selection |
| `oneshot.transformed.crlf` | one-shot | transformed-input | Yes | deterministic CRLF transformation |
| `oneshot.transformed.nul` | one-shot | transformed-input | Yes | deterministic NUL-record transformation |
| `warm-repeated.medium.rare-long-unicode` | warm-repeated | warm | No (build is reported separately) | same index, measured repeated searches |
| `interactive.large-repository.filtered` | interactive-large-repository | repeated | No | large corpus plus `glob:**/*.cpp` selection |
| `batch.multi-pattern.mixed` | batch-multi-pattern | repeated | No | same index, six independent patterns |

A cold scenario measures index construction and search separately. Warm/repeated scenarios
build and warm the index outside the measured interval, then report the repeated-search
interval. Correctness is always run before timing and is never folded into performance
numbers.

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

Every scenario emits a `SCENARIO` descriptor and a `METRIC` line with these dimensions:

- identity: matrix version, scenario, class, phase, corpus profile, transform, selector;
- workload: corpus bytes, query count, iteration count;
- lifecycle: `index_build_ms`, `search_time_ms`, and `search_ms_per_query`;
- performance: `throughput_mb_s` (bytes searched divided by measured search time);
- optimizer evidence: `logical_unique_kb` (deduplicated source ranges), `physically_touched_kb`
  (verifier slices, including overlap; legacy `verified_kb` alias), `index_probe_kb`,
  and `index_probe_operations`;
- candidate counts: `candidate_chunks`, `candidate_blocks`, and `candidate_files`;
- verifier/resource timing: `verifier_cpu_ms` (process CPU time after plan selection).
- result invariant: `matches` and `correctness=pass` from indexed-vs-reference comparison.

The matrix intentionally does not report allocation or page-fault timing: the current public
API has no portable per-search measurement for either event. Candidate counts are emitted
from the filter stage, while byte counters describe only verifier source slices and index rows;
none of these counters participates in matching or plan selection.

Release gates should retain the per-scenario lines rather than relying only on aggregate
metrics. Aggregate fields remain for compatibility with existing benchmark consumers.

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
