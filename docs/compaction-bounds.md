# ADR-0050: Bound compaction cost and segment read amplification

**Status:** Accepted (contract; enforcing regression in M4.5, on-disk segmented compaction follows in M4.7)
**Date:** 2026-09-03
**Scope:** M4.5 / GitHub issue #50
**Decision owner:** M4; M5 schedules compaction against these bounds

## Context

Appends (M4.2) and tombstones (M4.3) are materialized by rebuilding a single
merged in-memory index over base survivors + changed + added documents, minus
tombstones (M4.4 recomputes the global planner statistics the same way). There
are no on-disk segment files yet — that is deferred to M4.7 — so a "segment" is
one *logical* segment: an entry in the accumulated append-manifest chain, and
"compaction" is materializing the whole chain into a single fresh base, which is
observationally identical to a full `from_documents` rebuild of the merged set.

The cost of this model is *read amplification*: until the chain is compacted,
every append re-reads and re-indexes the full live corpus, and the accumulated
changed/replaced/tombstoned bytes track how much stale work the chain has
accumulated relative to the live corpus. Without a bound, an unbounded chain of
small appends grows the per-append reconstruct cost without an upper limit. This
ADR pins the contract for *when* the chain must be compacted and *how much*
transient space and read amplification is acceptable.

## Decision

### Compaction triggers

Compaction is due when **either** of the following holds for the in-memory
append chain:

- the accumulated segment count exceeds the declared fan-out limit
  `kMaxSegmentFanout = 64`; or
- the accumulated changed/replaced/tombstoned bytes exceed a fraction
  `kCompactionByteRatio = 0.25` of the live `corpus_bytes`:

```
should_compact(corpus, segments, appended)
  := segments > kMaxSegmentFanout
     || (corpus != 0 && appended > kCompactionByteRatio * corpus)
```

The predicate is a pure function of its inputs (no I/O, no mutable state) and is
thread-safe; it is exposed as `Index::should_compact(corpus_bytes, segments,
appended_bytes)`.

### Accounting

`appended_bytes` accumulates, across the whole un-compacted chain, the bytes of:

- each changed/replaced/added document (its post-append content size), and
- each tombstone-removed base document (its pre-removal size).

`segment_count` is the number of appended logical segments accumulated since the
last full materialization. Both are advanced by `Index::append` (one segment,
plus the delta of this append's changed + tombstoned bytes) and **reset to zero**
by any fresh full build — `Index::build`, `Index::from_documents`, and
`Index::load` — and by compaction.

`Index::compaction_stats()` reflects the chain as
`CompactionStats{ segment_count, appended_bytes, read_amplification }` where
`read_amplification == appended_bytes / corpus_bytes` (and `0.0` when the corpus
is empty). The fields are transient and are **never serialized**: `save`/`load`
write and read the existing structural and metadata fields only, so a loaded
index always reports a fresh, zero base.

### Scheduling

Compaction runs **single-threaded and synchronously at the next update**, never
on a background thread. There is no concurrent compaction against a live reader:
the rebuild is front-loaded into the update path, and the caller observes either
the pre-compaction snapshot or the post-compaction snapshot, never an
intermediate one.

### Temporary-space limit

A compaction rebuild holds at most **2x the live corpus** in transient buffers:
it reads the merged document set (≤ `corpus_bytes`) and writes the fresh base
(≤ `corpus_bytes`), then releases the old chain. It does not additionally
retain the full per-segment history during the rebuild.

### Tombstone reclamation

Tombstones are folded into the fresh base at compaction and removed from the
manifest chain: the compacted segment carries the surviving document set only,
so a tombstoned path's bytes stop contributing to `appended_bytes` and index
size after compaction.

### Crash behavior

Compaction is **all-or-nothing**. The fresh base is built fully, then atomically
swapped into place; no partial snapshot is ever published. A crash mid-build
leaves the previous intact chain untouched (the new base is discarded and the
old one remains the authoritative view).

### Read-amplification metric

```
read_amplification = sum(appended changed/replaced/tombstoned bytes) / live_corpus_bytes
```

with `segment_count` as the accompanying fan-out measure. Both are surfaced
through `CompactionStats` and are the inputs to `should_compact`.

## Consequences

- The compaction decision is deterministic, cheap, and side-effect-free; callers
  can gate `from_documents` rebuilds (compaction) on `should_compact` without
  touching I/O or the index.
- `append` keeps read amplification bounded: once either bound is crossed, the
  caller is expected to compact, resetting both counters to zero.
- Compaction preserves observable equivalence: a compacted index and the last
  appended index have byte-identical files/content and identical search results
  for every pattern, because both are full `materialize_index_filters` rebuilds
  of the same merged document set.
- No new merge algorithm or on-disk format is introduced here; both land in M4.7.
  Until then the always-rebuild path remains the correctness oracle, and these
  bounds are the *scheduling* contract over it.

## Evidence anchors

- [`include/pergrep/pergrep.hpp`](../include/pergrep/pergrep.hpp): `kMaxSegmentFanout`, `kCompactionByteRatio`,
  `CompactionStats`, `Index::compaction_stats`, `Index::should_compact`.
- [`src/internal.hpp`](../src/internal.hpp): `detail::IndexData::segment_count` / `appended_bytes` (transient fields).
- [`src/index.cpp`](../src/index.cpp): `Index::append` chain accounting, `Index::compaction_stats`,
  `Index::should_compact`.
- [`tests/test.cpp`](../tests/test.cpp): `pergrep_m45_compaction` chain/amplification/equivalence regression.