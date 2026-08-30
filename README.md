# pergrep / libpergrep

`pergrep` is an indexed text searcher. It compiles both the corpus and each query into conservative intermediate representations, narrows each search to positional candidate blocks, and performs exact verification only where necessary.

The distribution contains two surfaces:

- **`libpergrep`** — a small C++20 library, with both C++ and stable opaque-handle C APIs.
- **`pergrep`** — a command-line frontend targeting the **ripgrep 15.2.0 command-line interface** while using `libpergrep` as its only search engine.

No search path shells out to `grep`, `rg`, `ugrep`, Hyperscan, PCRE2 matching APIs, or another grep-like matcher. `--pre` may execute the transformer explicitly supplied by the user; `-z/--search-zip` uses libarchive only for decompression.

## Requirements

Linux/Unix build requirements:

- CMake 3.20+
- a C++20 compiler
- ICU (`uc`) for Unicode character properties/case folding
- libarchive for the `pergrep` CLI compressed-input path
- iconv (normally provided by libc on Linux) for non-UTF-8 input conversion

Windows build requirements:

- CMake 3.20+ with the Ninja generator
- Visual Studio 2022 (MSVC) or clang-cl
- [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set, providing
  the `icu` and `libarchive` ports

## Build and test

Linux/Unix:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Windows (MSVC via vcpkg):

```powershell
cmake --preset windows-vcpkg
cmake --build --preset windows-vcpkg
ctest --preset windows-vcpkg
```

Windows (clang-cl via vcpkg):

```powershell
cmake --preset windows-clang
cmake --build --preset windows-clang
ctest --preset windows-clang
```

The Windows port is implemented in `src/platform.hpp`: fnmatch, iconv, tty
detection, `--pre` subprocess execution, and file timestamps all map to Win32
calls, and UTF-8 is used end-to-end for paths (wmain converts argv). The
shell-based test suites (`cli_compat.sh`, `flags_surface.sh`,
`upstream_cases.sh`) run under Git for Windows' bash when it is on `PATH`.

Install to a prefix:

```bash
cmake --install build --prefix "$HOME/.local"
```

The installation exports `pergrep::libpergrep` for CMake consumers:

```cmake
find_package(pergrep CONFIG REQUIRED)
target_link_libraries(my_program PRIVATE pergrep::libpergrep)
```

## CLI

Typical commands are intentionally ripgrep-shaped:

```bash
pergrep 'TODO|FIXME' src
pergrep -F -n needle .
pergrep -S -g '*.cpp' -g '*.hpp' Pattern .
pergrep -P '(ab)\1' .
pergrep --json 'foo[0-9]+' .
pergrep -z needle archive.tar.gz
pergrep --pre 'sed -n 1,200p' needle file.txt
```

The compatibility frontend implements the ripgrep 15.2.0 public flag surface, including short/long aliases and negated forms; context/output-mode precedence; stdin and pattern-file input; ignore sources and VCS filtering; type definitions; Unicode-aware search; CRLF/NUL records; binary modes; replacement/captures; JSON/stats; colors; hyperlinks; sorting; compressed input; preprocessing; and generated completion/man output. See [`COMPATIBILITY.md`](COMPATIBILITY.md) for the exact compatibility contract and tests.

Persistent indexes are stored under `$PERGREP_CACHE_DIR`, `$XDG_CACHE_HOME/pergrep`, or `~/.cache/pergrep`. Filesystem fingerprints invalidate stale indexes. Inputs transformed by encoding conversion, `--pre`, or archive decompression are indexed ephemerally after transformation.

> **Scope note:** the on-disk index persists the q-gram/positional filter structures and file metadata; by default (`IndexOptions::persist_corpus=false`, v5) `Index::load()` still re-reads each source file into `I->loaded` so the corpus remains memory-resident during search (filter rebuild is saved, not a Lucene-style fully indexed store) — this is `O(corpus)` I/O per load, documented as the prototype cost. As a first step toward a true on-disk corpus (QO-5), `persist_corpus=true` emits v6 which also persists `loaded` sizes+data after the `pos` vector via `puts`/`gets` so `load` restores `content()`/`search` without re-reading files. `Index::fresh()` re-traverses the tree and compares path/size/mtime per file — cheap on stable trees (`O(files)` with `error_code` + `lexically_relative`, no `weakly_canonical` per file), proportional to churn on high-churn trees. The in-tree benchmark consumes the versioned deterministic workload matrix and compares indexed results with a brute-force reference within pergrep; it is not a rigorous `rg`/`ugrep` cross-tool benchmark.
The deterministic M0.1 workload contract lives in [`docs/workload-matrix.md`](docs/workload-matrix.md) and `bench/workload_matrix.hpp`. It defines four closed product classes (one-shot, warm-repeated, interactive large-repository, and batch multi-pattern), explicit cold/warm/repeated/filtered-scope/transformed-input scenarios, and the lifecycle, throughput, pruning, and correctness dimensions emitted by `pergrep_bench`. The corpus is generated in memory from fixed seeds; CI never downloads an external corpus. Later benchmark and release-gate work must consume matrix version 1 and may add cases only without changing class semantics.


## Search architecture

The default index uses:

1. 32 KiB logical chunks with a small overlap.
2. A query-major/transposed hashed 4-gram chunk filter.
3. Adaptive 256-byte positional Bloom descriptors.
4. Low-frequency q-gram query planning.
5. A query-compiled exact verifier using positional blocks and rare-byte/literal anchors.
6. An internal ordered Thompson/Pike NFA for regular expressions.
7. A separate internal extended VM for `-P` backreferences/lookaround.

All index filters are conservative: they may admit false-positive candidate regions, but exact acceptance is performed by the verifier. A corpus-side filter is never permitted to create a false negative.

## C++ API

```cpp
#include <pergrep/pergrep.hpp>
#include <iostream>

int main() {
    auto index = pergrep::Index::build("repo");
    pergrep::Searcher search(index);

    auto pattern = pergrep::Pattern::compile("needle", {
        .kind = pergrep::PatternKind::Fixed,
        .case_mode = pergrep::CaseMode::Sensitive,
    });

    for (const auto& m : search.find(pattern)) {
        std::cout << index.files()[m.file_id].path
                  << ':' << m.start << '-' << m.end << '\n';
    }
}
```

For in-memory/transformed content, use `Index::from_documents`. `Match` uses byte offsets and regex matches may carry capture spans. `SearchStats` exposes candidate-chunk/block and verified-byte counters without mixing CLI formatting into the library.

## C API

```c
#include <pergrep/pergrep_c.h>
#include <stdio.h>

int main(void) {
    char *err = NULL;
    pg_index_options io = pg_index_options_default();
    pg_index *idx = pg_index_build("repo", &io, &err);
    if (!idx) { fprintf(stderr, "%s\n", err); pg_error_free(err); return 1; }

    pg_pattern_options po = pg_pattern_options_default();
    po.kind = PG_FIXED;
    pg_pattern *pat = pg_pattern_compile("needle", &po, &err);
    pg_searcher *s = pg_searcher_new(idx, &err);

    size_t n = 0;
    pg_match *ms = pg_search(s, pat, NULL, &n, NULL, &err);
    for (size_t i = 0; i < n; ++i)
        printf("%s:%llu-%llu\n", pg_index_file_path(idx, ms[i].file_id),
               (unsigned long long)ms[i].start, (unsigned long long)ms[i].end);

    pg_matches_free(ms);
    pg_searcher_free(s);
    pg_pattern_free(pat);
    pg_index_free(idx);
    return 0;
}
```

## Regex engines

The default regex engine is internal and executes regular patterns with an ordered Thompson/Pike NFA, avoiding catastrophic exponential backtracking for regular expressions. It supports Unicode properties/classes and case folding through ICU, captures, named captures, scoped inline flags, greedy/lazy repetition, anchors, word/line boundaries, multiline and dotall operation.

`-P/--pcre2` selects pergrep's **internal extended compatibility VM** for constructs such as backreferences and lookaround. The name is retained for ripgrep CLI compatibility; no PCRE2 matcher is linked or invoked. `--pcre2-version` reports that fact explicitly.

## Testing

The CMake test set contains five release suites:

- C++ library/NFA/index tests.
- C ABI tests.
- CLI semantic/precedence/integration tests.
- ripgrep 15.2.0 flag-surface parser tests.
- behavior cases adapted from ripgrep's public integration tests.

Release packaging additionally performs a clean-source build, install, external C++/C consumer builds, installed-CLI smoke tests, and a dependency/source audit.

## Third-party material

`src/default_types.hpp` contains a file-type definition table adapted from ripgrep 15.2.0. See [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for attribution and license text.
