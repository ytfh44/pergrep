# Threading test matrix (M5.8)

**Status:** Accepted (capstone; matrix runs on every PR)
**Scope:** M5.8 / GitHub issue #62

## Matrix

Every pull request runs the full `ctest` suite — including all M5.1–M5.8
threading blocks and the M5.7 `-j` shell cases — on three toolchains:

| Platform | CI job | CMake preset |
|---|---|---|
| Linux x86-64 | Linux (unix preset) | unix |
| Windows x86-64 MSVC | Windows MSVC (windows-vcpkg) | windows-vcpkg |
| Windows x86-64 clang-cl | Windows clang-cl (windows-clang) | windows-clang |

`Bench bounds check` additionally gates single-thread performance so `-j`
wiring cannot regress the serial default (M0.7).

## Dimensions (behavioral, all platforms)

- Small selective queries (rare needle, 2 matches in 48 files).
- Large scans (common pattern, 4000+ matches incl. a bulky file).
- Mapped (mmap-backed `Index::build`) vs resident (`Index::from_documents`)
  providers, serial and parallel, including cross-provider agreement.
- Uneven file sizes (M5.2), chunk-boundary crossings (M5.3), budgets and
  cancellation (M5.4), output modes and stats (M5.5), shared-searcher stress
  (M5.6), CLI flag parity (M5.7).
- Resource exhaustion: threads (64) far exceeding files (3).
- Scalar fallback: `threads` 0/1, single-file corpora (parallel branch
  requires 2+ files), empty corpora.

Timing/speedup assertions are excluded by design: wall-clock is
nondeterministic (see M5.7 JSON summary note). Scaling is covered as
correctness scaling — identical results across sizes and counts — not as
timing thresholds.

## Artifacts

Reproducible pass/fail artifacts are the per-PR GitHub Actions check runs
(one per matrix row above) plus the `Bench bounds check` gate. Each run pins
the exact commit, toolchain, preset, and full `ctest --output-on-failure`
log.

## Explicit exclusions (untested)

- macOS (any arch) and ARM (any OS): no runners; behavior there is unclaimed.
- Network filesystems (NFS/SMB) and removable media: freshness/mmap semantics
  differ; untested, use at own risk.
- 32-bit targets and non-UTF-8 locales beyond the covered cases.

Platform maintainers add environments by extending the CI matrix; the serial
oracle stays the acceptance baseline and must not be weakened.

## Evidence anchors

- [`.github/workflows/ci.yml`](../.github/workflows/ci.yml): the matrix.
- [`tests/test.cpp`](../tests/test.cpp): `pergrep_m58_thread_matrix` (+ all
  M5.1–M5.6 blocks); [`tests/cli_compat.sh`](../tests/cli_compat.sh): `-j` cases.