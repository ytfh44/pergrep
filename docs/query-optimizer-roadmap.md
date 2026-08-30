# Query Optimizer Roadmap — from Indexed Grep to Text Query Compiler

`pergrep` today is an **indexed grep** based on **conservative filters + exact verification**. This roadmap evolves the already-present `mandatory literals` / `branch flavors` / `q-gram rarity` / `positional filtering` into a proper **Query Compiler**, each optimization with a well-defined IR, cost model, and correctness gate.

> Invariant: **zero false negatives**. Every optimization lives on its own branch with its own metrics and differential correctness checks. Optimizations alternate with BugFix tasks so that performance work never masks defects.

## Already Shipped (v5)

- `RegexProgram { mandatory, prefixes, branch_mandatory, is_pure_literal }` extracted in `src/regex.cpp`
- `nfa_search` prefix jump + `search.cpp` multi-branch union pruning + pure-literal fast path
- `bench/workload_matrix.hpp` + `docs/workload-matrix.md`: versioned M0.1 workload/product-mode matrix (four closed classes, deterministic corpus/query profiles, lifecycle phases, and decision rule)
- Persistent index `v5` field-by-field serialization + `IndexOptions` upper/lower bound validation

## Decomposition into Scoped Autoresearch Subtasks

### QO-1 — Literal & Branch Analyzer
- **Scope**: `src/regex.cpp`, `include/pergrep/pergrep.hpp`, `src/internal.hpp`
- **Deliverable**: Formal `QueryIR { mandatory, branch_mandatory, prefixes, is_pure_literal, word/line/multiline/case }` with unit tests; precise **Branch Flavor** semantics: longest literal per disjunct `Alt` branch vs global intersection
- **Correctness**: Conservativeness proof for `mandatory_literals()` + negative cases for `negative lookaround` isolation
- **Metrics**: `candidate_chunks` and `candidate_blocks` on the matrix alternation profiles, plus pure-literal dispatch hit rate
- **Branch**: `autoresearch/qo-1-literal-branch`

### QO-2 — Q-gram Rarity Planner
- **Scope**: `src/index.cpp` (`byte_freq`/`qgram_freq` stats), `src/search.cpp` (`planned_hashes`/`compile_qgram_query`)
- **Deliverable**: **Rarity Planner** that sorts all query 4-grams by `qgram_freq`, selects the rarest `k` (adaptive `planned_qgrams`, budgeted by `chunk_bytes`/`positional_block_bytes` and `positional_budget_ratio`)
- **Correctness**: Any subset remains conservative (only reduces candidates, never introduces false negatives); `k=0` degenerates to full scan
- **Metrics**: `verified_kb`, `candidate_chunks` Pareto vs `k`
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
