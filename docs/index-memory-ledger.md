# Index memory ledger (M8.1)

**Status:** Accepted (measurement surface + initial budgets)
**Scope:** M8.1 / GitHub issue #78

## What

`Index::memory_ledger()` reports deterministic per-component element bytes
(sizes, not capacities). `ledger().index_structures()` reconciles with
`index_bytes()` by construction. Corpus bytes are M3-owned provider memory,
reported for reference only — the index/corpus separation the bench relies
on is preserved.

## Measured breakdown (Windows, this machine)

| corpus | structs | corpus | groups raw+folded | positional | qstats+hpost | freq (fixed) |
|---|---|---|---|---|---|---|
| 23 B (2 docs) | 799 KB | 23 B | 8 KB | 0.1 KB | 1.5 KB | 788 KB |
| 53 KB (50 docs) | 988 KB | 53 KB | 66 KB | 26 KB | 102 KB | 788 KB |
| 1 MB (1 doc) | 2.40 MB | 1 MB | 1.05 MB | 0.5 MB | 34 KB | 788 KB |

Steady RSS (same process): 6.6 / 9.4 / 15.5 MB; build peak at 1 MB: 19 MB.
Residency gap (RSS vs structs+corpus) is process baseline (~5 MB) plus
vector capacities, map nodes (~48 B per exact q-gram entry, toolchain
specific), and allocator slack — all documented estimates, deliberately
not counted (nondeterministic across toolchains).

## Dominant components

- **Small corpora:** the fixed ~770 KB frequency tables
  (`byte_freq` 2 KB + `qgram_freq` 256 KB + `hash_chunk_freq` 512 KB).
- **Large corpora:** q-gram group bitmaps (~0.5x corpus each for raw and
  folded twin — ~1x combined at 1 MB) and positional matrices (~0.5x).
- **Transient:** the folded twin equals the raw groups and is never
  serialized — loaded snapshots drop it (verified in test).

## Initial per-component budgets (M8.2+ must hold or deliberately revise)

Ratios measured on ≥1 MB corpora (scale-free; small-corpus ratios are
dominated by the fixed tables and not budgeted):

- groups raw + folded ≤ 1.5x corpus (measured 1.0x)
- positional ≤ 1x corpus (measured 0.5x)
- qgram stats + hash postings ≤ 0.25x corpus (measured 0.03x)
- index structures overall ≤ 3x corpus (measured 2.3x)
- frequency tables ≤ 1 MB absolute (measured 770 KB fixed)

## Non-goals

Representation changes (M8.2+ compare against these budgets), RSS-capped
allocators, mapped-page accounting (snapshots are read fully today).

## Evidence anchors

- [`include/pergrep/pergrep.hpp`](../include/pergrep/pergrep.hpp):
  `IndexMemoryLedger`, `Index::memory_ledger`.
- [`src/internal.hpp`](../src/internal.hpp): single-source `ledger()`.
- [`tests/test.cpp`](../tests/test.cpp): `test_m81_memory_ledger`.