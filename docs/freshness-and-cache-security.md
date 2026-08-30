# M0.8: Freshness, Snapshot, and Cache-Security Contracts

**Version: 1.0** (Corresponds to Workload Matrix v2, M0.8 / Issue #19)

This document establishes the formal specification for **corpus freshness validation**, **live view vs. immutable snapshot taxonomy**, **time-of-check to time-of-use (TOCTOU) semantics**, **cache security & access control**, and **binary format integrity & corruption defense** in `pergrep`.

---

## 1. Executive Summary & Core Invariants

`pergrep` operates across two primary usage paradigms:
1. **Interactive developer / live workspace search**: High-speed indexing over an active filesystem tree where files may undergo concurrent edits, creation, or deletion.
2. **Deterministic snapshot search & artifact auditing**: Offline or containerized execution against immutable datasets where search results must be 100% reproducible and isolated from external filesystem mutations.

To serve both workflows without sacrificing performance or introducing silent data corruption:
- **Explicit Taxonomy**: Indexes are categorized as either **Source-Backed Live Views (v5)** or **Self-Contained Immutable Snapshots (v6)**, exposed via the `Index::is_snapshot()` query API.
- **Strict Freshness Contract**: Live view indexes provide an $O(\text{files})$ metadata validation pass via `Index::fresh()`, detecting filesystem drift before executing verification against underlying source files.
- **TOCTOU Hazard Isolation**: Immutable snapshots decouple candidate verification from the live filesystem by persisting exact document byte sequences into the index, eliminating time-of-check to time-of-use divergence.
- **Zero-Trust Cache Security**: Index cache files containing persisted cleartext corpus bytes must enforce strict user-private access controls (POSIX `0600`/`0700` and restricted Windows NTFS ACLs) to prevent cross-user privilege escalation.
- **Fail-Safe Integrity Gates**: Deserialization is protected by multi-tier validation guards: 8-byte magic header checks, strict version gating, allocation bounds, and explicit truncation detection.

---

## 2. Metadata Fingerprint Strength & TOCTOU Behavior

### Metadata Fingerprint Structure

`pergrep` tracks filesystem state using a composite per-file metadata fingerprint tuple $\mathcal{F}(f) = (P_f, S_f, T_f, B_f)$ alongside the global directory set $\mathcal{D}$:
- **$P_f$ (Normalized Path)**: Lexically relative path from the index root in normalized UTF-8 representation (`f.path`), sorted lexicographically.
- **$S_f$ (File Size)**: 64-bit unsigned integer representing exact file length in bytes (`f.size`).
- **$T_f$ (Modification Timestamp)**: 64-bit signed integer capturing nanosecond-precision timestamp (`f.mtime_ns`).
- **$B_f$ (Binary Classification)**: Boolean flag indicating presence of NUL bytes within the first 8 KiB prefix (`f.binary`).
- **$\mathcal{D}$ (Directory Topology)**: Total count and canonical paths of visited directories, accounting for hidden file exclusions and symlink recursion policies.

### Complexity: $O(\text{files})$ Traversal vs. $O(\text{corpus})$ Content Hashing

| Dimension | Metadata Fingerprinting (`pergrep::Index::fresh`) | Full Content Hashing (SHA-256 / BLAKE3) |
| --- | --- | --- |
| **Computational Complexity** | $O(N_{\text{files}})$ directory traversal + `stat` calls | $O(N_{\text{files}}) + O(B_{\text{corpus}})$ full payload read and cryptographic digest |
| **I/O Operations** | Metadata only (directory dentries and inode / `FILE_BASIC_INFO` queries) | Complete I/O bandwidth consumption across every byte of every indexed file |
| **Throughput** | $>100,000$ files/sec on modern SSDs and OS buffer caches | Bound by disk read throughput (500 MB/s – 5 GB/s) and hashing CPU cores |
| **Collision / False Freshness** | Vulnerable to intentional timestamp tampering (`touch -m -d`) with identical file size | Cryptographically collision-resistant ($< 2^{-128}$ probability) |
| **Design Fit** | Optimal for high-frequency interactive search loops and build-system fast paths | Unnecessary overhead for local development indexing |

The $O(\text{files})$ metadata fingerprint provides the optimum trade-off for interactive search: it validates thousands of files in milliseconds without saturating disk bandwidth.

### Time-of-Check to Time-of-Use (TOCTOU) Analysis

In source-backed indexing systems, a fundamental concurrency hazard exists between the moment an index is constructed (or checked for freshness) and the moment candidate matches are verified:

```
Timeline:
t0: Index::build(root) -> extracts q-grams, stores metadata F0, offsets O0
    ...
t1: User/Process modifies root/doc.txt (appends, deletes, or shifts text)
    ...
t2: Searcher::find(pattern) -> filter matches chunk at offset O0
t3: Searcher verifies candidate by reading live file root/doc.txt
    -> Slices text at offset O0 (which now contains shifted/different data)
    -> FAILS: Yields false positive, corrupted coordinates, or false negative!
```

#### TOCTOU Hazard Scenarios

1. **Content Mutation In-Place (Identical Size & Preserved mtime)**: If a fast in-place edit modifies content without triggering timestamp or size change (sub-nanosecond resolution or clock skew), candidate verification reads stale bytes.
2. **File Shrinkage / Truncation**: A candidate chunk referencing offset $[10000, 20000]$ reads past EOF if the file was truncated to 5000 bytes, resulting in partial reads or out-of-bounds slice indexing.
3. **File Deletion**: A candidate file deleted at $t_1$ causes `std::ifstream` failure during v5 load or live verification, throwing `runtime_error("indexed source disappeared")`.
4. **File Insertion**: New files created at $t_1$ containing matching patterns are silently omitted from search results until a re-index occurs.

#### Resolution by Index Mode
- **Source-Backed Live View (v5)**: Relies on `Index::fresh()` as an explicit gate. If `fresh()` returns `false`, callers must invalidate cache and rebuild the index before trusting match coordinates.
- **Immutable Snapshot (v6)**: Completely immune to TOCTOU divergence. Because document contents are frozen directly in the index payload (`I->loaded`), verification never touches the live filesystem. Coordinates and match validity remain 100% deterministic regardless of concurrent external mutations.

---

## 3. Snapshot vs. Live View Taxonomy

`pergrep` formalizes two distinct index operational modes:

```
                               +-----------------------------+
                               |     pergrep::Index Data     |
                               +-----------------------------+
                                              |
                     +------------------------+------------------------+
                     |                                                 |
                     v                                                 v
      [ v5: Source-Backed Live View ]                 [ v6: Persisted Immutable Snapshot ]
      - persist_corpus = false                        - persist_corpus = true
      - Index::is_snapshot() == false                 - Index::is_snapshot() == true
      - Filter + Metadata only on disk                - Filter + Metadata + Full Content on disk
      - Re-reads files from disk on load              - Zero filesystem I/O on load
      - Dependent on live filesystem                  - Autonomous, frozen point-in-time
      - Subject to TOCTOU divergence                  - Fully deterministic & reproducible
```

### Behavioral & Architectural Comparison

| Characteristic | Source-Backed Live View (v5) | Persisted Immutable Snapshot (v6) |
| --- | --- | --- |
| **Persistence Option** | `IndexOptions::persist_corpus = false` | `IndexOptions::persist_corpus = true` |
| **Disk Format Version** | Version `5` | Version `6` |
| **Query API (`is_snapshot`)** | `idx.is_snapshot() == false` | `idx.is_snapshot() == true` |
| **C API (`pg_index_is_snapshot`)**| `pg_index_is_snapshot(idx) == 0` | `pg_index_is_snapshot(idx) == 1` |
| **Index File Size** | Compact: $\approx 1\% - 5\%$ of corpus size (filter bitsets + metadata) | Larger: filter bitsets + metadata + 100% corpus raw bytes |
| **Load Complexity** | $O(N_{\text{files}})$ file opens + $O(B_{\text{corpus}})$ file read I/O | $O(1)$ file open + sequential single-stream deserialization |
| **Filesystem Dependency** | Mandatory. Source files must exist at original relative paths. | None. Source files can be moved, modified, or deleted. |
| **Search Coordinate Stability**| Dependent on source file stability. | Immutable and guaranteed stable for the index lifetime. |
| **In-Memory Representation** | Populated `loaded` vector via source re-read. | Populated `loaded` vector via index payload deserialization. |

### API Semantics

#### C++ API (`include/pergrep/pergrep.hpp`)
```cpp
class Index {
public:
    // Returns true if the index is a self-contained snapshot with persisted corpus
    // (built or loaded with persist_corpus == true / v6).
    // Returns false if the index is a source-backed live view (v5) or uninitialized.
    bool is_snapshot() const noexcept;

    // Validates whether the live filesystem matching root() still matches the
    // index metadata fingerprint. Returns false for ephemeral or stale indexes.
    bool fresh() const;
};
```

#### C API (`include/pergrep/pergrep_c.h`)
```c
// Returns 1 if the index is a self-contained snapshot (v6), 0 if source-backed (v5) or null.
int pg_index_is_snapshot(const pg_index* index);
```

---

## 4. Stale-Read & Freshness Policy

### Freshness Verification Workflow (`Index::fresh()`)

When `Index::fresh()` is called:
1. **Uninitialized & Ephemeral Check**: If `impl_` is null or `impl_->ephemeral` is true (e.g. built via `Index::from_documents`), returns `false` (no backing filesystem root to validate).
2. **Directory Traversal**: Re-traverses `impl_->root` using `std::filesystem::recursive_directory_iterator` with identical traversal options (`include_hidden`, `follow_symlinks`, `skip_permission_denied`).
3. **File Set Cardinality & Path Matching**:
   - Collects all regular file paths relative to `root()`.
   - Compares total regular file count against `infos.size()`. Any discrepancy immediately returns `false`.
   - Compares sorted relative path strings one by one. Any added, deleted, or renamed file immediately returns `false`.
4. **Metadata Fingerprint Equality**:
   - For every indexed file, queries filesystem size and modification time with zero-exception error codes (`std::error_code`).
   - If `file_size(path) != f.size` or `mtime_ns(path) != f.mtime_ns`, returns `false`.
5. **Success**: Returns `true` if and only if every file matches the indexed fingerprint exactly.

### Caller Decision Matrix

```
                          +------------------------+
                          |   Caller Has Cached    |
                          |      Index Handle      |
                          +------------------------+
                                       |
                                       v
                        +----------------------------+
                        |  Check idx.is_snapshot()   |
                        +----------------------------+
                               /              \
                              /                \
                       [ false ]              [ true ]
                            /                    \
                           v                      v
                +---------------------+   +-------------------------------+
                | Call idx.fresh()    |   | Immutable Point-in-Time       |
                +---------------------+   | - Zero TOCTOU hazard          |
                    /             \       | - Content guaranteed frozen   |
                   /               \      | - Search immediately          |
             [ true ]           [ false ] +-------------------------------+
                /                    \
               v                      v
     +-------------------+   +------------------------------------+
     | Execute Search    |   | Invalidate Cache & Rebuild Index   |
     | - Source files    |   | - Filesystem has drifted           |
     |   unchanged       |   | - Prevent stale/invalid matches    |
     +-------------------+   +------------------------------------+
```

---

## 5. Cache Security, Privacy & ACL Model

### Cleartext Corpus Exposure in v6 Files

When `persist_corpus == true` (v6), `pergrep` serializes the verbatim byte sequences of all indexed documents into the binary index file.
- **Data Sensitivity**: If the indexed repository contains source code, configuration files, environment definitions (`.env`), private keys, or credentials, those cleartext bytes are duplicated directly into the `.bin` index file.
- **Risk Profile**: An index file placed in a shared directory (`/tmp`, `/var/tmp`, or world-readable project caches) exposes sensitive repository content to unprivileged local users, even if the original source directory had restricted permissions.

### Filesystem Permission & ACL Rules

To prevent information disclosure and local privilege escalation:

#### 1. POSIX File Mode Guardrails
- **File Permissions**: Cache index files MUST be written with mode `0600` (`-rw-------`), permitting read/write access solely to the file owner.
- **Directory Permissions**: Index cache storage directories (e.g. `~/.cache/pergrep`) MUST enforce mode `0700` (`drwx------`).
- **Process Umask**: Tooling managing automated cache generation SHOULD set `umask(0077)` prior to creating cache directories and files.

#### 2. Windows NTFS Access Control Lists (ACLs)
- Index files MUST inherit permissions strictly from user-private directories (`%LOCALAPPDATA%\pergrep\cache` or `%USERPROFILE%\.cache\pergrep`).
- If created under shared temporary locations (`GetTempPathW()`), explicit security descriptors (`SECURITY_ATTRIBUTES`) restricting access to `CURRENT_USER_SID` and `SYSTEM` (blocking `BUILTIN\Users` and `Everyone`) MUST be enforced.

#### 3. Prohibited Locations
- Storing unencrypted v6 snapshot indexes in world-writable or shared multi-user locations without dedicated private subdirectories is **strictly prohibited**.

---

## 6. Integrity, Corruption & Truncation Handling

`pergrep` implements a defense-in-depth deserialization pipeline designed to fail fast with structured exceptions (`std::runtime_error`) rather than crash, loop indefinitely, or perform out-of-memory allocations on corrupted input.

```
Binary Layout:
+-------------------+-----------------+----------------------------------------+
| Magic (8 bytes)   | Version (4B)    | Metadata, Options & Filter Blocks      |
| "PERGREP\0"       | uint32_t (5 / 6)| Groups, Bits, Positional Descriptors   |
+-------------------+-----------------+----------------------------------------+
| Positional Matrix | (v6 only) Corpus Stream                                  |
| pos vector bytes  | [size_64, raw_bytes] * nf                                |
+-------------------+----------------------------------------------------------+
```

### Structural Defense Gates

#### 1. Header Magic Verification
- The first 8 bytes must match the exact sequence `PERGREP\0` (`0x50, 0x45, 0x52, 0x47, 0x52, 0x45, 0x50, 0x00`).
- Any mismatch, NUL byte substitution, or non-matching magic immediately throws `std::runtime_error("pergrep index: truncated")`.

#### 2. Strict Version Gating
- Bytes 8–11 encode the 32-bit unsigned little-endian format version.
- Currently valid versions are **`5`** (v5 source-backed) and **`6`** (v6 persisted corpus snapshot).
- Any other version (e.g. `0`, legacy `1..4`, future `7+`, or corrupted values like `0xFFFFFFFF`) is rejected immediately:
  ```cpp
  if (ver != 5 && ver != 6) throw std::runtime_error("unsupported pergrep index version");
  ```

#### 3. Vector Length Bounding & OOM Prevention
Corrupted index headers may report astronomical element counts (e.g. $2^{64}-1$). `pergrep` enforces hard upper bounds before allocating vectors:
- **Max File Count**: `nf <= 1,000,000` (`kMaxFiles`).
- **Max Chunk Count**: `nc <= 10,000,000` (`kMaxChunks`).
- **Max Positional Descriptors**: `npd <= 10,000,000` (`kMaxPosDesc`).
- **Max Corpus Size per File (v6)**: `n <= 512 MiB` (`kMaxCorpusPerFile`). Prevents memory exhaustion attacks where a corrupted size field forces `std::string::resize` to allocate gigabytes of memory.

#### 4. Stream State & Truncation Detection
- Prior to reading, the file size is verified against the minimum header size ($8 + 4 = 12$ bytes).
- Every deserialization primitive (`get<T>`, `getv<T>`, `gets`, `i.read`) checks the `std::ifstream` state.
- If EOF is reached unexpectedly or stream failbit/badbit is set during any field read, loading terminates with:
  ```cpp
  throw std::runtime_error("pergrep index: truncated");
  ```

#### 5. Atomic Write & Crash Consistency
To prevent partial index files from persisting across crashes or power interruptions:
- `Index::save` writes to a temporary file (`<target>.tmp.<pid>`).
- Once fully flushed and closed, the temporary file is atomically moved to the destination via `std::filesystem::rename` (which invokes atomic POSIX `rename` or Win32 `MoveFileExW`).
- On any exception during serialization, a best-effort cleanup removes the temporary file without masking the originating error.

---

## 7. Specification Summary & Checklist

| Requirement | Contract Specification | Enforcement Mechanism |
| --- | --- | --- |
| **Snapshot Query** | Callers can programmatically query index lineage | `Index::is_snapshot()`, `pg_index_is_snapshot()` |
| **Freshness Check** | Detects path, count, size, and nanosecond mtime divergence | `Index::fresh()` ($O(\text{files})$ traversal) |
| **TOCTOU Immunity** | v6 indexes isolate search verification from live filesystem | In-index corpus serialization (`persist_corpus = true`) |
| **Cache Privacy** | Uncompressed corpus cache protected from unauthorized local access | POSIX `0600`/`0700` & restricted NTFS ACLs |
| **Magic Header** | Rejects non-pergrep or malformed files | `PERGREP\0` 8-byte prefix validation |
| **Version Enforcement**| Accepts only supported versions | Strict `ver == 5 || ver == 6` gate |
| **Corruption Defense**| Bounded allocations and stream state checks | `kMaxFiles`, `kMaxChunks`, `kMaxPosDesc`, `kMaxCorpusPerFile` |
| **Crash Safety** | Never exposes partially-written index files | PID-stamped temporary file + atomic rename |
