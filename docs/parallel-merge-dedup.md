# Parallel merge duplicate suppression (M5.3)

**Status:** Accepted (structural guarantee + regression proof)
**Scope:** M5.3 / GitHub issue #57

## Contract

The ordered per-file parallel merge (M5.2) introduces **no new duplicate
source**: partitioning is by file, and every within-file duplicate source is
handled by the unchanged serial per-file `find`.

- **Deduplication key:** `(file_id, start, end)`. Merge-time concatenation is
  by file index, never a re-sort, so keys cannot collide across partitions.
- **Per-file progress:** owned entirely by the serial per-file find — the same
  code that runs with `threads=1`. The queue never reorders or re-slices work
  inside a file.
- **Cross-chunk behavior:** chunk-overlap double-reports are suppressed inside
  the verifier (M2 progress semantics); legitimate overlapping matches
  (`overlapping=true`) are preserved. Parallelism does not change chunking.
- **Cross-segment behavior:** appended generations are materialized into one
  merged index (M4); per-file search sees the merged view, so segment
  boundaries cannot duplicate. Tombstoned paths never reach the verifier.
- **Zero-width progress:** empty matches advance by M2.5 rune/byte rules inside
  the per-file find; the merge carries them through untouched.
- **Rune versus byte advancement:** UTF-8 advancement is a per-file verifier
  concern; concatenation is offset-agnostic.
- **Captures:** each `Match` carries its own `captures` vector through the
  merge; capture order is the per-file find's order.

## Consequence

Parallel output equals serial output for overlapping and non-overlapping
searches — including chunk-boundary-crossing matches, multi-segment matches,
zero-width matches, and multi-byte-rune matches — by construction. The
`pergrep_m53_overlap_dedup` regression block pins this across thread counts.

## Evidence anchors

- [`src/search.cpp`](../src/search.cpp): M5.2 parallel branch (per-file
  partition, index-order concatenation).
- [`src/worker_queue.hpp`](../src/worker_queue.hpp): M5.1 in-order queue.
- [`tests/test.cpp`](../tests/test.cpp): `pergrep_m53_overlap_dedup`.