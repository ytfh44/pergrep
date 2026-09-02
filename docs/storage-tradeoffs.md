# Storage trade-offs: source-backed (mmap), resident, and packed corpus

**Status:** Measured report (release-gate input, not a default switch)
**Date:** 2026-09-02
**Scope:** M3.7 / GitHub issue #43
**Owner:** M3; M9 consumes this report for release decisions

## Purpose

Decide whether the corpus provider improves the complete lifecycle rather than a single
load-time number. This report compares the three storage providers on the closed M0
workload matrix and records where each is slower, more memory-hungry, or unsuitable. No
default switch occurs without a declared target win: `IndexOptions::persist_corpus` remains
`false` and the source-backed (mapped) view remains the default.

## Providers measured

| Provider | Trigger | Load behavior |
|---|---|---|
| **resident** (in-memory) | `Index::from_documents` | Corpus bytes owned in-process; no filesystem on load. |
| **source-backed / mmap** | `Index::build` with `persist_corpus=false` (default) | v7 save writes filters only; load re-attaches files through a read-only anonymous/private mapping, falling back to resident on failure. `fresh()` revalidates source identity. |
| **packed** (immutable snapshot) | `Index::build` with `persist_corpus=true` | v7 save embeds corpus bytes; load restores them from the index without touching the source tree. `is_snapshot()` is `true`; `fresh()` is not required before search. |

## Measurement method

- `pergrep_bench` runs the closed matrix in `bench/workload_matrix.hpp`; this change adds
  `StorageBackend::Packed` and two `packed.*` scenarios mirroring `filesystem.*`.
- Metrics sampled per scenario: `index_build_ms`, `index_save_ms`, `index_load_ms`,
  `freshness_check_ms`, cold/warm/repeated search, `rss_kb`, `peak_rss_kb`, `page_faults`,
  `index_bytes`, `logical_unique_bytes`, `physically_touched_bytes`, and `correctness`.
- `packed.*` removes the source tree after `save` and before `load` to prove snapshot
  independence; the load then materializes the corpus payload from the index.
- Environment: Windows 11 x64 (AMD Ryzen 9 8945HX), clang-cl release preset.

## Results (filesystem/mapped vs packed, same corpus and query sets)

| Scenario | provider | build_ms | load_ms | rss_kb | page_faults | cold_ms | correctness |
|---|---|---|---|---|---|---|---|
| cold.roundtrip (small 524 KiB) | filesystem/mapped | 112.4 | 112.2 | 9420 | 134 475 | 30.6 | pass |
| cold.roundtrip (small 524 KiB) | packed | 140.1 | 151.2 | 9676 | 163 763 | 35.0 | pass |
| warm-repeated.medium (1.5 MiB) | filesystem/mapped | 321.1 | 342.2 | 10 224 | 156 123 | 39.4 | pass |
| warm-repeated.medium (1.5 MiB) | packed | 333.2 | 347.2 | 10 820 | 187 097 | 54.7 | pass |

Both providers report `correctness=pass` against the shared `Index::from_documents`
reference. Filter structures and match coordinates are equivalent; only the storage
provider differs (see the M3.6 cross-mode equivalence regression).

## Findings

- **Packed load is slower than source-backed load.** `packed` pays a full corpus-payload
  read plus checksum on `Index::load` (cold load +35%, warm load +1.5%). Source-backed load
  re-attaches a read-only mapping per file and revalidates identity instead of re-reading
  the corpus payload.
- **Packed is more memory-hungry in the transient load path.** `page_faults` rise ~22%
  (cold) and ~20% (warm) because the packed payload is copied into a private backing before
  exposure. Steady-state `rss_kb` is close (9676 vs 9420; 10820 vs 10224) because all modes
  keep a resident `loaded` view after materialization — packed is not a steady-state RSS
  win.
- **Packed build is slightly more expensive** (save serializes and checksums corpus bytes).
- **Freshness diverges.** `freshness_check_ms` is negligible for packed (no source
  re-traversal) because a snapshot is valid from its own checksum; source-backed pays a
  full tree + per-file mtime/size scan.
- **Disk footprint diverges.** Packed index files are larger by approximately the raw
  corpus bytes (the persisted payload); source-backed index files contain only filters and
  metadata.

## When each provider is suitable

| Provider | Suitable | Unsuitable |
|---|---|---|
| source-backed / mmap | Mutable local workspaces; `one-shot`, `warm-repeated`, `interactive-large-repository` where freshness matters; avoids raw-corpus duplication. | Source unavailable/unstable across search; network/removable media with metadata latency; transformed input. |
| resident | Transformed input (`--pre`, encoding conversion, archive decode, stdin); the ephemeral path never enters the source cache. | Persistent reuse where re-building is the dominant cost. |
| packed | Offline/reproducible batch, audit artifact, source that moves/disappears, cross-process/machine hand-off, high-latency media. | Mutable `interactive-large-repository` (stale-by-design); any workload where load-time corpus-payload read dominates. |

## Decision consequence

The source-backed mapped provider remains the default. `persist_corpus=true` stays an
explicit opt-in. No default switch occurs without a declared target win, per the acceptance
criterion; this report supplies the measured data for M9 release gating.