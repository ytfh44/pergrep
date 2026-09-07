#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#include <immintrin.h>
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

#if defined(__ARM_NEON) || defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

namespace pergrep {
namespace detail {

enum class SimdLevel : std::uint8_t {
    Scalar = 0,
    Neon = 1,
    Avx2 = 2,
    Avx512 = 3
};

inline const char* simd_level_name(SimdLevel level) noexcept {
    switch (level) {
        case SimdLevel::Avx512: return "avx512";
        case SimdLevel::Avx2:   return "avx2";
        case SimdLevel::Neon:   return "neon";
        case SimdLevel::Scalar:
        default:                return "scalar";
    }
}

// Runtime CPU feature detection via CPUID on x86/x64 and architecture defines on ARM.
inline SimdLevel detect_cpu_simd() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    int info[4] = {0};
#if defined(_MSC_VER)
    __cpuid(info, 0);
#else
    __cpuid(0, info[0], info[1], info[2], info[3]);
#endif
    if (info[0] < 7) return SimdLevel::Scalar;

#if defined(_MSC_VER)
    __cpuid(info, 1);
#else
    __cpuid(1, info[0], info[1], info[2], info[3]);
#endif
    // ECX bit 27: OSXSAVE, bit 28: AVX
    if ((info[2] & (1 << 27)) == 0 || (info[2] & (1 << 28)) == 0) return SimdLevel::Scalar;

    // Read XCR0 (OS support for YMM state: bits 1 and 2)
#if defined(_MSC_VER)
    uint64_t xcr0 = _xgetbv(0);
#else
    uint32_t xcr0_lo = 0, xcr0_hi = 0;
    __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
    uint64_t xcr0 = (static_cast<uint64_t>(xcr0_hi) << 32) | xcr0_lo;
#endif
    if ((xcr0 & 0x6) != 0x6) return SimdLevel::Scalar;

#if defined(_MSC_VER)
    __cpuidex(info, 7, 0);
#else
    __cpuid_count(7, 0, info[0], info[1], info[2], info[3]);
#endif

    // Check AVX-512F: EBX bit 16 and XCR0 bits 5, 6, 7 (opmask, ZMM_Hi256, Hi16_ZMM)
    // Disable AVX-512 on MSVC due to incomplete support in MSVC toolchain / runtime.
#if !defined(_MSC_VER)
    if (((xcr0 & 0xE6) == 0xE6) && (info[1] & (1 << 16)) != 0) {
        return SimdLevel::Avx512;
    }
#endif

    // Check AVX2: EBX bit 5
    if ((info[1] & (1 << 5)) != 0) {
        return SimdLevel::Avx2;
    }

    return SimdLevel::Scalar;
#elif defined(__aarch64__) || defined(_M_ARM64)
    return SimdLevel::Neon;
#else
    return SimdLevel::Scalar;
#endif
}

// Global SIMD override slot (-1 for none/auto, 0..3 for SimdLevel).
inline int& simd_override_slot() noexcept {
    static int override_val = -1;
    return override_val;
}

inline void set_simd_override(int level) noexcept {
    simd_override_slot() = level;
}

inline SimdLevel get_active_simd_level() noexcept {
    int o = simd_override_slot();
    if (o >= 0 && o <= 3) {
        return static_cast<SimdLevel>(o);
    }
    static const SimdLevel detected = detect_cpu_simd();
    return detected;
}

// ---------------------------------------------------------------------------
// Kernel 1: Scalar fallback (always available, unrolled by 4 words = 256 bits).
// ---------------------------------------------------------------------------
inline bool bitmap_intersect_scalar(std::uint64_t* dst, const std::uint64_t* src, std::size_t words) noexcept {
    std::uint64_t any = 0;
    std::size_t i = 0;
    for (; i + 4 <= words; i += 4) {
        std::uint64_t d0 = dst[i + 0] & src[i + 0];
        std::uint64_t d1 = dst[i + 1] & src[i + 1];
        std::uint64_t d2 = dst[i + 2] & src[i + 2];
        std::uint64_t d3 = dst[i + 3] & src[i + 3];
        dst[i + 0] = d0;
        dst[i + 1] = d1;
        dst[i + 2] = d2;
        dst[i + 3] = d3;
        any |= (d0 | d1 | d2 | d3);
    }
    for (; i < words; ++i) {
        std::uint64_t d = dst[i] & src[i];
        dst[i] = d;
        any |= d;
    }
    return any != 0;
}

// ---------------------------------------------------------------------------
// Kernel 2: AVX2 (256-bit: 4 uint64_t words per instruction).
// ---------------------------------------------------------------------------
#if defined(__x86_64__) || defined(_M_X64)
#if defined(__clang__) || defined(__GNUC__)
__attribute__((target("avx2")))
#endif
inline bool bitmap_intersect_avx2(std::uint64_t* dst, const std::uint64_t* src, std::size_t words) noexcept {
    __m256i vany = _mm256_setzero_si256();
    std::size_t i = 0;
    for (; i + 4 <= words; i += 4) {
        __m256i vd = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + i));
        __m256i vs = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
        __m256i vr = _mm256_and_si256(vd, vs);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), vr);
        vany = _mm256_or_si256(vany, vr);
    }
    uint64_t any = 0;
    if (!_mm256_testz_si256(vany, vany)) {
        any = 1;
    }
    for (; i < words; ++i) {
        std::uint64_t d = dst[i] & src[i];
        dst[i] = d;
        any |= d;
    }
    return any != 0;
}

// ---------------------------------------------------------------------------
// Kernel 3: AVX-512 (512-bit: 8 uint64_t words per instruction).
// ---------------------------------------------------------------------------
#if defined(__clang__) || defined(__GNUC__)
__attribute__((target("avx512f")))
#endif
inline bool bitmap_intersect_avx512(std::uint64_t* dst, const std::uint64_t* src, std::size_t words) noexcept {
    __m512i vany = _mm512_setzero_si512();
    std::size_t i = 0;
    for (; i + 8 <= words; i += 8) {
        __m512i vd = _mm512_loadu_si512(reinterpret_cast<const void*>(dst + i));
        __m512i vs = _mm512_loadu_si512(reinterpret_cast<const void*>(src + i));
        __m512i vr = _mm512_and_si512(vd, vs);
        _mm512_storeu_si512(reinterpret_cast<void*>(dst + i), vr);
        vany = _mm512_or_si512(vany, vr);
    }
    uint64_t any = 0;
    if (_mm512_test_epi64_mask(vany, vany) != 0) {
        any = 1;
    }
    for (; i < words; ++i) {
        std::uint64_t d = dst[i] & src[i];
        dst[i] = d;
        any |= d;
    }
    return any != 0;
}
#endif

// ---------------------------------------------------------------------------
// Kernel 4: ARM NEON (128-bit: 2 uint64_t words per instruction).
// ---------------------------------------------------------------------------
#if defined(__ARM_NEON) || defined(__aarch64__) || defined(_M_ARM64)
inline bool bitmap_intersect_neon(std::uint64_t* dst, const std::uint64_t* src, std::size_t words) noexcept {
    uint64x2_t vany = vdupq_n_u64(0);
    std::size_t i = 0;
    for (; i + 2 <= words; i += 2) {
        uint64x2_t vd = vld1q_u64(dst + i);
        uint64x2_t vs = vld1q_u64(src + i);
        uint64x2_t vr = vandq_u64(vd, vs);
        vst1q_u64(dst + i, vr);
        vany = vorrq_u64(vany, vr);
    }
    std::uint64_t any = vgetq_lane_u64(vany, 0) | vgetq_lane_u64(vany, 1);
    for (; i < words; ++i) {
        std::uint64_t d = dst[i] & src[i];
        dst[i] = d;
        any |= d;
    }
    return any != 0;
}
#endif

// ---------------------------------------------------------------------------
// Unified Runtime Dispatcher
// ---------------------------------------------------------------------------
inline bool bitmap_intersect(std::uint64_t* dst, const std::uint64_t* src, std::size_t words) noexcept {
    switch (get_active_simd_level()) {
#if defined(__x86_64__) || defined(_M_X64)
        case SimdLevel::Avx512:
            return bitmap_intersect_avx512(dst, src, words);
        case SimdLevel::Avx2:
            return bitmap_intersect_avx2(dst, src, words);
#endif
#if defined(__ARM_NEON) || defined(__aarch64__) || defined(_M_ARM64)
        case SimdLevel::Neon:
            return bitmap_intersect_neon(dst, src, words);
#endif
        case SimdLevel::Scalar:
        default:
            return bitmap_intersect_scalar(dst, src, words);
    }
}

} // namespace detail
} // namespace pergrep
