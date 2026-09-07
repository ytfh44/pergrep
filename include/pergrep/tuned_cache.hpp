#pragma once

#include "pergrep/pergrep.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pergrep::cache {

// Invalidation reasons when comparing candidate cache identity against runtime environment (M9.3)
enum class InvalidationReason {
    None,                     // Compatible, valid cache hit
    SchemaMismatch,           // Schema version incompatible
    EngineVersionMismatch,    // Engine version mismatch
    SourceFingerprintMismatch,// Source document content or hash changed
    SelectorScopeMismatch,    // Path glob or selector scope changed
    TransformMismatch,        // Line ending, record separator, or text transform changed
    HardwareIncompatible,     // Requires CPU feature not supported by current runtime
    ToolchainMismatch,        // Compiled for different OS/arch/ABI
    OptionsMismatch,          // Chunk, overlap, or index representation options differ
    PolicyMismatch            // Execution or operator policy differs
};

const char* to_string(InvalidationReason reason) noexcept;

// Bitmask flags for CPU/ISA and execution capabilities
enum CacheFeatureFlags : std::uint32_t {
    FeatureNone = 0,
    FeatureAvx2 = 1 << 0,
    FeatureAvx512 = 1 << 1,
    FeatureNeon = 1 << 2,
    FeatureSse42 = 1 << 3,
    FeatureBmi2 = 1 << 4,
    FeaturePopcnt = 1 << 5,
    FeaturePositionalEncoding = 1 << 6,
    FeatureSparsePostings = 1 << 7,
    FeatureDenseBitmap = 1 << 8,
    FeatureAhoCorasick = 1 << 9
};

// Represents complete identity and binding of a tuned index configuration (M9.3)
struct TunedCacheIdentity {
    std::string schema_version = "pergrep-tuned-cache-v1";
    std::string engine_version = "0.1.0";
    std::uint64_t source_fingerprint = 0;       // Hash of source documents / tree
    std::string source_root;                   // Canonical root path (if filesystem-backed)
    std::string selector_scope = "*";          // Active selector or glob pattern
    std::uint64_t transform_identity = 0;      // Transform fingerprint (CRLF, NUL, encoding)
    std::string toolchain;                     // Compiler, target architecture, OS
    std::uint32_t required_features = 0;       // Required CacheFeatureFlags
    IndexOptions index_options{};              // Chunk, overlap, block, budget options
    std::string operator_policy = "default";   // "default", "scalar", "avx2", etc.
    std::uint64_t corpus_bytes = 0;
    std::uint64_t corpus_files = 0;

    // Computes a deterministic 64-bit cache key binding all behavior-affecting fields
    std::uint64_t compute_cache_key() const noexcept;

    // Checks compatibility with a target runtime environment.
    // Returns InvalidationReason::None if compatible, or the first invalidating reason.
    InvalidationReason check_compatibility(const TunedCacheIdentity& runtime_env,
                                           std::uint32_t available_features) const noexcept;

    // Serializes this identity to a deterministic key-value string representation
    std::string serialize() const;

    // Deserializes identity from string representation. Throws std::runtime_error on parse failure.
    static TunedCacheIdentity deserialize(std::string_view text);
};

// Returns current runtime hardware feature flags based on CPU detection
std::uint32_t detect_runtime_features() noexcept;

// Explains why a candidate cache identity was selected or rejected against the target environment
std::string explain_selection(const TunedCacheIdentity& candidate,
                              const TunedCacheIdentity& target_env,
                              std::uint32_t available_features);

} // namespace pergrep::cache
