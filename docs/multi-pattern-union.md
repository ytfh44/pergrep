# Multi-pattern union semantics (M7.5)

**Status:** Accepted (CLI `-e` runs one `find_multi`; union code untouched)
**Scope:** M7.5 / GitHub issue #75

## What

The union of shared-pattern results is defined to be exactly what
independent `-e` search produces today. The CLI now runs one `find_multi`
over all `-e` patterns (each entry carries the same `core_opt`) and feeds
the per-`source_id` vectors into the unchanged union below, so every rule
here holds by construction: shared paths change how matches are found
(M7.2–M7.4), never what is found.

## Union rules (all pre-existing, preserved)

- **Fan-in order.** Per-pattern vectors concatenate in source order into
  `perpat`; the union regroups by file (`byfile`), so output is per-file
  byte order, **not** source order. Pattern provenance is dropped at merge.
- **Duplicates.** The same expression twice searches twice; identical spans
  collapse in dedup — one output line, counted once.
- **Identical spans, different patterns.** `unique` on `(start, end)` keeps
  one survivor. The survivor is a deterministic function of the fan-in
  order; shared paths feed byte-identical fan-in, hence the identical
  survivor (captures included).
- **Overlapping.** Each entry's own `overlapping` flag governs its matches
  (the AC post-process and serial path both honor it per entry); the union
  never re-expands or trims overlaps.
- **Global max-count.** `--max-count` truncates selected lines per file
  downstream (rg parity); entries always search unbounded (`max_matches=0`).
- **Quiet exit.** Any hit in the union exits 0 (positive quiet uses
  `FirstHit` per entry; invert keeps exhaustive search for the line-level
  decision). No hit exits 1.
- **Files-with/without.** Derived per file from union presence (OR across
  patterns); `-l`/`--files-without-match` see a file once.
- **Invert.** `NOT (A OR B)`: entries search positive, the union merges,
  inversion applies once at line level.
- **Compile errors.** Patterns compile up front in source order; a bad `-e`
  errors exactly as the old per-pattern loop did (before any search output).
- **Internal stats.** The whole-IR `SearchStats` aggregates into the same
  accumulator; it has no downstream reader (`--stats` derives from selected
  lines), so shared work doing less is unobservable.

## Non-goals

A globally byte-sorted cross-pattern presentation (output stays per-file
merged lines, rg parity), per-pattern result streaming, changing the merge
into a library primitive (it drops pattern identity, so it stays
presentation-adjacent in the CLI; M5 owns the deterministic comparators it
reuses).

## Evidence anchors

- [`src/cli.cpp`](../src/cli.cpp): single-`find_multi` fan-in.
- [`tests/cli_compat.sh`](../tests/cli_compat.sh): `m75-*` union cases.
- [`tests/test.cpp`](../tests/test.cpp): `test_m75_union_fanin`.