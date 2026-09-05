# Guarded one-pass NFA fast path (M6.5)

**Status:** Accepted (eligible patterns accelerate; all others cost nothing extra)
**Scope:** M6.5 / GitHub issue #67

## What

The bounded-region executor (`RegexBoundedRegion`) is the guarded one-pass
fast path: mandatory literals anchor candidate regions, and the general
ordered verifier runs once per region instead of scanning whole files. The
general Thompson/Pike NFA remains the only execution engine; one-pass is a
scheduling restriction, not a second matcher — so spans, captures, and order
cannot diverge by construction.

## Eligibility (all required)

- Not extended (`Engine::Default` regex only); no lookahead/lookbehind/backref.
- At most one capture group (capture restriction).
- Two or more mandatory literals overall, or per-branch.
- `CaseMode::Sensitive` (folded/ICU paths stay on their own filters).
- Every width finite, match width in `(0, 1MiB]`, finite lookahead/lookbehind.
- No repeat-limit-applied or lookbehind-limit-applied analyses, no unbounded
  repeat, mandatory literals fit the width.

Anything else falls back to `RegexChunk`/`RegexBruteForce`/whole-file paths —
the exact code that ran before this gate existed. The gate itself is an O(AST)
analysis at plan time; ineligible patterns pay nothing beyond it.

## Restrictions honored

- **Captures:** single-group captures preserved with byte spans (pinned).
- **Greediness:** bounded greedy and lazy repeats keep oracle ends (pinned for
  both modes; anchored ends coincide, each mode verified against the oracle).
- **Prefix behavior:** mandatory-literal anchoring seeks across large
  non-matching prefixes (pinned with an 18KB filler).
- **State limits:** 1MiB width cap, 10000-iteration repeat cap, 50000-state VM
  cap, 10000-depth guard; exceeding any falls back or throws cleanly (BF-2).

## Measurement

- Structural win: `verified_bytes < corpus_bytes` on the eligible path
  (pinned per search via `SearchStats`).
- Operator visibility: `physical_operator` reports `RegexBoundedRegion` vs the
  baseline operators.
- Workload wins: the bench matrix carries `bounded-regex` queries
  (`ID_[0-9]{4,6}` family); the `Bench bounds check` gate keeps the default
  path from regressing.

## Rollback

M1 owns plan integration: deleting the eligibility call (or forcing it false)
restores the baseline path bit-identically. No index format or API change is
involved.

## Evidence anchors

- [`src/search.cpp`](../src/search.cpp): `bounded_regex_eligible`,
  `bounded_regex_regions`, dispatch.
- [`tests/test.cpp`](../tests/test.cpp): `test_m65_onepass` (static function,
  called from `main`).
- [`bench/workload_matrix.hpp`](../bench/workload_matrix.hpp): bounded-regex
  workload.