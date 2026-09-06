# Shared literal prefilters across regex (M7.3)

**Status:** Accepted (grouped fallback inside `find_multi`)
**Scope:** M7.3 / GitHub issue #73

## What

Regex entries of a `MultiQueryIR` that share a mandatory literal reuse one
chunk-candidate probe: each member searches the files containing shared
chunks (intersected with its own scope) instead of scanning everything.
Fixed literals travel the Aho-Corasick path or run independent; only filter-
layer atoms are factored — every pattern keeps its exact `RegexProgram`,
branch semantics, source order, and captures.

## Soundness

A mandatory literal (from the pattern's own `QueryIR`, intersected across
its branches) is necessary for any match: every true match lies in a chunk
containing it, hence in a file containing such chunks. Scoping a member's
search to those files (∩ its own scope) preserves exact results. This is the
M6.1 filter-necessity contract applied one level up; chunk candidates come
from the same `chunk_candidates` machinery as single-pattern search.

## Grouping and gate

- Candidates: regex entries (non-fixed kind) with mandatory literals;
  case-insensitive patterns yield no mandatory literals and stay independent
  automatically; fixed entries are skipped (AC/independent handle them).
- A literal shared by ≥2 ungrouped entries forms a group (first-shared wins,
  deterministic source order).
- The group's chunk set maps to files; if the file set covers everything
  (no pruning — including short/long literals the probe short-circuits on),
  the group dissolves and members run unscoped. Setup cost is never paid
  without benefit (no magic threshold; M7.7 may add one).
- Operator reporting: `SharedPrefilterGroups` iff at least one group pruned,
  else `IndependentFallback` (also when the whole IR took the AC path —
  that reports `AhoCorasickShared`).

## Composition (all preserved by construction)

Overlapping, binary policy, record separators, captures, max-matches,
objectives, threads, and per-entry scopes flow into each member's serial
`find` unchanged — only the eligible file set narrows, which M5.2 proved
order- and semantics-preserving. False-positive chunks (literal present,
pattern absent) verify to nothing; results stay exact.

## Non-goals

Cross-pattern result merging (M7.5), scope pushdown into planning (M7.6),
threshold tuning (M7.7), CLI wiring (independent until M7.5+).

## Evidence anchors

- [`src/search.cpp`](../src/search.cpp): grouping + scoped fallback in
  `find_multi`.
- [`tests/test.cpp`](../tests/test.cpp): `test_m73_shared_prefilter`.