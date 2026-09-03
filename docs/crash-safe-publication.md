# ADR-0052: Crash-Safe Segment/Snapshot Publication

**Status**: Accepted  
**Date**: 2026-09-03  
**Related**: M3.3/BF-4 (index integrity), M4.7 (crash-safe publication)

## Context

Pergrep persists index snapshots to disk. A crash during write must never leave a partial/corrupt file that could be mistaken for a valid snapshot on the next load. The existing `Index::save` (M3.3/BF-4) already implements:

- **Temporary file**: writes go to `file.tmp.<pid>` in the same directory
- **Flush + checksum**: full payload is written, flushed, then a trailing 8-byte FNV-1a checksum is appended
- **Atomic rename**: `fs::rename(tmp, file)` replaces the final path atomically (POSIX `rename` / Windows `MoveFileExW` same-volume)

However, the contract was not formally documented for segment/snapshot publication, and no startup orphan cleanup or fault-injection regression test existed.

## Decision

Define the crash-safe publication contract for all segment/snapshot writes and add a minimal startup recovery helper.

### 1. Temporary Files

- All writes use a same-directory temp file: `final_path + ".tmp." + <pid> + "." + <counter>`
- The `<pid>` is the process ID (`_getpid` on Windows, `getpid` on POSIX)
- The `<counter>` distinguishes concurrent saves from the same process
- Temp files are **never** the final path; readers only ever open the final path

### 2. Generation Manifests

The final artifact (v7 portable format) carries:

- **Schema version** (`kManifestSchema`)
- **Magic** (`kManifestMagic = 0x43414348455F4D46` = "CACHE_MF")
- **Feature flags**: `kFeatureIntegrityChecksum` (mandatory for v7), `kFeaturePersistedCorpus`
- **Integrity checksum**: trailing 8-byte FNV-1a over bytes `[12, file_size-8)`
- **Manifest fields** (validated at load):
  - `source_identity` — hash of root path + index options
  - `selector_identity` — reserved (0)
  - `transform_identity` — reserved (0)
  - `corpus_files`, `corpus_bytes` — totals
  - `generation` — root mtime_ns (monotonic across rebuilds)

Load validates schema, magic, feature flags, checksum, and manifest coherence before any allocation.

### 3. Atomic Commit Order

1. Build complete payload in memory/stream
2. Write to temp file (`ofstream::binary | trunc`)
3. `flush()` + verify stream state
4. Compute checksum over `[12, body_end)`; append as little-endian `uint64_t`
5. `fs::rename(tmp, final)` — atomic on same volume
6. On any failure before rename: remove temp, propagate error

### 4. Startup Recovery / Orphan Cleanup

- Any stale temp matching `*.tmp.<pid>.<n>` left by a crashed writer is removed on the next successful `save` or `load`
- New public static helper:
  ```cpp
  static bool Index::cleanup_orphans(const fs::path& dir, std::string_view pattern = ".tmp.");
  ```
  - Iterates `dir`, removes regular files whose filename contains `pattern`
  - Returns `true` if any removed
  - No-throw (uses `error_code` internally)
- Wired into `load_impl` start (best-effort, before opening target):
  ```cpp
  Index::cleanup_orphans(file.parent_path(), ".tmp.");
  ```

### 5. Concurrent Writer Behavior

- **Last-writer-wins** on the atomic rename
- Each writer uses a unique temp (`pid + counter`) so concurrent writers cannot corrupt each other's temp
- Readers only ever open the final path (post-rename), so they see either the old or new complete snapshot — never a partial write

## Implementation

- `src/index.cpp`: `cleanup_orphans` implementation (~25 lines), called at `load_impl` entry
- `include/pergrep/pergrep.hpp`: public declaration
- `tests/test.cpp`: regression block `pergrep_m47_crash_safe`

## Test Coverage (`pergrep_m47_crash_safe`)

1. **Valid save/load round-trip**: save snapshot → assert final exists → reload → search correct
2. **Orphan cleanup**: create `final.pgi.tmp.12345.0` (truncated garbage) + `final.pgi.tmp.99999.5` → call `cleanup_orphans` → assert both removed, returns `true` → final still loads
3. **Atomic commit property**: write temp, **do not rename** → assert final untouched (previous snapshot loads, search correct) → complete rename → assert new snapshot loads
4. **Concurrent-writer smoke**: two sequential `save()` to same final → both succeed, last one loads, no leftover temps in dir

## Consequences

- **Positive**: formalizes existing M3.3 behavior; adds observable recovery; regression test catches regressions
- **Negative**: minimal API surface addition (`cleanup_orphans`); no format/ABI change
- **Migration**: none required — existing v7 snapshots load unchanged

## Alternatives Considered

- **File locking**: adds cross-platform complexity, not needed for last-writer-wins semantics
- **Separate cleanup tool**: overkill; inline no-throw helper is sufficient
- **Periodic background cleanup**: unnecessary — orphan temps are rare and harmless; startup reclamation is enough