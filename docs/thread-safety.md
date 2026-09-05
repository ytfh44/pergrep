# Thread-safety contracts (M5.6)

**Status:** Accepted (audited + stress-proven)
**Scope:** M5.6 / GitHub issue #60

## C++ API

- **`Index`: immutable and freely shareable.** All factories (`build`,
  `from_documents`, `append`, `load`) return new values; no method mutates an
  existing `Index`. Sharing one `Index` across threads (by value — each copy
  shares the same immutable storage — or by `shared_ptr`) is race-free.
  Planner statistics are finalized at build time, so the first concurrent
  search needs no warm-up.
- **`Searcher`: one object may serve concurrent `find` calls.** All working
  state (candidate sets, recorders, scopes) is per-call; the object's members
  (`owned_`, `index_`) are immutable after construction. Proven by the M5.6
  stress block: 8 threads on one `Searcher` plus a 20-iteration soak, all
  byte-identical to serial. The M5.2 per-task-`Searcher` pattern is an
  additionally-safe construction, not a requirement.
- **`Searcher` construction and lifetime.** `Searcher(shared_ptr<const Index>)`
  shares ownership: the searcher (and its provider mappings) stays valid after
  every `Index` handle is destroyed. `Searcher(const Index&)` borrows: the
  caller must keep the `Index` alive past the searcher's last use (the M5.2
  parallel branch satisfies this by joining tasks before returning).
- **`Pattern`: immutable and freely shareable** (`shared_ptr<const Impl>`).
- **`SearchStats*`: per-call, never shared.** Concurrent writes to one stats
  object are a data race (unsupported, not tested — do not do this). Requesting
  stats also forces the serial path (M5.4).
- **`SearchOptions`: safe to share across threads** provided the referenced
  `eligible_file_ids` span and `should_cancel` callable are themselves safe
  for concurrent reads and outlive every call.
- **Results and views:** returned `Match` vectors are per-call owned. Views
  from `Index::content()` borrow provider storage and stay valid while any
  `Index`/`Searcher` sharing that storage is alive (M3.5); concurrent reads
  are race-free.
- **No thread-local assumptions for consumers.** The one internal
  `thread_local` (`suppress_guarded_fixed_dispatch`) is managed by an RAII
  scope guard that always restores the previous value, so worker threads never
  leak dispatch state across tasks.

## C API (`pergrep_c.h`)

- No handle is internally synchronized; safety comes from ownership, as above.
- `pg_index` / `pg_pattern`: freely shareable across threads.
- `pg_searcher_new` **shares** ownership of the index (it copies the
  `shared_ptr`), so freeing the `pg_index` first does not dangle live
  searchers. Still, use one `pg_searcher` per thread (`unique_ptr` inside, no
  internal lock); sharing one across threads is supported exactly as far as
  the C++ `Searcher` contract above, but per-thread searchers are the
  recommended pattern.
- `pg_search_stats`: per call, never shared. `pg_match` buffers are per-call
  owned; free with `pg_matches_free`. Error strings with `pg_error_free`.
- Misuse (unsupported, documented here rather than tested): sharing one stats
  object across threads; using any handle after its `*_free`; double-free.

## Providers and mappings

- Corpus providers are immutable read-only mappings (or resident bytes);
  concurrent reads never mutate. Lifetime follows the shared `Index` storage;
  source-file deletion/replacement after build cannot affect live views.

## Evidence anchors

- [`src/search.cpp`](../src/search.cpp): `Searcher` ctors (share vs borrow),
  RAII `FixedDispatchSuppression`, per-call `StatsRecorder`.
- [`src/c_api.cpp`](../src/c_api.cpp): handle structs, `pg_searcher_new`
  ownership sharing.
- [`src/index.cpp`](../src/index.cpp): build-time planner finalization.
- [`tests/test.cpp`](../tests/test.cpp): `pergrep_m56_thread_safety`.