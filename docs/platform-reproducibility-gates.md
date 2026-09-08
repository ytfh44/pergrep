# Platform Reproducibility Gates (M9.5)

**Status:** Accepted contract
**Scope:** M9.5 / GitHub issue #89
**Owner:** M9 (Offline autotuning & release gates); platform maintainers own environment evidence

## Purpose

Distinguish genuine platform-specific performance optimizations from non-reproducible, noisy, or unsafe tuning decisions.
Enforce that candidate configuration selection remains reproducible across Linux (GCC/Clang), Windows MSVC, and Windows clang-cl within declared allowances, and that configurations never rely on untested or hazardous runtime environments.

## Platform Support Tiers

| Tier | Platforms & Toolchains | CI Verification Status | Tuning Cache Policy |
|---|---|---|---|
| **Tier 1 (Supported)** | Linux x86_64 (GCC/Clang), Windows x86_64 (MSVC), Windows x86_64 (clang-cl) | Full CI build, test, and benchmark gating | Fully supported and persisted |
| **Tier 2 (Experimental)** | Linux ARM64, Windows ARM64 | CI build and test execution | Supported with architectural feature isolation |
| **Unsupported** | macOS (untested), network / remote filesystems, removable media | Not qualified | Prohibited from assuming safe caching |

## Documented Tolerances & Invariants

When comparing offline tuning exploration across runs and platforms:

1. **Candidate Exploration Sequence (Zero Tolerance):**
   Given identical corpus and parameter bounds, the sequence of evaluated candidate configurations must match 100% in order, parameters, and skip decisions across all platforms.
2. **Timing & Measurement Tolerance:**
   Up to 15% relative latency variance is permitted across platforms to accommodate scheduler, OS timer, and memory subsystem differences.
3. **Objective Score Tolerance:**
   Up to 10% relative objective score variance is permitted between platforms.
4. **Thread Scaling Bounds:**
   Concurrencies from 1 to 64 worker threads are formally bounded.
5. **Deterministic File Enumeration:**
   All document and directory walks sort in canonical lexicographic order; case-insensitive comparisons are normalized.

## Disallowed Environment Guardrails

`validate_environment_safety()` enforces that tuning configurations do not assume or execute on:
- Untested macOS environments
- Network-attached storage (NFS, SMB, CIFS) due to caching and mtime inconsistency
- Removable media (USB, SD cards) due to write amplification and latency spikes
- Zero or unbounded hardware concurrency

## Evidence Anchors

- [`include/pergrep/platform_gate.hpp`](../include/pergrep/platform_gate.hpp): Public interface (`PlatformTier`, `PlatformTolerances`, `EnvironmentInfo`, `PlatformComparisonReport`, `evaluate_platform_reproducibility()`, `validate_environment_safety()`).
- [`src/platform_gate.cpp`](../src/platform_gate.cpp): Platform probing, tolerance checks, environment validation, and comparison reporting.
- [`tests/test.cpp`](../tests/test.cpp): `test_m95_platform_reproducibility_gates()`.
