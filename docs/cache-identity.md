# Tuned Configuration Cache Identity (M9.3)

**Status:** Accepted contract
**Scope:** M9.3 / GitHub issue #87
**Owner:** M9 (Offline autotuning & release gates); M3 owns storage manifest compatibility

## Purpose

Prevent tuned index layouts or plan policies from being reused with incompatible corpora, selector scopes, transforms, schemas, or hardware capabilities.
Every tuned configuration is bound to a deterministic, comprehensive 64-bit cache key and serialized manifest identity.

## Cache Identity Specification

A `TunedCacheIdentity` encapsulates all variables affecting runtime execution and performance:

| Dimension | Field | Invalidation Trigger |
|---|---|---|
| **Schema & Engine** | `schema_version`, `engine_version` | Upgraded index format or engine ABI break |
| **Source Content** | `source_fingerprint`, `source_root` | Any modification, addition, or removal in input documents |
| **Filter Scope** | `selector_scope` | Different glob pattern, file-type filter, or directory filter |
| **Input Transform** | `transform_identity` | Line ending (LF vs CRLF), NUL record, or encoding changes |
| **Toolchain & ABI** | `toolchain` | Different compiler family/version, target architecture, or OS |
| **Hardware Capabilities** | `required_features` | Target CPU lacks required SIMD/ISA features (e.g. AVX2, NEON, BMI2) |
| **Index Options** | `index_options` | Changes to chunk bytes, overlap, positional block size, or budget |
| **Operator Policy** | `operator_policy` | Changes to execution policy ("scalar", "avx2", "default") |

## Invalidation Rules & Reasons

When a cached configuration is checked against candidate runtime environments, `check_compatibility()` evaluates compatibility in strict precedence order:

1. `SchemaMismatch`: Incompatible schema version.
2. `EngineVersionMismatch`: Different engine version.
3. `SourceFingerprintMismatch`: Source document set has changed.
4. `SelectorScopeMismatch`: Active file selector or path filter differs.
5. `TransformMismatch`: Transform pipeline fingerprint differs.
6. `HardwareIncompatible`: Cache requires CPU features not available on current hardware.
7. `ToolchainMismatch`: Cross-architecture or cross-compiler ABI mismatch.
8. `OptionsMismatch`: Chunk size, overlap, or index layout parameters differ.
9. `PolicyMismatch`: Operator execution policy differs.

## Deterministic Serialization & Inspection

- **Key-Value Serialization:** Format consists of deterministic line-oriented `key=value` pairs sorted in canonical dependency order.
- **Cache Key Generation:** Evaluated via a 64-bit non-cryptographic FNV-1a mixing function across all identity fields.
- **Diagnostic Explanation:** `explain_selection()` produces human-readable diagnostic reports detailing exact field comparisons and pinpointing any reasons for cache misses or invalidations.

## Evidence Anchors

- [`include/pergrep/tuned_cache.hpp`](../include/pergrep/tuned_cache.hpp): Public interface (`TunedCacheIdentity`, `InvalidationReason`, `CacheFeatureFlags`, `detect_runtime_features()`, `explain_selection()`).
- [`src/tuned_cache.cpp`](../src/tuned_cache.cpp): Serialization, deserialization, cache key calculation, hardware feature detection, and compatibility validation.
- [`tests/test.cpp`](../tests/test.cpp): `test_m93_tuned_cache_identity()`.
