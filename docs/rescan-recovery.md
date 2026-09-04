# ADR-0053: Recover from Watcher Loss with Periodic Rescans

**Status**: Accepted (contract + convergence regression)  
**Date**: 2026-09-04  
**Scope**: M4.8 / GitHub issue #53  
**Decision owner**: M4; M5+ callers observe `fresh()` and convergence behavior

---

## Context

M4.9 will introduce a watcher daemon that incrementally updates the index as files
change. Watcher pipelines can lose events under load (queue overflow, process
crash, cross-volume moves, network fs stalls). A client that only applies
incremental events would diverge from the true filesystem state.

This ADR defines the **reconciliation contract** that guarantees convergence:
any sequence of dropped/reordered/duplicated/coalesced events must eventually
converge to exactly the full-rebuild result (no duplicate or missing matches;
deleted content never matches; renamed content appears exactly once under the
new path — mirroring M4.1/M4.3 guarantees).

---

## Decision

### 1. Event Loss / Staleness Detection

A watcher (M4.9) or any incremental path may report a **loss signal** (e.g. a
dedicated "stale" notification, or a queue-overflow flag). When the client
observes a loss signal, it **MUST** mark the current incremental index as
stale. The existing `Index::fresh()` method already provides this check: it
re-traverses the directory tree and compares per-file path/size/mtime against
the loaded generation. `fresh() == false` means the index no longer reflects
the filesystem.

### 2. Periodic Rescan

On loss signal **or** a backoff timer (exponential backoff from a base
interval, e.g. 1s → 2s → 4s … capped at 60s), the client performs a **full
re-scan** of the source tree and recomputes `source_identity`:

```cpp
// The existing public API is sufficient:
Index fresh_idx = Index::build(root, options); // canonical path-sorted rebuild
// OR
if (!idx.fresh()) {
    idx = Index::build(root, options); // reconcile by rebuilding
}
```

The recomputed `source_identity` (the FNV hash over root + sorted path/size/mtime
tuples, computed by the private `source_identity(root, opt)` in `index.cpp`) is
compared to the current generation's manifest `source_identity`. Any difference
triggers a rebuild of the affected generation.

### 3. Fingerprint Reconciliation

The contract is: **reconcile == rebuild from the canonical path-sorted tree**.
No incremental replay, no log merge, no heuristic deduplication. A full rebuild
via `Index::build(root, options)` (or `Index::from_documents` with the same
sorted inputs) produces the canonical generation.

Because `source_identity` includes:
- root path (canonical UTF-8)
- sorted `(path, size, mtime_ns)` tuples
- `corpus_bytes`

…any filesystem mutation that the watcher missed (delete+add in one step,
rename, in-place overwrite, permission-only change that alters mtime) changes
the identity. The full rebuild incorporates **all** current files, sorted, with
their current bytes and metadata — exactly the state a fresh `Index::build`
would see.

### 4. Backoff Policy

A naive periodic rescan would re-scan healthy trees unnecessarily. The
recommended client-side policy:

| Attempt | Interval | Cap |
|---------|----------|-----|
| 1 | 1s | |
| 2 | 2s | |
| 3 | 4s | |
| 4 | 8s | |
| … | ×2 each | 60s max |

After a successful reconcile (`fresh() == true`), the backoff resets to the
base interval. The cap prevents a permanently broken watcher from hammering
the filesystem.

### 5. User-Visible Stale Status

The existing `bool Index::fresh() const` is the user-visible stale flag. A
caller that holds an `Index` can query `idx.fresh()` before or after a search.
If `false`, the caller knows the current index **no longer reflects the tree**
and should rebuild (or wait for the background reconciler to do so).

### 6. Convergence Guarantee (Structural, Not Heuristic)

**Any sequence of dropped/reordered/duplicated/coalesced events converges to
exactly the full-rebuild result.** This is a structural property of the design:

- `Index::build` enumerates **all** files under `root`, sorts by path, assigns
  `file_id` by sorted position, and materializes filters from the **current**
  file contents.
- No event log is trusted as the source of truth. The filesystem is the source
  of truth.
- M4.1 guarantees: same file set + same bytes + same metadata → same
  `file_id` mapping + same search results.
- M4.3 guarantees: tombstones (delete/rename) are applied atomically in the new
  generation; the old path is excluded **before** changed/new documents are
  applied.
- Therefore, a rebuild from the canonical tree after any watcher loss produces
  the same `files()`, same `content(file_id)`, same search results as if the
  watcher had never lost events.

---

## No New Library API Required

`fresh()`, `Index::build`, `Index::from_documents`, and `Index::append` (for
incremental paths) already exist. The private `source_identity` is recomputed
inside `build`/`from_documents`. A tiny public helper **is not added** because
the composition is clean and explicit:

```cpp
// Reconciliation is simply:
if (!idx.fresh()) {
    idx = Index::build(root, options); // or from_documents with fresh scan
}
```

Callers that need a one-liner can wrap it locally; the library exposes the
primitives.

---

## Regression Test: `pergrep_m48_rescan`

Located in `tests/test.cpp`. The test block:

1. Builds a canonical `idx0` over `{a.txt, b.txt, c.txt}`.
2. Simulates watcher loss + mutations a naive incremental would miss:
   - (i) **Delete b.txt + add d.txt** (reordered+delete+add in one "coalesced" step)
   - (ii) **Rename a.txt → z.txt**
   - (iii) **Overwrite c.txt in place**
3. After each step, "reconcile" by rebuilding via `Index::build(root)` and asserts:
   - (a) `reconciled.files()` equals a fresh build's `files()` (path set + order)
   - (b) `Searcher(reconciled).find()` equals `Searcher(fresh_build).find()` for
     a literal, a regex, and a word pattern — **zero duplicate/missing matches**;
     deleted content never matches; renamed content appears exactly once under the
     new path.
   - (c) `fresh()` flips `false` after a mutation and `true` after the reconcile
     rebuild.

---

## Consequences

- **Positive**: Formalizes the watcher-recovery contract; convergence is a
  structural guarantee (not a heuristic); reuses existing primitives (`fresh`,
  `build`, `source_identity`).
- **Negative**: Requires M4.9 watcher to emit loss signals or clients to run
  backoff timers. No library enforcement — the contract is documented; callers
  must implement it.
- **Migration**: None — existing code using `fresh()` + `build` already follows
  the pattern; this ADR makes it the official recovery path.

---

## Alternatives Considered

| Alternative | Why Rejected |
|-------------|--------------|
| Incremental log replay with idempotent ops | Complex; requires total ordering, deduplication, tombstone tracking; still diverges on overflow |
| Merkle-tree diff of index state | Overhead for every write; doesn't help when the watcher process dies |
| `Index::reconcile(root)` public method | Not needed — `!fresh() ? build(root) : idx` is already a one-liner; adding it would hide the backoff/rescan policy from the caller |
| Periodic rescan only (no loss signal) | Misses fast recovery when loss is detected; backoff-only is the fallback |

---

## Evidence Anchors

- [`include/pergrep/pergrep.hpp`](../include/pergrep/pergrep.hpp): `Index::fresh()`, `Index::build`, `Index::files()`, `Index::content()`, `Searcher::find`, `Searcher::files`.
- [`src/index.cpp`](../src/index.cpp): `Index::fresh` (O(files) re-traversal), `source_identity` (FNV over sorted path/size/mtime), `Index::build` (canonical path-sorted enumeration).
- [`tests/test.cpp`](../tests/test.cpp): `pergrep_m48_rescan` convergence regression.
- [`docs/generation-consistency.md`](generation-consistency.md): M4.6 immutable generation contract.
- [`docs/incremental-identity.md`](incremental-identity.md): M4.1 stable document identity and path mapping.