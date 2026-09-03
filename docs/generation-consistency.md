# ADR-0051: Generation and snapshot consistency

**Status:** Accepted (contract; enforcing regression in M4.6, on-disk segmented publication follows in M4.7)

**Date:** 2026-09-03

**Scope:** M4.6 / GitHub issue #51

**Decision owner:** M4; M5 and all callers hold Index values/snapshots under these guarantees

## Context

An `Index` is a value type: a thin handle over a `std::shared_ptr<Impl>` that
holds the fully-materialized index state (filter structures, path-sorted
`FileInfo`s, `file_id -> content` mapping, planner statistics, and the segment
manifest chain). `Index::build`, `Index::from_documents`, `Index::append`, and
`Index::load` each construct a **new, complete `Impl`** before any handle to it
can be observed. Once built, an `Impl` is never mutated in place: the only way
to obtain a newer view is to produce a fresh `Impl` and bind an `Index` to it.

This immutability is the load-bearing property behind the earlier M4 contracts
— stable `file_id` identity (ADR-0046), append/segment publication (M4.2, ADR
re the merge), tombstones (M4.3), segment-local statistics (ADR-0049), and
compaction bounds (ADR-0050) — but it was implicit. This ADR makes it an
explicit, testable contract and defines what readers may and may not observe
across generation boundaries.

## Decision

### Immutable generations

An `Index` value (and every copy of it) **names exactly one generation**: the
`Impl` it transitively holds. A copy — `Index b = a;` — shares the *same* `Impl`
by reference count; it does not clone it. There is no in-place mutation of a
published `Impl`, so `content(i)`, `files()`, `Searcher::find`, and every other
read observe one internally-consistent generation: filters, content, paths, and
`file_id`s all derive from that single `Impl`. A reader can never observe old
filters with new content, or old paths with new `file_id`s.

### Reader lifetime

Because the state is owned by a `shared_ptr<Impl>`, a held `Index` (or a
`shared_ptr<const Index>` copy) keeps its generation alive for as long as the
handle survives — independently of what the caller does to the *original*
handle. Reassigning or destroying the original `Index` has no effect on the
`Impl` still referenced by a surviving copy. (The `Searcher(const Index&)`
constructor stores a pointer to the caller's `Index` value rather than taking
ownership; its lifetime contract is unchanged and is the caller's to satisfy.
The owning `Searcher(std::shared_ptr<const Index>)` overload keeps the
generation alive on its own.)

### Publication points

A generation becomes visible only when its `Impl` is complete:
`build`/`from_documents`/`append`/`load` return a fully-materialized `Index`.
There is no partial or tearing publication: a caller either holds the old
generation or the new one, never a half-built intermediate. `append` and
compaction (a full `from_documents` rebuild of the merged set) are front-loaded
and synchronous; the caller observes the pre- or post- view, never an
intermediate one (consistent with ADR-0050's "all-or-nothing" rebuild).

### Stale-read policy

A previously-held `Index` keeps serving its own — possibly stale — generation.
Staleness is a *caller opt-in* concern: the reader deliberately advances to a
newer generation via `fresh()`/rebuild (`build`/`from_documents`/`append`/
`load`) and must keep a new handle. Merely producing a newer generation
elsewhere never retroactively invalidates or mutates the bytes a held handle
already serves.

### Concurrent-update behavior

Producing a newer generation never mutates an older one. Two handles bound to
different generations operate on disjoint `Impl`s; a reader on the older
generation is unaffected by the newer one's construction, and vice versa. A
search against a held snapshot returns results whose `file_id`, `start`/`end`
offset, and content are all resolved within that snapshot's generation — no
cross-generation contamination (an offset is sliced from the same generation's
content that produced the `file_id`).

### Mapping / tombstone visibility

Any deletes, renames, and replaced content applied by an update (per
ADR-0046/0049, M4.2/M4.3) are **atomically part of the new generation**. A
tombstoned or replaced path is unreachable only through the new handle; the old
handle still serves the pre-update mapping. A rename is encoded as
tombstone(old) + changed{new}, and both halves land in the same generation, so
no reader ever sees the old path half-removed or the new path half-added.

## Consequences

- Correctness of append/tombstone/compaction reduces to the immutability
  invariant: each is a full `materialize_index_filters` rebuild published
  atomically; there is no shared mutable buffer to corrupt.
- Callers that must pin a view for the duration of a search or a report hold an
  `Index` (or `shared_ptr<const Index>`) copy; they are insulated from
  concurrent re-indexing.
- The always-rebuild path remains the correctness oracle until the on-disk
  segmented format (M4.7) lands; M4.7 must preserve the same
  one-`Impl`-per-publication guarantee for persisted segments.

## Evidence anchors

- [`include/pergrep/pergrep.hpp`](../include/pergrep/pergrep.hpp): `Index` (value handle over `shared_ptr<Impl>`), `Index::content`, `Index::files`, `Index::build`/`from_documents`/`append`/`load`, `Index::fresh`.
- [`src/index.cpp`](../src/index.cpp): `Index::content` (bounds-checked `loaded[file_id].view()`); `build`/`from_documents`/`append`/`load` materialize a fresh `Impl`.
- [`src/search.cpp`](../src/search.cpp): `Searcher::Searcher(const Index&)` (borrowed) vs. `Searcher(std::shared_ptr<const Index>)` (owning); `Searcher::find`.
- [`tests/test.cpp`](../tests/test.cpp): `pergrep_m46_generation` snapshot/generation/consistency regression.