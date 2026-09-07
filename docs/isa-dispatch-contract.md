# Cross-Platform ISA Dispatch Contract (M8.6)

**Status:** Accepted contract
**Scope:** M8.6 / GitHub issue #83
**Owner:** M8; platform maintainers own build and CI integration

## Purpose

Define the architecture, feature detection, dispatch boundaries, compiler flags, and fallback rules that make CPU-specific optimizations (AVX2, AVX-512, ARM NEON) safe, observable, and reproducible across every supported hardware target and binary distribution.

## Principles

1. **Baseline portability:** The binary must start, execute, and pass all tests on baseline hardware without requiring optional instruction sets (baseline x86-64-v1, ARMv8-A).
2. **Runtime detection:** Optional instruction sets are selected solely through dynamic runtime detection (CPUID on x86, architecture defines on ARM).
3. **Graceful degradation:** Unsupported or disabled hardware features fall back cleanly to the scalar implementation with identical functional behavior.
4. **Observable dispatch:** The selected SIMD kernel is explicitly observable in telemetry (`SearchStats::simd_backend`).
5. **Deterministic testing:** Tests can override the detected ISA level to exercise all kernels on any host.

## Supported Architectures & Feature Matrix

| Architecture | Target Level | Detection Mechanism | Feature Flags | Kernel Implemented |
|---|---|---|---|---|
| **x86-64** | AVX-512F | CPUID leaf 7 EBX bit 16 + XCR0 bits 5,6,7 | `__attribute__((target("avx512f")))` | `bitmap_intersect_avx512` |
| **x86-64** | AVX2 | CPUID leaf 7 EBX bit 5 + XCR0 bits 1,2 | `__attribute__((target("avx2")))` | `bitmap_intersect_avx2` |
| **AArch64 / ARM64** | NEON | Compile-time architecture define | `__ARM_NEON` | `bitmap_intersect_neon` |
| **Universal** | Scalar | Always available fallback | None | `bitmap_intersect_scalar` |

## Detection & Dispatch Architecture

### 1. Feature Detection Protocol

Feature detection executes once on startup and caches the result:

```cpp
enum class SimdLevel : std::uint8_t {
    Scalar = 0,
    Neon = 1,
    Avx2 = 2,
    Avx512 = 3
};
```

On x86-64:
- **Leaf 0:** Check `max_leaf >= 7`.
- **Leaf 1:** Check OSXSAVE (ECX bit 27) and AVX (ECX bit 28).
- **XCR0:** Read extended control register 0 via `_xgetbv(0)`:
  - YMM state (bits 1, 2) must be enabled by the OS before executing AVX/AVX2 instructions.
  - ZMM state (bits 1, 2, 5, 6, 7) must be enabled by the OS before executing AVX-512 instructions.
- **Leaf 7, Subleaf 0:** Check EBX bit 5 (AVX2) and EBX bit 16 (AVX-512F).

On ARM:
- AArch64 mandates NEON as part of the baseline architecture specification.

### 2. Dispatch Boundaries

Dispatch occurs at function-call boundaries:

- **Per-operation dispatch:** `detail::bitmap_intersect` switches on `get_active_simd_level()`.
- **Inlined kernels:** Each kernel is marked `inline` within its compilation unit to allow compiler optimization.
- **Target attributes:** Clang and GCC functions are decorated with `__attribute__((target("...")))` so the compiler emits target-specific instructions only for that function, keeping the rest of the binary portable.

### 3. AVX-512 Frequency Considerations

AVX-512 instructions on certain Intel microarchitectures (e.g., Skylake-X) can trigger core frequency downclocking due to high power draw:

- **Light vs. Heavy:** Bitmap intersections use "light" integer instructions (`_mm512_and_si512`, `_mm512_or_si512`) that do not use the floating-point FMA units responsible for severe downclocking.
- **AMD Zen 4/5:** Implements AVX-512 via double-pumped 256-bit execution units or full 512-bit units without core downclocking penalties.
- **Measured trade-off:** On short buffers (< 16 words), AVX2 performs comparably to AVX-512 due to lower setup overhead. Both kernels are available and AVX2 serves as the stable fallback.

## Compiler & ABI Rules

1. **No global arch flags:** The project must NOT be compiled with global flags like `-mavx2`, `-mavx512f`, or `/arch:AVX2`. Global flags cause the compiler to emit vector instructions in startup code, breaking baseline hardware.
2. **Function-level attributes:** Use `__attribute__((target("avx2")))` and `__attribute__((target("avx512f")))` on Clang/GCC to enable vectorization for specific functions only.
3. **MSVC handling:** MSVC provides intrinsics without target attributes. Guard calls by CPUID detection at runtime to prevent execution on unsupported CPUs.
4. **Memory alignment:** All SIMD loads and stores use unaligned variants (`_mm256_loadu_si256`, `_mm512_loadu_si512`, `vld1q_u64`) to eliminate alignment faults while maintaining full speed on modern hardware.

## Testing & CI Contract

- **Test override:** `detail::set_simd_override(int level)` forces specific kernel execution in tests:
  - `-1`: Auto-detect (normal runtime operation)
  - `0`: Force Scalar
  - `1`: Force NEON
  - `2`: Force AVX2
  - `3`: Force AVX-512
- **Differential testing:** Unit tests run identical inputs through all compiled kernels to ensure bit-exact output equivalence.
- **CI matrix:** The CI matrix tests on:
  - Linux x86-64 (GCC / Clang)
  - Windows x86-64 (MSVC)
  - Windows x86-64 (Clang-cl)

## Evidence Anchors

- [`src/simd_bitmap.hpp`](../src/simd_bitmap.hpp): CPUID detection, kernel implementations, unified dispatcher.
- [`src/search.cpp`](../src/search.cpp): Dispatch integration and `SearchStats::simd_backend` telemetry.
- [`docs/simd-bitmap-intersection.md`](simd-bitmap-intersection.md): M8.4 benchmark and kernel measurements.
- [`tests/test.cpp`](../tests/test.cpp): `test_m84_simd_bitmap_intersection` and `test_m86_isa_dispatch_contract`.
