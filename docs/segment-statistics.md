# ADR-0049: Segment-local statistics and candidate unions

**Status:** Accepted (contract; enforcing regression in M4.4, on-disk segments follow in M4.7)
**Date:** 2026-09-03
**Scope:** M4.4 / GitHub issue #49
**Decision owner:** M4; M5 consumes per-segment statistics for ordered merging

## Context

Incremental segments (M4.2) and tombstones (M4.3) mutate the corpus a document at
a time, but candidate generation and rarity estimation are driven by the *global*
planner statistics (`byte_freq`, `qgram_freq`, hash-chunk frequency, `exact_qgrams`)
materialized over the whole index. If a deleted or replaced document's bytes and
q-grams lingered in those tables, they would bias `estimated_selectivity` and could
manufacture candidates for content that no longer exists. Conversely, if statistics
were recomputed too eagerly or inconsistently, a true match might be dropped (a
false negative). This ADR pins the contract that keeps the merged view's statistics
correct and conservative after every append and tombstone application.

## Decision

### Per-segment contribution

- Each appended segment contributes the byte-frequency, 4-gram hash-frequency, and
  chunk hash-frequency of *its own* documents. There is no separate per-segment
  table in the merged view yet; the segment's documents are folded into the merged
  resident index, and its statistics become part of the recomputed global tables.
- The merged view recomputes global statistics over the **union** of
  base survivors + changed + added documents, **minus** tombstones. This recompute
  flows through `materialize_index_filters` (which calls `rebuild_planner_stats`
  over the merged `loaded` document set), the same shared M3.6 construction contract
  used by `Index::build` and `Index::from_documents`.

### Conservative candidate generation

- Candidate generation is conservative: a candidate that intersects **any** live
  segment's survived content is retained. Because every surviving document's bytes
  are present in the recomputed tables, no true match is ever rejected (zero false
  negatives).
- Removed (tombstoned) documents are excluded from the recomputed statistics, so
  their byte/q-gram presence cannot inflate rarity estimates or emit candidates
  for content that is no longer reachable.

### Statistics recompute trigger

- Global statistics are recomputed **after every** append and tombstone application
  (always recompute). There is no incremental per-segment stat maintenance yet;
  that optimization lands together with the on-disk segmented format in M4.7, at
  which point the always-recompute path remains the correctness oracle.

### Selector-aware scopes and union/intersection

- Candidate scopes (e.g. `eligible_file_ids`) are defined over the merged
  file-id space: path-sorted and stable per M4.1 (ADR-0046).
- The candidate union is the candidate set over **all live segments**. No segment
  may admit a false negative: a candidate is the union of what each live segment's
  content actually contains; intersection/scoping only narrows the eligible set and
  never widens it past the recomputed global tables' truth.

## Consequences

- `Index::append` reuses `Index::from_documents`, so a merged index and a
  from-scratch `from_documents` over the same post-merge document set are
  byte-for-byte equivalent in filter structures and produce identical search
  results (file_id/start/end) for every pattern.
- Tombstoned and replaced content is unreachable: a literal unique to a removed
  document matches nothing, and a rare token's `candidate_files` equals exactly the
  number of surviving files that contain it.
- This is a contract and regression guard, not a new merge algorithm; the
  source-merging of segment statistics is deferred to M4.7.

## Evidence anchors

- [`include/pergrep/pergrep.hpp`](../include/pergrep/pergrep.hpp): `SegmentManifest`, `Index::append`, `SearchStats`.
- [`src/index.cpp`](../src/index.cpp): `rebuild_planner_stats`, `materialize_index_filters`, `Index::append`, `Index::from_documents`.
- [`src/search.cpp`](../src/search.cpp): conservative candidate generation and `candidate_files`/`estimated_selectivity` reporting.
- [`tests/test.cpp`](../tests/test.cpp): `pergrep_m44_segment_stats` tombstone-exclusion and equivalence regression.