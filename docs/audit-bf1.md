# BF-1 Audit — Remaining Regex / CLI Corners

**Date:** 2026-08-30  
**Branch:** BF-1-audit  
**Scope:** `.wt/bf1/src/cli.cpp`, `src/regex.cpp`, `src/platform.hpp`, `src/index.cpp`, `tests/*.sh`, `tests/test.cpp`

## Summary

Audit found **2 true bugs** and **2 known limitations** documented below. All `ctest 5/5` continue to pass after fixes.

## Bugs Fixed

### 1. `src/platform.hpp:fnmatch` — missing `**` (double-star) handling

*Symptom:* `--glob '**/*.txt'` only matched depth 1 (`a/file.txt`) and missed `a/b/c/file.txt` and `file.txt`; `a/**/*.txt` matched nothing.  
*Root cause:* `fnmatch` treated consecutive `*` as two independent single-star wildcards that both stop at `/`, so `**` could never cross directory boundaries.  
*Fix:* Detect `**` at `pat[i]` and treat it as "match anything including `/`". Special-case `**/` to match zero or more directories so `**/*.txt` matches `file.txt` at depth 0 (parity with gitignore/ripgrep). Single `*` retains pathname semantics.

### 2. `src/cli.cpp:globmatch` — case-insensitive globs were ASCII-only

*Symptom:* `--iglob '*CAFÉ*'` did not match file `café.txt`; only ASCII `A-Z` was folded.  
*Root cause:* Previous BF-1 fix correctly changed `std::tolower` (locale-sensitive) to ASCII-only, but left `iglob` without Unicode folding. On Windows the pattern and path both contain Latin-1 `é/É` (2-byte UTF-8) which differ only by Unicode case.  
*Fix:* Fold both `pat` and `path` per code-point using ICU `u_foldCase` + `U8_NEXT`/`U8_APPEND_UNSAFE` before delegating to `fnmatch`. Keeps meta-characters (`*?[]`) untouched (they are ASCII).

### 3. `src/cli.cpp:wildcard_path_match` — `?` consumed one byte, not one code-point

*Symptom:* In `.gitignore`-style matching a `?` could split a multi-byte UTF-8 sequence (e.g., `aé` counted as `a`+two bytes).  
*Fix:* Advance `?` by `utf8_char_len` (one code-point) instead of one byte. `*` remains byte-wise because UTF-8 continuation bytes never contain `/`, so its semantics are unchanged.

## Corners Verified — No Bug

| Corner | Result |
|---|---|
| `\p{Any}` vs `\p{any}` | Case-insensitive via `norm` lower-casing — both match. |
| `\p{scx=Han}`, `\p{ScriptExtensions=Han}` | `k_norm == "scx"/"scriptextensions"` maps to `uscript_getCode`; case-insensitive; matches Han/ Han extensions (Common chars with Han extension are considered Han via script extensions; strict script value is Script). |
| `\p{gc = Lu}` with spaces | `trim()` on name, raw_key/unit, raw_val handles leading/trailing and around `=`/`:` plus `_`/`-`/space-insensitive normalization. |
| `\p{Invalid}` | Throws `unknown Unicode property: Invalid` (exit 2 via CLI). |
| `\p{sc = Greek}` with spaces | Same trim path — works. |
| `--type-add 'foo:*.foo'` / brace expansion | `expand_braces` recursive handles `*.{foo,txt}`; `include:` prefix handled. |
| `--hidden` | `allowed_path` skips dotfiles unless `--hidden`; explicit file path bypasses filter (ripgrep parity). |
| `--one-file-system` on Windows junction | `same_device` resolves volume via `GetFullPathNameW` handling `\\?\UNC\`, `\\?\C:\`, `C:\`, and `\\server\share\`. Junction on same volume stays same device; mount point on different volume filtered. Verified via `mklink /J` manual test. |
| `?` with multi-byte in `--glob` | `platform::fnmatch` already used `utf8_char_len`; verified `a?` matches both `ab` and `aé`. |
| `--glob` with `!` negation | `allowed_path` splits `!` prefix; `!*.log` excludes logs; `*` + `!` combination works; same for `--iglob`. |

## Known Limitations Documented (Not Fixed)

### C API `pg_index_options::include_hidden`

`pergrep::IndexOptions` has `bool include_hidden = true`, but the C binding struct `pg_index_options` exposes only `{chunk_bytes, chunk_overlap, positional_block_bytes, positional_budget_ratio, planned_qgrams, follow_symlinks}` — `include_hidden` is not exposed.

*Decision:* Documented as known limitation; **no ABI break** in this audit. The C default `pg_index_options_default()` mirrors `IndexOptions{}` with `include_hidden=true` implicitly; callers cannot toggle it via C yet. Future fix should either:
  - Add `int include_hidden` as a new trailing field with size-aware `convert()` (check `sizeof`/`offsetof`), or
  - Bump to `pg_index_options_v2` / `pg_index_options_ex` and keep `pg_index_build` backward-compatible via `memcpy` + zero-default.

The C default remains filtering-agnostic: hidden filtering is normally a CLI concern, not an index build concern.

## Regression Tests Added

* `tests/test.cpp` — new block "BF-1 audit corners": `\p{sc = Greek}` with spaces, `\p{Any}`/`\p{any}` case, `\p{Invalid}` throws, and `\p{gc = Lu}` spaced form all compile/match as specified.
* `tests/cli_compat.sh` — new section "BF-1 audit: iglob Unicode and double-star": creates `café.txt`/`Café.txt` and asserts `--iglob '*café*'`/`'*CAFÉ*'` both match; creates `a/b/c/file.txt` tree and asserts `**/*.txt` and `a/**/*.txt` double-star semantics.

## Verification

```
cmake --preset windows-clang
cmake --build --preset windows-clang
ctest --preset windows-clang -V   # 5/5
```

## References

* `src/platform.hpp:fnmatch` — double-star docs
* `src/cli.cpp:globmatch` — ICU folding comment
* `src/cli.cpp:wildcard_path_match` — `?` UTF-8 comment
* `include/pergrep/pergrep_c.h` — `include_hidden` limitation (see comment in `src/c_api.cpp:convert`)
