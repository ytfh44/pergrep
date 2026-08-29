# BF-4 Audit — Crash-safe & Cross-platform Serialization Hardening

**Date:** 2026-08-30
**Branch:** BF-4-hardening
**Scope:** `src/index.cpp` (`Index::save`/`load`), `src/internal.hpp` (Chunk/PosDesc), `tests/test.cpp`
**Worktree:** `.wt/bf4`

## Summary

BF-4 hardens index persistence for crash safety and for corrupted/truncated input, and documents cross-platform serialization non-portability. All `ctest 5/5` pass; no false negatives introduced.

## Crash-safety (`Index::save`)

- **Atomic replace:** `save()` now writes to `<path>.tmp.<pid>` then `fs::rename(tmp, file)`. `fs::rename` is atomic on POSIX and on Windows via `MoveFileExW` (same volume). A crash or power loss mid-write leaves the previous index (or no file) rather than a half-written file.
- **Parent creation:** `fs::create_directories(file.parent_path())` is called before any write, so save to `a/b/c/index.bin` with non-existent parents succeeds.
- **Flush + error check:** After writing the v5 format, `o.flush()` is called and `!o` is checked before rename. On failure the temp is removed and an exception is thrown. `close()` is also checked. The whole write is wrapped in try/catch with best-effort temp removal so stale `.tmp.<pid>` files do not accumulate.
- **Stale temp cleanup:** Any pre-existing temp with same name is removed before writing.

## Truncation / OOM hardening (`Index::load`)

- **Header size guard:** `load()` first checks `fs::file_size` against `kMinHeader = 12` (magic 8 + version 4). If smaller, throws `pergrep index: truncated` instead of attempting reads that would produce `string too long` or `bad_alloc`.
- **Magic / version:** Short read or mismatch now throws `pergrep index: truncated`.
- **String size bound:** `gets()` validates `n <= 16 MiB`; otherwise throws `pergrep index: truncated` before `string::resize`, preventing `std::length_error: string too long` / OOM on corrupted length prefixes.
- **Count bounds:** `nf`, `nc`, `npd` validated against `kMaxFiles = 10M`, `kMaxChunks = 100M`, `kMaxPosDesc = 100M`. Exceeding throws `pergrep index: truncated` instead of huge `reserve` + OOM. `getv` validates `n <= 200M` and overflow of `n * sizeof(T)`.
- **Stream failure mapping:** `get<T>` and `getv` throw `pergrep index: truncated` on `!i`, so any EOF mid-field is reported as truncation rather than generic `index read failed`. Tests accept either substring `truncated` or `read failed`.
- **Deferred file I/O:** File contents are reloaded from `I->root / f.path` only after all `infos` are parsed, so a truncated header fails fast without touching the filesystem.

## Cross-platform serialization

- **Host-endian, not portable:** `put<T>` / `get<T>` write raw host bytes (`sizeof(T)` via `memcpy`). On x86_64 this is little-endian. Comment added in `src/index.cpp` stating the index is host-endian and not portable across architectures (e.g., x86_64 vs big-endian). bumping to v5 is unrelated; portability would require explicit little-endian shifts for `uint32_t`/`uint64_t` etc., which is deferred.
- **Field-by-field:** `Chunk` (`file_id`, `core_begin`, `core_end`, `ext_end`) and `PosDesc` (`off`, `m`, `mask_bytes`, `blocks`) are serialized field-by-field with individual `put` calls. No `putv<Chunk>` or `putv<PosDesc>` remains, so struct padding does not leak into the file and layout is stable across compilers.
- **Verified:** `grep` for `putv.*Chunk` / `putv.*PosDesc` returns no matches. `groups[].gids` / `bits` and `pos` remain as `putv<uint32_t>` etc., which are well-defined element types without padding.

## Known non-portability (documented, not fixed)

- Index files are host-endian and host-size_t agnostic only for fixed-width types; they cannot be moved between little- and big-endian hosts. This is acceptable for a local cache (index lives beside the corpus on one machine). A future `v6` could switch to explicit little-endian varint encoding if cross-arch sharing is required.
- `double positional_budget_ratio` is written as raw `double` bits; NaN payloads and endianness are host-dependent but the value is advisory (recomputed on rebuild) so cross-host mismatch is harmless.

## Tests added (`tests/test.cpp` — BF-4 hardening)

- **Parent creation + crash-safe round-trip:** Save to `tmp/bf4_nonexistent_parent/subdir/index.bin` where parent does not exist; verify `create_directories` behavior, temp file is removed, final file loads and content matches.
- **Truncated file:** Create a valid index, truncate to 10 bytes and to half size, verify `Index::load` throws with message containing `truncated` or `read failed` and does not segfault; also test absurd `nf` via corrupted header yields `truncated`.
- **Chunk large-offset round-trip:** Verify `Chunk` field-by-field encoding preserves offsets >4 GiB by constructing an `Index` via `from_documents`, mutating `IndexData::chunks` to large values (5 GiB, 10 GiB), saving and reloading, and asserting equality. This exercises 64-bit fields without allocating 4 GiB of content.

## Verification

```
cmake --preset windows-clang
cmake --build --preset windows-clang
ctest --preset windows-clang -V   # 5/5
```

## References

- `src/index.cpp:save` — temp+rename, flush check, field-by-field Chunk/PosDesc comments
- `src/index.cpp:load` — file_size guard, kMax* bounds, truncated messages
- `src/internal.hpp:PosDesc` / `Chunk` — field-by-field note
