# SIMD bitmap intersection with scalar fallback (M8.4)

**Status:** Accepted
**Scope:** M8.4 / GitHub issue #81
**Owner:** M8; M1 owns statistics semantics; platform maintainers own capability detection

## Purpose

Accelerate q-gram bitmap intersection in `group_candidates` using runtime-dispatched SIMD
kernels (AVX2, AVX-512, ARM NEON) while providing an unrolled scalar fallback on unsupported
hardware. Verify bit-exact candidate equivalence (zero false negatives) across all backends.

## Architecture

### 1. Fused intersection & early-exit

In `group_candidates`, intersecting query hash rows against candidate chunks previously required
two sequential passes over the word vector:
1. `c[j] &= p[j]` for all words in the group
2. Scan `c` to check if any bit remains non-zero (`any |= v != 0`)

M8.4 fuses these into a single vectorized pass `detail::bitmap_intersect(c.data(), p, g.words)`:
- Computes `dst[j] &= src[j]` while accumulating bitwise OR into a vector register
- If the accumulator is all-zero after any hash row, the function immediately returns `false`,
  allowing `group_candidates` to early-exit without evaluating remaining hash rows
- Fused single-pass execution halves cache traffic on intermediate bitmap buffers

### 2. Available kernels & vectorization

| Kernel | Target ISA | Vector width | Words / iter | Alignment rule | Tail handling |
|---|---|---|---|---|---|
| **`bitmap_intersect_scalar`** | Baseline C++20 | 64-bit | 4 (unrolled) | Unaligned safe | 1-word scalar cleanup |
| **`bitmap_intersect_avx2`** | x86-64 AVX2 | 256-bit | 4 | `_mm256_loadu_si256` | Scalar cleanup for `words % 4` |
| **`bitmap_intersect_avx512`** | x86-64 AVX-512F | 512-bit | 8 | `_mm512_loadu_si512` | Scalar cleanup for `words % 8` |
| **`bitmap_intersect_neon`** | AArch64 / ARM NEON | 128-bit | 2 | `vld1q_u64` | Scalar cleanup for `words % 2` |

### 3. Runtime feature detection

- **x86-64:** CPUID checks via `__cpuid` / `__cpuidex` on MSVC and `__cpuid_count` on Clang/GCC:
  - Validates `max_leaf >= 7`
  - Validates OSXSAVE and AVX bits in leaf 1 ECX (`bits 27, 28`)
  - Validates OS state enablement via `_xgetbv(0)`:
    - YMM state (bits 1, 2) required for AVX2
    - ZMM and opmask state (bits 1, 2, 5, 6, 7) required for AVX-512
  - Validates leaf 7 EBX bit 5 (AVX2) and bit 16 (AVX-512F)
- **ARM:** Compile-time architecture detection via `__ARM_NEON` / `__aarch64__` / `_M_ARM64`.
- **Cached detection:** CPUID executes once on startup (`get_active_simd_level()`); subsequent queries read the cached `SimdLevel`.
- **Testing override:** `set_simd_override(int)` allows unit tests to force `Scalar`, `Avx2`, `Avx512`, or `Neon` execution regardless of host capabilities.

## Calibration & measured performance

Measured on 64 words (4,096 chunks) across 200,000 iterations (AMD Zen 4, Windows clang-cl):

| Kernel | Time per call | Time per word | Relative speedup |
|---|---|---|---|
| **Scalar** | 14.80 ns | 0.231 ns | 1.00x (baseline) |
| **AVX2** | 8.85 ns | 0.138 ns | **1.67x** |
| **AVX-512** | 9.18 ns | 0.143 ns | **1.61x** |

AVX2 and AVX-512 provide a ~1.65x speedup on chunk-bitmap intersection. AVX2 matches AVX-512 on this workload due to loop overhead and 256-bit unaligned memory load throughput.

## Zero false negatives invariant

All kernels pass the full differential test suite:
- Every kernel produces identical candidate bit vectors across random, sparse, dense, and full inputs.
- Early exit triggers only when all bits are zero; because bitwise AND is monotonic non-increasing, subsequent AND operations could never produce a non-zero bit.
- Candidate set output is 100% equivalent to the reference oracle.

## Telemetry & observability

`SearchStats` exposes `simd_backend` ("scalar", "avx2", "avx512", "neon"), reflecting the active SIMD kernel used during search.

## Evidence anchors

- [`src/simd_bitmap.hpp`](../src/simd_bitmap.hpp): CPUID detection, AVX2/AVX-512/NEON/scalar kernels, unified dispatcher.
- [`src/search.cpp`](../src/search.cpp): Fused `group_candidates` intersection and `SearchStats::simd_backend` reporting.
- [`include/pergrep/pergrep.hpp`](../include/pergrep/pergrep.hpp): `SearchStats::simd_backend`.
- [`tests/test.cpp`](../tests/test.cpp): `test_m84_simd_bitmap_intersection`.
