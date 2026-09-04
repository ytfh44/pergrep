# ADR-0054: Optional filesystem-watcher daemon

**Status:** Accepted (contract; opt-in only, no default daemon)
**Date:** 2026-09-04
**Scope:** M4.9 / GitHub issue #54
**Decision owner:** M4 (lifecycle); operations and platform maintainers own deployment adapters

## Context

An incremental index can be kept continuously up to date by watching the source
tree and applying changes as they happen. But a background process is only worth
its resource and correctness risk if the incremental snapshot model (M4.1–M4.8)
has demonstrated value and reliability. This ADR specifies the daemon as an
**optional, opt-in, in-process integration point** — never the default execution
path, and never required for correct foreground search.

## Decision

### Lifecycle and ownership

- The **client** owns the daemon: `watch` is started and stopped by the caller in
  the same process; it is not a system service and has no independent lifecycle.
- A single worker reconciles the watched root; no background thread is spawned or
  left running unless the caller opts in.

### IPC / library integration

- No cross-process protocol. The daemon is the in-process loop:
  `for (;;) { wait(event_or_timer); if (stale) reconcile(); }`, where `reconcile`
  is the M4.8 contract (recompute `source_identity`; on mismatch, rebuild).
- A loss signal (queue overflow, watcher restart) marks the held generation stale
  via the existing `fresh() == false` indicator until the next reconcile.

### Resource limits

- Single-worker; the event queue is bounded and, on overflow, downgrades to a
  full periodic rescan (M4.8 backoff) rather than unbounded buffering.
- Platform watchers are **adapters only**: they report events and loss signals,
  never mutate the index directly.

### Idle behavior

- When the tree is unchanged, the daemon sleeps between scans; it never busy-polls.

### Shutdown / restart

- On stop, drain the queue and run one final reconcile.
- On restart, reconcile from scratch; M4.8's convergence guarantee makes the
  result identical to a full rebuild regardless of how many events were dropped,
  reordered, duplicated, or coalesced between shutdown and restart.

### Stale reporting

- `Index::fresh()` is the user-visible stale indicator; a watch loss or an
  un-applied event makes it return `false` until reconcile completes.

### Opt-in configuration

- The opt-in surface is a client flag (documented `--watch`, default **off**).
  No `IndexOptions` field is added: watch is a client/CLI concern, not an index
  semantic, so it must not enter the manifest, `PlanKey`, or `same_options`.
- Foreground search (default `--watch` off) remains fully correct with no
  background process.

## Consequences

- Continuously maintained indexes are available only where the caller opts in
  and accepts the single-worker resource profile.
- Correctness never depends on the daemon: a caller can drop `--watch` entirely
  and get the same results via ordinary `Index::build` + `fresh()`.
- The daemon cannot drift `IndexOptions`, the manifest, or serialization, because
  it never touches the index-semantic surface.

## Evidence anchors

- [`src/index.cpp`](../src/index.cpp): `Index::fresh`, `Index::build`, `source_identity`.
- [`docs/rescan-recovery.md`](rescan-recovery.md): the M4.8 reconcile contract the daemon invokes.
- [`docs/generation-consistency.md`](generation-consistency.md): immutable generations observed by readers.