# ADR-0040: Source-backed corpus residency by default

**Status:** Accepted (lifecycle policy; performance gate remains conditional)
**Date:** 2026-09-01
**Scope:** M3.4 / GitHub issue #40
**Decision owner:** M3; M4 implementations must conform to this snapshot model

## Context

`pergrep` has two different storage questions that must not be conflated:

1. **Where the search corpus comes from at runtime.** A normal repository search should follow the files in the workspace and detect when its index no longer describes that tree.
2. **Whether an index is a self-contained artifact.** An audit, offline run, or hand-off to another machine may need results to remain valid when the source tree is unavailable or changes.

The repository already exposes both modes. `Index::build` reads regular files and builds the filters from their bytes; `Index::from_documents` is the in-memory/ephemeral path. `IndexOptions::persist_corpus` defaults to `false`. A v7 `Index::save` can nevertheless include an optional persisted-corpus section when it is explicitly enabled, and `Index::is_snapshot()` exposes that choice. A source-backed load revalidates the source identity before materializing the loaded bytes; a persisted snapshot restores those bytes from the index instead.

The decision must cover the closed workload classes in [the workload matrix](workload-matrix.md), not just a single microbenchmark:

- **one-shot:** cold construction plus one search on a small local repository;
- **warm-repeated:** repeated searches over one already-built index;
- **interactive-large-repository:** repeated filtered searches over a large repository;
- **batch-multi-pattern:** several heterogeneous patterns over one index.

The matrix also includes `filesystem.cold.roundtrip` and `filesystem.warm-repeated.medium`, and requires lifecycle, RSS, page-fault, logical/physical bytes, and correctness measurements. No benchmark result is checked into this worktree. This ADR therefore does not claim a universal mmap speedup or numeric crossover: the accepted decision is a lifecycle and invalidation policy based on the current API contracts, with the matrix measurements below as a mandatory performance gate before changing the default.

## Decision

**Use a source-backed, read-only corpus view as the default for ordinary local filesystem indexes.** The source-backed form is the product default for `one-shot`, `warm-repeated`, and `interactive-large-repository` searches, and for the filesystem round-trip/warm scenarios in the matrix. An implementation may use OS read-only file mappings for this view, or an equivalent read-only source-backed mechanism, but it must preserve the freshness and coordinate contracts below. The current implementation's equivalent is a source re-read into `Index::Impl::loaded`; this ADR does not claim that it is already using `mmap`.

**Keep the packed immutable corpus as an explicit opt-in, not the default.** Set `IndexOptions::persist_corpus = true` when the caller needs an autonomous point-in-time artifact: offline or reproducible batch work, artifact auditing, a source tree that may be moved or disappear, high-latency network/removable media, or a deliberate cross-process/machine hand-off. Such an index is a snapshot; consumers use `is_snapshot()` and do not require `fresh()` before searching it.

**Treat transformed input as a separate snapshot domain.** Stdin, encoding conversion, `--pre`, archive decompression, and BOM-triggered decoding are represented through `Index::from_documents` after transformation. They remain ephemeral and must not be written into the ordinary source-backed cache key. The current public API rejects persistence for transformed `from_documents` indexes; transformed input therefore remains ephemeral. Supporting explicit transformed snapshots requires a separate API and cache-identity decision, not an assumption in this ADR.

This gives local, mutable workspaces the smaller cache and natural freshness behavior they need, while retaining the packed form where source independence and deterministic bytes are more important than avoiding duplication.

## Options considered

### Option A: Source-backed read-only view — selected default

| Dimension | Assessment |
|---|---|
| One-shot cold | Avoids writing a second copy of the corpus; load still needs source access and metadata/file reads. |
| Warm/repeated | Once resident, repeated searches do not re-open files; the source-backed identity is paid at cache reuse/load time. |
| Interactive large repository | Best lifecycle fit: the index follows the live tree and can be rejected after drift instead of silently returning an old snapshot. |
| Batch multi-pattern | Works when the source tree is stable and available; not sufficient for an autonomous artifact. |
| Memory | No on-disk corpus duplication. A mapping can share read-only OS pages; the current re-read implementation still has a resident `loaded` copy, so lower RSS must be measured rather than assumed. |
| I/O and locality | Many source files mean metadata/file-open work on load. File placement controls locality; a future mapping must not assume contiguous corpus access. |
| Changed/deleted/renamed files | Detectable through the source fingerprint and `fresh()` when size or nanosecond mtime changes, or when the regular-file set/path set changes. |
| Network/removable media | Requires the source root on every load/revalidation and is exposed to availability and metadata latency. |
| Windows sharing | A mapping implementation must request read/write/delete sharing explicitly; this ADR does not rely on unspecified CRT `ifstream` sharing behavior. |
| Transformed input | Not valid for transformed bytes; use the ephemeral path. Explicit transformed snapshots are not currently supported by the public API. |

**Pros:** compact cache, no raw-source duplication, natural live-workspace invalidation, and a good fit for the interactive classes.
**Cons:** source availability is required; load/revalidation can touch many files; a freshness check cannot close every TOCTOU window; same-size edits that preserve mtime are not detected.

### Option B: Packed immutable corpus blob — retained alternative

| Dimension | Assessment |
|---|---|
| One-shot cold | One index file can be read sequentially, but save and storage write the corpus a second time. |
| Warm/repeated | Strong fit when the source is remote/unavailable or when one artifact is consumed by many repeated queries/processes; contiguous payload locality may reduce opens, but the win must be measured. |
| Interactive large repository | Wrong default: it freezes a point in time and duplicates a changing repository. It is useful only when the caller explicitly wants frozen results. |
| Batch multi-pattern | Strong fit for offline/reproducible batches and hand-offs where source independence matters. |
| Memory | The persisted file is larger by approximately the raw corpus bytes. After load, both current modes populate `loaded`, so packed mode is not promised to reduce steady-state RSS. |
| I/O and locality | One payload stream and fewer source opens can improve sequential/page locality; save/load must pay serialization and checksum work. |
| Changed/deleted/renamed files | The packed bytes and paths remain unchanged by source mutations. That is the snapshot guarantee, not freshness. A policy that needs the newest source must compare an external generation/source identity and repack. |
| Network/removable media | Source-independent after creation; only the packed index must remain accessible. This is the preferred mode when the source cannot be reliably re-read. |
| Windows sharing | Search/load need not retain source handles. The index file still needs normal read sharing and should be created/replaced with the repository's crash-safe same-volume rename rules. |
| Transformed input | Not available through the current public API; a future transformed-snapshot design must bind transformation identity into the artifact/cache identity. |

**Pros:** deterministic coordinates, no source dependency after creation, one sequential payload, and a portable hand-off artifact.
**Cons:** raw corpus duplication, larger cache and transfer size, stale-by-design paths/content, and cleartext corpus exposure requiring the access controls in [the freshness and cache-security contract](freshness-and-cache-security.md).

## Workload choice

| Target workload | Residency decision | Reason |
|---|---|---|
| `one-shot` on a local, ordinary repository | Source-backed default | The cold path should not create a second raw corpus copy. |
| `warm-repeated` on a stable local repository | Source-backed default | Repeated queries reuse one resident index; source freshness is checked at reuse boundaries. |
| `interactive-large-repository` | Source-backed default | Metadata-detectable additions, deletions, renames, size changes, or mtime changes must cause cache rejection/rebuild rather than silently changing the meaning of coordinates. |
| `batch-multi-pattern` with an available stable tree | Source-backed is sufficient | One loaded index serves all patterns; pack only when source independence or reproducibility is required. |
| Offline/reproducible batch, audit artifact, or source unavailable | Packed opt-in | The artifact must own its bytes and remain searchable after source movement or mutation. |
| `oneshot.transformed.*`, stdin, `--pre`, `--search-zip`, or encoding conversion | Ephemeral `from_documents` | The searched bytes are not the source bytes and must not enter the ordinary source-backed cache. The current public API does not persist this path. |

## Invalidation and correctness rules

### Source-backed indexes

1. Manifest-aware reuse requires the caller to supply and match the requested root, selector, index options, transform identity, and corpus counts. The current CLI uses unqualified `Index::load()` and then `fresh()`, so it relies on the embedded source identity and does not provide selector/transform expectations.
2. `fresh()` re-traverses with the same hidden-file and symlink policy, compares the sorted regular-file path set, then compares each file's size and nanosecond mtime. Any added, deleted, renamed, resized, or mtime-changed file is stale and must trigger cache invalidation and rebuild.
3. A same-size edit that preserves the recorded mtime is outside the metadata fingerprint. Callers requiring adversarial or byte-for-byte freshness must use a content hash policy or a packed snapshot; they must not treat `fresh() == true` as a cryptographic proof.
4. A file can change after `fresh()` and before verification (TOCTOU). A live search that requires stable coordinates must use a packed snapshot or an equivalent external stability protocol; source-backed mode is not silently upgraded to snapshot semantics.
5. A source disappearance or failed source read invalidates the source-backed cache. It must never be replaced by a packed interpretation of the same cache file.

### Packed snapshots

1. `is_snapshot()` is the residency discriminator. A packed snapshot is valid from its own v7 checksum/structure and manifest checks; callers do not gate search on `fresh()`.
2. Source changes, deletion, or rename do not invalidate the bytes in a packed snapshot. They make the snapshot older than the source. A “latest source” cache policy must track a separate source generation and explicitly repack.
3. Schema/version, checksum, index options, selector identity, source-root policy, corpus counts, or transform identity mismatches invalidate the artifact. A transformed artifact must never be reused for a different transformation pipeline.
4. Paths are frozen at snapshot creation. Results intentionally refer to the stored path names even if the original tree later renames or deletes those files.
5. Because the packed payload contains cleartext corpus bytes, cache permissions/ACLs must follow [the cache-security rules](freshness-and-cache-security.md). The current save path still requires platform-specific permission hardening; this ADR does not treat that requirement as already enforced.

## Trade-off analysis and measurement gate

The selected default is a lifecycle decision, not a claim that source mapping is always faster. The matrix's filesystem scenarios must compare both modes using the same corpus, patterns, and correctness reference, recording at least:
The current benchmark matrix does not yet run both residency modes or provide mutation/network/removable-media arms; these are future release-gate scenarios, not completed measurements.

- cold build/search and warm/repeated search latency;
- save and load time, including packed serialization/checksum cost;
- freshness/revalidation time and the cost of changed-file rejection;
- RSS/peak RSS and page faults;
- index bytes versus corpus bytes, deduplicated logical bytes, and physically touched/verified bytes;
- behavior on local SSD, a network share, and removable media where those environments are part of the release gate;
- mutation cases: edit, same-size edit with preserved mtime, append/truncate, deletion, rename, and source disappearance;
- Windows open/rename/delete behavior with explicit sharing flags.

A measured result may refine thresholds or select a packed snapshot for a particular deployment profile, but it must not change the default for mutable local workspaces without an ADR update. The packed alternative remains available behind `persist_corpus=true` for the named snapshot workloads.

## Consequences

- Local caches remain small and do not duplicate every repository's cleartext bytes by default.
- M4 can implement residency-aware operators against the `is_snapshot()`/`fresh()` contract instead of guessing from file layout.
- Offline and reproducible consumers get a supported, deterministic packed artifact rather than relying on a live source tree.
- Source-backed load/revalidation continues to incur filesystem metadata and content I/O, and live search retains a bounded TOCTOU risk.
- Packed artifacts consume additional disk and transfer bandwidth, expose cleartext corpus bytes, and intentionally become stale relative to later source changes.
- A future true mmap implementation must preserve the source-backed invalidation contract and use explicit Windows sharing; changing residency alone is not permission to weaken freshness checks.

## Evidence anchors

- [`include/pergrep/pergrep.hpp`](../include/pergrep/pergrep.hpp): `IndexOptions::persist_corpus` default and comments; `Index::is_snapshot()`, `Index::fresh()`, and `Index::content()` API.
- [`src/index.cpp`](../src/index.cpp): `Index::build`, `Index::from_documents`, `Index::fresh`, v7 save/load, manifest validation, packed payload restoration, and source re-read behavior.
- [`src/cli.cpp`](../src/cli.cpp): cache load plus `fresh()` gate and transformed-input rebuild through `Index::from_documents`.
- [`bench/workload_matrix.hpp`](../bench/workload_matrix.hpp) and [`docs/workload-matrix.md`](workload-matrix.md): closed workload classes, storage scenarios, and required measurements.
- [`docs/freshness-and-cache-security.md`](freshness-and-cache-security.md): live-view/snapshot taxonomy, TOCTOU limits, and cleartext-cache permission rules.
