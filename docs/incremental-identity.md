# ADR-0046: Stable document identity and path mapping

**Status:** Accepted (contract; segment implementation follows in M4.2+)
**Date:** 2026-09-02
**Scope:** M4.1 / GitHub issue #46
**Decision owner:** M4; M5 consumes stable file IDs for ordered merging

## Context

Incremental segments reuse prior index bytes when only some files change. That is
safe only if a file's identity is stable across reordering, renaming, and
replacement: a stale or aliased `file_id` would otherwise produce duplicate results,
wrong output paths, or content attributed to the wrong document.

The current model already assigns `file_id` as the position of a file in the
path-sorted `infos` vector (`Index::build` sorts the traversal; `Index::from_documents`
sorts by `path`). This ADR makes that implicit arrangement an explicit, testable
contract and defines rename/replace/generation semantics on top of it.

## Decision

### Canonical document identity

- A document's identity is its **relative path** (UTF-8 generic string, forward
  slashes), unique within a single generation.
- `file_id` is the deterministic byte-wise-sorted position of that path. Two indexes
  built from the same file set with the same bytes/metadata produce the same
  `path -> file_id` mapping regardless of traversal or insertion order.

### Mapping rules

1. **Reorder** (files enumerated in a different order): no effect. `file_id` follows
   sorted path order, so the mapping is unchanged.
2. **Replace in place** (same path, different bytes or mtime): same identity, same
   `file_id`, new content/metadata. A new generation records the change.
3. **Rename** (path `A` -> path `B`): `A` is removed (tombstone) and `B` is a new
   document at its sorted position. A rename is recomputed, never represented as a
   stale alias of `A`'s old `file_id`.
4. **Delete**: the path is removed; its `file_id` is retired for the generation. No
   later document reuses a retired id within the same generation.
5. **Add**: a new path occupies its sorted position; existing ids are shifted only by
   a full re-materialization, never mutated in place.

### Generation ownership

- A generation is signed by `root` + path set + per-file `size`/`mtime` + `corp_bytes`
  (the existing `source_identity` fingerprints). Any of these changing marks a new
  generation.
- `file_id` values are only meaningful within one loaded generation. Cross-generation
  correspondence is by path, not by id.

### Compatibility

- The public search result still carries `file_id`; within a single loaded index the
  id maps 1:1 to a path via `Index::files()[file_id]`. No change to `Match`/`Searcher`.
- Consumers that need path-stable identity across generations read the path from
  `files()`, not the id.

## Consequences

- Incremental segments (M4.2+) can key content by path + generation without risk of
  stale or duplicate ids.
- Rebuild-from-scratch and incremental updates converge to identical `files()` and
  search results when the file set and bytes are equivalent.

## Evidence anchors

- [`include/pergrep/pergrep.hpp`](../include/pergrep/pergrep.hpp): `FileInfo`, `Index::files()`.
- [`src/index.cpp`](../src/index.cpp): `Index::build` path sort, `Index::from_documents` path sort, `source_identity`.
- [`tests/test.cpp`](../tests/test.cpp): `pergrep_m41_identity` determinism/rename regression.