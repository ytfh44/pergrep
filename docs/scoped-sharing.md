# Scope-aware shared planning (M7.6)

**Status:** Accepted
**Scope:** M7.6 / GitHub issue #76

## What

Shared scans must not process files the caller excluded. Selector scope
reaches multi-pattern planning only as eligible file IDs per entry
(selectors — ignore/glob/type/hidden/depth/filesize/binary — are compiled
to that set by the CLI before search; M7 owns ID plumbing, CLI maintainers
own selector semantics). M7.6 makes both shared paths scope-aware so shared
and independent execution inspect exactly the same files.

## Rules

- **Aho-Corasick.** Scoped entries are eligible (M7.2's unscoped-only gate
  is lifted). The scan covers the union of member scopes — an unscoped
  member means all files — so excluded files are never read; the
  post-process keeps per entry only matches in its own scope (empty = all).
  Union scanning with per-entry filtering is sound: every member's true
  matches lie in its scope, which the union covers. Overlapping, binary
  policy, and per-file non-overlap reset apply as before, inside the filter.
- **Grouped prefilter.** Each group's shared probe filters candidate chunks
  to the union of group members' scopes before the file set is computed
  (again, an unscoped member means all files). Excluded files therefore
  cannot affect candidacy, the pruned file set, downstream verification, or
  counts. Posting-list traversal itself stays global — necessarily so, since
  the q-gram index is corpus-wide; this matches single-pattern search,
  whose probe is likewise global while verification and accounting honor
  scope. "Processing" a file means verifying its bytes or counting it as a
  candidate, never traversing shared index structures.
- **Empty groups stay empty correctly.** If no in-scope chunk carries the
  shared literal, the group scope is empty and members find nothing — sound
  by literal necessity (an in-scope match would require such a chunk).
- **Scope-aware statistics.** Counters on shared paths reflect only
  in-scope work by construction: the AC scan never visits excluded files,
  grouped file sets exclude them before verification, and member finds keep
  their narrowed scopes in accounting. Excluded files contribute zero to
  candidates, matches, counts, and plan selection — they cannot make a
  shared plan look better or worse than the independent equivalent.

## Non-goals

Pushing selector *predicates* (globs, types) into planning — the eligible
set is their compiled form and the only thing search may depend on;
changing single-pattern scope handling (parity target, untouched).

## Evidence anchors

- [`src/search.cpp`](../src/search.cpp): union scan + per-entry filter
  (AC), group-union probe filter.
- [`tests/test.cpp`](../tests/test.cpp): `test_m76_scoped_sharing`.