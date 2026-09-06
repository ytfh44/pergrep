# Guarded Aho-Corasick shared scan (M7.2)

**Status:** Accepted (shared above threshold, independent below)
**Scope:** M7.2 / GitHub issue #72

## What

`Searcher::find_multi` runs one result vector per `MultiQueryIR` entry, in
source order. When the whole IR is eligible it performs a single
Aho-Corasick scan per file (`detail::AhoCorasick`, byte trie + failure links
+ merged outputs); otherwise it runs independent per-pattern searches with
full per-entry semantics (the fallback IS the reference behavior).

## Eligibility (all entries; any failure falls back)

- Fixed literals, `CaseMode::Sensitive`, no word/line scoping.
- `invert_match`, files-with/without-match off; `max_matches == 0`;
  `objective` exhaustive; `threads == 1`; empty eligible scope;
  `record_separator == '\n'`; non-empty literals.
- `overlapping` and `include_binary` may VARY per entry: overlap is filtered
  per pattern post-scan (leftmost non-overlapping, matching serial
  semantics); binary files are scanned once with per-entry match filtering.
- Set size within `[kMinSharedPatterns, kMaxSharedPatterns] = [4, 256]`,
  total literal bytes within `kMaxSharedBytes` (64KiB), automaton within
  `kMaxAcNodes` (16384) nodes or build refuses (falls back).

## Threshold

Measured crossover below 2 patterns on a 140KB mixed corpus (2x at n=2, 6x
at n=4, 12x at n=16, byte-identical totals); the gate requires 4+ for margin
against small-corpus variance. M9 may tune the constants; correctness never
depends on them (both paths are exactly equal — proven by test).

## Equivalence argument

- Partition is by file (ascending), per-pattern streams ascending by construction
  (fixed-length: end order == start order); concatenation preserves serial order.
- Non-overlap post-filter keeps an occurrence iff `start >= last_end` — the
  serial leftmost rule.
- Duplicates (same literal, distinct source IDs) each receive every occurrence.
- Verification stays exact per byte; the automaton only locates candidates.
  Captures are empty (fixed hot path, M6.1); binary/record behavior matches
  serial per entry.
- Observability: `SearchStats.physical_operator` reports `AhoCorasickShared`
  vs `IndependentFallback`; `matches` aggregates.

## Unicode eligibility (M6 owns the rules)

Byte-wise trie handles arbitrary bytes uniformly (UTF-8 subsequences match
exactly). Case-insensitive patterns are excluded (ICU folding is not
byte-uniform); word/line modes excluded (boundary semantics stay serial).
Covered by test with multibyte content.

## Non-goals

Regex sharing (M7.3), captures/replacement/only-matching modes (M7.4),
dedup/order/limits/invert merging (M7.5), scope pushdown (M7.6). CLI wiring
stays independent until M7.5+.

## Evidence anchors

- [`src/aho_corasick.hpp`](../src/aho_corasick.hpp): automaton.
- [`src/search.cpp`](../src/search.cpp): gate + `find_multi`.
- [`tests/test.cpp`](../tests/test.cpp): `test_m72_aho_corasick`.