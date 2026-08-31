# Query Optimizer Roadmap — from Indexed Grep to Text Query Compiler

`pergrep` today is an **indexed grep** based on **conservative filters + exact verification**. This roadmap evolves the already-present `mandatory literals` / `branch flavors` / `q-gram rarity` / `positional filtering` into a proper **Query Compiler**, each optimization with a well-defined IR, cost model, and correctness gate.

> Invariant: **zero false negatives**. Every optimization lives on its own branch with its own metrics and differential correctness checks. Optimizations alternate with BugFix tasks so that performance work never masks defects.

## Already Shipped (v5)

- `RegexProgram::query_ir` is the canonical optimizer-facing representation (`mandatory`, `branch_mandatory`, `prefixes`, `is_pure_literal`, and `exact_literal`), built once during parsing.
- Legacy `RegexProgram` metadata members remain synchronized compatibility views for existing internal callers; planner and verifier code reads `query_ir` only.
- M1.2 `QueryIR::filter` is a filter-only `Atom`/`And`/`Or`/`True` algebra. Atoms are exact, non-empty, case-sensitive literal-presence necessary conditions; factoring/CSE may only widen candidates and never touches ordered regex execution.
- `bench/workload_matrix.hpp` + `docs/workload-matrix.md`: versioned M0.1 workload/product-mode matrix (four closed classes, deterministic corpus/query profiles, lifecycle phases, and decision rule)
- `nfa_search` prefix jump + `search.cpp` multi-branch union pruning + pure-literal fast path
- Persistent index `v5` field-by-field serialization + `IndexOptions` upper/lower bound validation

### M1.4 — Physical operator capability inventory

[`docs/physical-operator-capabilities.md`](physical-operator-capabilities.md) is the ownership note and source-of-truth inventory for the pre-scheduler physical operators, their exact backends, option boundaries, fallback behavior, and `SearchStats` evidence. It is documentation only: current `Searcher::find` dispatch remains hard-coded, while `PlanKey`/cost-model results remain annotations until QO-4 adds an actual scheduler.

## Decomposition into Scoped Autoresearch Subtasks

### QO-1 — Literal & Branch Analyzer
- **Scope**: `src/regex.cpp`, `include/pergrep/pergrep.hpp`, `src/internal.hpp`
- **Deliverable**: Formal `QueryIR { mandatory, branch_mandatory, prefixes, is_pure_literal, word/line/multiline/case }` with unit tests; precise **Branch Flavor** semantics: longest literal per disjunct `Alt` branch vs global intersection
- **Correctness**: Conservativeness proof for `mandatory_literals()` + negative cases for `negative lookaround` isolation
- **Metrics**: `candidate_chunks` and `candidate_blocks` on the matrix alternation profiles, plus pure-literal dispatch hit rate
- **Branch**: `autoresearch/qo-1-literal-branch`

### QO-2 — Q-gram Rarity Planner
- **Scope**: `src/index.cpp` (`byte_freq`/`qgram_freq` stats), `src/search.cpp` (`planned_hashes`/`compile_qgram_query`)
- **Deliverable**: **Rarity Planner** that deterministically sorts distinct query 4-gram hash rows by frequency and selects the rarest `k` for both chunk and positional candidate filters. `IndexOptions::planned_qgrams` is a maximum probe budget; `0` means auto (all available rows), and positive values clamp only to the number available (there is no hidden 8-row cap).
- **Value contract**: for a query with `a` distinct hash rows, effective `k` is `a` when configured `0`, otherwise `min(configured, a)`. Thus configured values `0`, `1`, `2`, `8`, `16`, and `64` select `a`, `1`, `min(2,a)`, `min(8,a)`, `min(16,a)`, and `min(64,a)` rows respectively. Queries shorter than four bytes have `a=0` and use the documented conservative full-candidate fallback.
- **Correctness**: Any selected subset remains conservative (only widens candidate sets relative to the full intersection, never introduces false negatives); exact verification and operator dispatch remain unchanged.
- **Metrics**: C++ `SearchStats` exposes configured/effective/selected q-gram rows, chunk and positional probe bytes/operations, and a deterministic fallback reason; benchmark output uses these names rather than implying all query q-grams are probed.
- **Branch**: `autoresearch/qo-2-qgram-rarity`

### QO-3 — Positional Filter Compiler
- **Scope**: `src/index.cpp` (`PosDesc`/`pos` construction), `src/search.cpp` (`fixed_candidate_blocks`)
- **Deliverable**: Compile queries into **positional constraints**: `chunk → block` Bloom row selection, safe cross-chunk fallback (full-file scan when `literal_len > overlap`), block-level semantics for `word`/`line`/`multiline`/`null-data`/`crlf`
- **Correctness**: Conjunction of `q-gram rarity` and `positional` remains conservative; `>128 blocks` supported via dynamic bitmaps
- **Metrics**: `candidate_blocks`, positional hit rate
- **Branch**: `autoresearch/qo-3-positional`

### QO-4 — Execution Strategy & Cost Model
- **Scope**: `src/search.cpp` (`Searcher::find` dispatch), `src/regex.cpp` (`nfa_search`/`eval`)
- **Deliverable**: **Cost model + scheduler** choosing `Fixed (rare-byte anchor)` vs `NFA (Thompson)` vs `VM (extended)` vs `prefix-anchored` inverted index; per-record cost for `multiline`/`crlf`/`record_separator`; short-circuit on `max_matches`/`overlapping`
- **Correctness**: Strategy switch preserves `regex_find_all` semantics; `is_pure_literal` fast path only when `(multiline || !contains sep)`
- **Metrics**: Matrix-versioned per-scenario `search_time_ms`/`search_ms_per_query`/`throughput_mb_s`, plus candidate chunks/blocks and verified bytes; QO-4 should preserve the cold-vs-warm lifecycle split
- **Branch**: `autoresearch/qo-4-cost-model`

### QO-5 — Corpus & Freshness Optimizer (orthogonal to persistent index)
- **Scope**: `src/index.cpp` (`Index::save`/`load`/`fresh`), `src/cli.cpp` (cache path & rebuild policy)
- **Deliverable**: True **on-disk index** prototype: decouple filter structures from corpus (`I->loaded` need not be fully re-read), incremental `fresh()` via `mtime`/`size` and `mmap` path; first clarify current scope ("filter persistence, corpus re-read") in `bench` and docs, then evolve
- **Status (shipped)**: `IndexOptions::persist_corpus` (default `false` for backward compat) added as first step toward on-disk corpus. `false` = v5 filter-only persistence with `O(corpus)` re-read on `load` (current `std::ifstream` per file). `true` = v6 filter+corpus persistence (`loaded` sizes+data via `puts`/`gets` after `pos` vector) so `load` restores `content()`/`search` without filesystem. `fresh()` is `O(files)` using `error_code` overloads + `lexically_relative` (no `weakly_canonical` per file), with `skip_permission_denied`.
- **Metrics**: `fresh()` traversal time, `load` peak memory
- **Branch**: `autoresearch/qo-5-corpus`

### M1.3 — PlanKey (Execution Flags and Scope in Plan Key)
- **Scope**: `include/pergrep/pergrep.hpp` (`PlanKey`), `src/search.cpp` (`make_plan_key`, `estimateCost`/`chooseVerifier`/`estimate_all_candidate_plans` overloads, `Searcher::find` routing), `docs`
- **Deliverable**: Explicit `PlanKey` capturing all semantic inputs: `PatternOptions` fields, `SearchOptions` fields (`overlapping`, `invert_match`, `files_with/without_match`, `max_matches`, `record_separator`, `include_binary`, `eligible_file_ids` sorted deduped), `IndexOptions` capabilities (`chunk_bytes`, `chunk_overlap`, `positional_block_bytes`, `positional_budget_ratio`, `planned_qgrams`, `include_hidden`, `follow_symlinks`, `persist_corpus`), and `transformed_input_identity`. Deterministic FNV-64 `hash()` and `operator==`, `std::hash` specialization, and explicit selection/caching contract (distinct keys never reuse cached plan; no default newline/overlap/positive-match assumptions).
- **Correctness**: Plan selection routed through `PlanKey`-based cost model; distinct keys force recompute/fallback. Exact AST/NFA/VM execution unchanged.
- **Tests**: Deterministic distinctness for each semantic input (NUL vs LF, overlap, max_matches, invert/files, binary, eligible ids, index options, PatternOptions fields, transformed identity) and `std::hash` stability.
- **Branch**: `autoresearch/m1.3-plan-key` (M1.3)
### M2.2 — Conservative Byte/Rune Width and Context Analysis
- **Scope**: src/regex.cpp, src/internal.hpp, internal tests
- **Metadata**: RegexAnalysis records source-byte and decoded-rune lower/upper widths, lookahead/lookbehind context, nullability, record/line/absolute/word anchor requirements, effective scoped-node flags, and VM resource-limit notes. RegexBound::Finite, Unknown, and Unbounded are distinct; unknown/unbounded facts never become finite bounds.
- **Ownership**: computed after line/word parser wrappers are attached; advisory metadata only. Exact AST/NFA/VM matching, captures, ordering, overlap, and progress remain authoritative. Record separator (LF by default, or explicit NUL/custom byte) is a search input, not AST ownership.

| Node kind | Conservative rule |
|---|---|
| Empty/assertions/anchors | zero span; explicit boundary/anchor requirements |
| Literal | exact source bytes/runes when sensitive; folded byte width Unknown |
| Dot/Class | one rune; source bytes 1..4 |
| Concat/Alt | sum or min/max; overflow and unknown/unbounded states propagate |
| Group | child metadata |
| LookAhead/LookBehind | zero span; child upper width is forward/backward context |
| BackRef | Unknown lower and Unbounded upper (capture-dependent) |
| Repeat | AST min/max multiplication; existing VM caps remain notes, not execution changes |

M2.2 reports the existing 10,000-repeat, 8,192-byte lookbehind, and 50,000-state VM limits without changing them or claiming unsupported precision.
Finite AST repeat maxima remain finite semantic bounds even when above 10,000; `repeat_limit_applied` records that the unchanged VM execution cap is relevant and does not convert a finite fact into an unknown bound.

### M2.3 — Bounded-Region Execution
The verifier uses a guarded bounded-region operator for sensitive, non-nullable, finite-width regular patterns with proven mandatory literals. It narrows rune consumption to absolute source regions while preserving record splitting, source/file anchors, captures, and traversal order.

### M2.4 — Boundary-Preserving Bounded Verification
Boundary metadata is explicit: record bounds exclude separators (including CRLF terminators), multiline mode retains the complete file as one source, trailing separators do not create an implicit record, and custom/NUL separators remain distinct from payload NUL bytes. Record-local anchors (`^`/`$`, `\A`/`\z`) consult the established record/separator policy, and word assertions decode adjacent Unicode runes from source even when they lie outside the execution region. Region endpoints never become implicit file or record endpoints. Extended, unbounded, or otherwise ambiguous patterns retain the established full-record/full-file fallback.
### M2.6 — Interval-Aware Multi-Literal Candidate Joins
For a finite regular branch, each mandatory literal is compiled to a match-start interval using its relative minimum/maximum offset. Occurrence intervals are intersected across literals, then expanded by proven verifier halo and merged before bounded execution. The join is branch-local and operates on complete records, so literals in separate blocks or chunks remain candidates; if offsets are unknown or unbounded, the existing conservative full verifier path is retained.

### M2.7 — Explicit Unknown/Unbounded Region Fallback
A bounded-region plan is admitted only for finite, known byte/rune widths with no VM resource-limit approximation and no boundary-context requirement. Unbounded repeats (including .*), backreferences, lookaround, folded-width uncertainty, resource-capped repeats, and boundary-sensitive plans are rejected explicitly. Rejected plans clear their candidate set and invoke the exact per-file regex verifier over every indexed chunk; they never become an empty candidate rejection. SearchStats::verifier_fallback, physical_operator, and qgram_fallback_reason identify the fallback and reason (unbounded-repeat, backreference, lookaround, unknown-unicode-width, repeat-resource-limit, or missing-boundary-context).

## M1.8 — Objective-Aware First-Hit and Ordered Prefix

`SearchOptions::objective` separates exhaustive search from two bounded-result contracts: `FirstHit` returns the first result in the existing `(file_id, offset)` traversal, while `OrderedPrefix` permits stopping at `max_matches` only after that ordered prefix has been verified. The default remains `Exhaustive`; it never inherits hit-probability ordering or early-stop bias.

Quiet CLI searches opt into `FirstHit`. `files_with_matches` and `files_without_match` retain file polarity and ascending file order; a first-file objective probes files in that order rather than reordering candidates. If ordering cannot be proven, callers must use exhaustive traversal. The optional C++ `should_cancel` callback is cooperative and checked only at safe candidate/record boundaries; absent callbacks mean no cancellation is requested.

`SearchStats` reports `objective`, `candidate_order`, `candidate_order_preserved`, `early_stopped`, `early_stop_reason`, `first_hit_observed`, and `time_to_first_hit_ns`. The first-hit metric starts at search execution and ends at the first selected match (not candidate discovery); no-hit searches report `first_hit_observed=false` and `time_to_first_hit_ns=0`.

## Interleaving with BugFix

```
BF-0 (done): v5, IndexOptions bounds, word unification, unknown-escape rejection, k=v properties
  → QO-1
BF-1: remaining regex/CLI corners (glob case, \p{gc=...} spacing, same_device UNC)
  → QO-2
BF-2: extended VM resource bounds (lookbehind 8192 / Repeat 10k done, add state 50k & recursion error)
  → QO-3
BF-3: CLI multi-pattern invert/max-count and stats differential regression
  → QO-4
BF-4: crash-safe persistent index & cross-platform serialization (padding/endianness) hardening
  → QO-5
```

Every BF/QO runs on its own `autoresearch/*` branch gated by `autoresearch.sh` + `ctest 5/5`. The main agent judges **performance and mathematical correctness** (guarding zero false negatives) before merging to `master`.

## Evaluation Harness

- **Correctness**: every matrix scenario runs an indexed (32 KiB) vs. brute-force reference differential before timing; matches, file selection, byte offsets, overlap, max, CRLF, and NUL record behavior must agree
- **Performance**: matrix-versioned per-scenario `index_build_ms`, `search_time_ms`, `search_ms_per_query`, `throughput_mb_s`, `verified_kb`, `candidate_chunks`, and `candidate_blocks`; aggregate metrics remain for existing consumers
- **CI**: `.github/workflows/ci.yml` on `ubuntu`/`windows-msvc`/`windows-clang` presets with `ctest --output-on-failure` + `pergrep_bench` smoke; all corpora are deterministic in-memory fixtures and never downloaded

## Milestone

When QO-1..QO-4 land, `pergrep` will have a complete compilation pipeline: **Query IR → Rarity Planning → Positional Compilation → Cost-based Dispatch** — a small but orthodox text query compiler, not just a filtered grep.
