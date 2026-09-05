# ASCII folded auxiliary filter (M6.2)

**Status:** Accepted (eligible queries filter; all others fall back)
**Scope:** M6.2 / GitHub issue #64

## What

A Bloom twin of the per-chunk group filters, built from ASCII-lowercased chunk
bytes (`A-Z` → `a-z`, all other bytes untouched), doubling group-Bloom memory
and adding one hashing pass at build time. It lets common ASCII
case-insensitive fixed-string queries prune chunks instead of scanning
everything (`sel = 1.0` fallback).

## Eligibility (all required)

- Fixed-string literal, `CaseMode::Insensitive`.
- Every literal byte is ASCII (`< 128`).
- No `word`/`line` scoping, no files-with/without-match modes.
- The index carries folded groups (built by `materialize_index_filters`;
  absent on loaded snapshots, which fall back).

Anything else — non-ASCII queries, scoped flags, missing auxiliary — takes
the pre-existing unfiltered path with identical results. The fallback reason
is observable in `SearchStats.qgram_fallback_reason`
(`case-insensitive-folded` vs `case-insensitive`).

## Equivalence (why rejection is safe)

For ASCII bytes, `ascii_fold_byte` equals ICU `u_foldCase`
(`U_FOLD_CASE_DEFAULT`): `A-Z` fold to `a-z`, all other ASCII bytes fold to
themselves — proven by exhaustive test over bytes 0–127, never assumed.
Therefore a folded-Bloom miss means no chunk window folds to the query window,
which means no ICU-folded match exists in the chunk. Rejection is exact;
acceptance is verified by the unchanged ICU-aware verifier, so byte offsets
are preserved. Non-ASCII chunk bytes pass through the fold untouched and can
only cause (safe) false positives, never false negatives.

## Budgets

- Index size: +100% of group-Bloom bytes (`folded_groups` mirrors `groups`
  shape exactly; asserted structurally).
- Build time: +1 q-gram hashing pass over chunk bytes (same complexity class
  as the raw pass; wall-clock deliberately unasserted).
- Query: one lowercase copy of the literal plus the standard Bloom probe.

`M8` owns representation-cost measurement; serialization is intentionally
untouched — the auxiliary is transient and never persisted.

## Non-goals

Full-Unicode folding, normalization (explicitly excluded per M6.1: precomposed
never matches decomposed), cost-model selectivity updates (estimates stay
conservative upper bounds).

## Evidence anchors

- [`src/internal.hpp`](../src/internal.hpp): `folded_groups`, fold helpers.
- [`src/index.cpp`](../src/index.cpp): twin build in `materialize_index_filters`.
- [`src/search.cpp`](../src/search.cpp): `folded_chunk_candidates`, hook.
- [`tests/test.cpp`](../tests/test.cpp): `pergrep_m62_folded_filter`.