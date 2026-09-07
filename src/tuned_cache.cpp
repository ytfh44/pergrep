#include "pergrep/tuned_cache.hpp"
#include <sstream>
#include <iomanip>
#include <stdexcept>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

namespace pergrep::cache {

const char* to_string(InvalidationReason reason) noexcept {
    switch (reason) {
        case InvalidationReason::None: return "compatible";
        case InvalidationReason::SchemaMismatch: return "schema_mismatch";
        case InvalidationReason::EngineVersionMismatch: return "engine_version_mismatch";
        case InvalidationReason::SourceFingerprintMismatch: return "source_fingerprint_mismatch";
        case InvalidationReason::SelectorScopeMismatch: return "selector_scope_mismatch";
        case InvalidationReason::TransformMismatch: return "transform_mismatch";
        case InvalidationReason::HardwareIncompatible: return "hardware_incompatible";
        case InvalidationReason::ToolchainMismatch: return "toolchain_mismatch";
        case InvalidationReason::OptionsMismatch: return "options_mismatch";
        case InvalidationReason::PolicyMismatch: return "policy_mismatch";
    }
    return "unknown";
}

namespace {

inline std::uint64_t fnv1a_mix(std::uint64_t hash, std::string_view data) noexcept {
    for (unsigned char b : data) {
        hash ^= static_cast<std::uint64_t>(b);
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline std::uint64_t fnv1a_mix_u64(std::uint64_t hash, std::uint64_t val) noexcept {
    for (int i = 0; i < 8; ++i) {
        hash ^= (val & 0xFF);
        hash *= 1099511628211ULL;
        val >>= 8;
    }
    return hash;
}

} // namespace

std::uint64_t TunedCacheIdentity::compute_cache_key() const noexcept {
    std::uint64_t h = 14695981039346656037ULL;
    h = fnv1a_mix(h, schema_version);
    h = fnv1a_mix(h, engine_version);
    h = fnv1a_mix_u64(h, source_fingerprint);
    h = fnv1a_mix(h, source_root);
    h = fnv1a_mix(h, selector_scope);
    h = fnv1a_mix_u64(h, transform_identity);
    h = fnv1a_mix(h, toolchain);
    h = fnv1a_mix_u64(h, required_features);
    h = fnv1a_mix_u64(h, index_options.chunk_bytes);
    h = fnv1a_mix_u64(h, index_options.chunk_overlap);
    h = fnv1a_mix_u64(h, index_options.positional_block_bytes);
    h = fnv1a_mix_u64(h, static_cast<std::uint64_t>(index_options.positional_budget_ratio * 1000000.0));
    h = fnv1a_mix_u64(h, index_options.planned_qgrams);
    h = fnv1a_mix(h, operator_policy);
    h = fnv1a_mix_u64(h, corpus_bytes);
    h = fnv1a_mix_u64(h, corpus_files);
    return h;
}

InvalidationReason TunedCacheIdentity::check_compatibility(
    const TunedCacheIdentity& runtime_env,
    std::uint32_t available_features) const noexcept {

    if (schema_version != runtime_env.schema_version) {
        return InvalidationReason::SchemaMismatch;
    }
    if (engine_version != runtime_env.engine_version) {
        return InvalidationReason::EngineVersionMismatch;
    }
    if (source_fingerprint != 0 && runtime_env.source_fingerprint != 0 &&
        source_fingerprint != runtime_env.source_fingerprint) {
        return InvalidationReason::SourceFingerprintMismatch;
    }
    if (selector_scope != runtime_env.selector_scope) {
        return InvalidationReason::SelectorScopeMismatch;
    }
    if (transform_identity != runtime_env.transform_identity) {
        return InvalidationReason::TransformMismatch;
    }
    if ((required_features & ~available_features) != 0) {
        return InvalidationReason::HardwareIncompatible;
    }
    if (!toolchain.empty() && !runtime_env.toolchain.empty() &&
        toolchain != runtime_env.toolchain) {
        return InvalidationReason::ToolchainMismatch;
    }
    if (index_options.chunk_bytes != runtime_env.index_options.chunk_bytes ||
        index_options.chunk_overlap != runtime_env.index_options.chunk_overlap ||
        index_options.positional_block_bytes != runtime_env.index_options.positional_block_bytes ||
        index_options.planned_qgrams != runtime_env.index_options.planned_qgrams) {
        return InvalidationReason::OptionsMismatch;
    }
    if (operator_policy != runtime_env.operator_policy &&
        operator_policy != "default" && runtime_env.operator_policy != "default") {
        return InvalidationReason::PolicyMismatch;
    }
    return InvalidationReason::None;
}

std::string TunedCacheIdentity::serialize() const {
    std::ostringstream ss;
    ss << "schema_version=" << schema_version << "\n";
    ss << "engine_version=" << engine_version << "\n";
    ss << "source_fingerprint=" << source_fingerprint << "\n";
    ss << "source_root=" << source_root << "\n";
    ss << "selector_scope=" << selector_scope << "\n";
    ss << "transform_identity=" << transform_identity << "\n";
    ss << "toolchain=" << toolchain << "\n";
    ss << "required_features=" << required_features << "\n";
    ss << "chunk_bytes=" << index_options.chunk_bytes << "\n";
    ss << "chunk_overlap=" << index_options.chunk_overlap << "\n";
    ss << "positional_block_bytes=" << index_options.positional_block_bytes << "\n";
    ss << "positional_budget_ratio=" << index_options.positional_budget_ratio << "\n";
    ss << "planned_qgrams=" << index_options.planned_qgrams << "\n";
    ss << "operator_policy=" << operator_policy << "\n";
    ss << "corpus_bytes=" << corpus_bytes << "\n";
    ss << "corpus_files=" << corpus_files << "\n";
    ss << "cache_key=" << compute_cache_key() << "\n";
    return ss.str();
}

TunedCacheIdentity TunedCacheIdentity::deserialize(std::string_view text) {
    TunedCacheIdentity id;
    std::string line;
    std::istringstream stream{std::string(text)};

    while (std::getline(stream, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;

        std::string key = line.substr(0, pos);
        std::string val = line.substr(pos + 1);

        if (key == "schema_version") id.schema_version = val;
        else if (key == "engine_version") id.engine_version = val;
        else if (key == "source_fingerprint") id.source_fingerprint = std::stoull(val);
        else if (key == "source_root") id.source_root = val;
        else if (key == "selector_scope") id.selector_scope = val;
        else if (key == "transform_identity") id.transform_identity = std::stoull(val);
        else if (key == "toolchain") id.toolchain = val;
        else if (key == "required_features") id.required_features = static_cast<std::uint32_t>(std::stoul(val));
        else if (key == "chunk_bytes") id.index_options.chunk_bytes = std::stoull(val);
        else if (key == "chunk_overlap") id.index_options.chunk_overlap = std::stoull(val);
        else if (key == "positional_block_bytes") id.index_options.positional_block_bytes = std::stoull(val);
        else if (key == "positional_budget_ratio") id.index_options.positional_budget_ratio = std::stod(val);
        else if (key == "planned_qgrams") id.index_options.planned_qgrams = std::stoull(val);
        else if (key == "operator_policy") id.operator_policy = val;
        else if (key == "corpus_bytes") id.corpus_bytes = std::stoull(val);
        else if (key == "corpus_files") id.corpus_files = std::stoull(val);
    }
    return id;
}

std::uint32_t detect_runtime_features() noexcept {
    std::uint32_t flags = FeaturePositionalEncoding | FeatureSparsePostings | FeatureDenseBitmap | FeatureAhoCorasick;

#if defined(__aarch64__) || defined(_M_ARM64)
    flags |= FeatureNeon;
#elif defined(__x86_64__) || defined(_M_X64)
    flags |= FeatureSse42 | FeaturePopcnt;

#if defined(_MSC_VER)
    int cpu_info[4];
    __cpuid(cpu_info, 0);
    int n_ids = cpu_info[0];
    if (n_ids >= 7) {
        __cpuidex(cpu_info, 7, 0);
        if (cpu_info[1] & (1 << 5)) { // AVX2
            flags |= FeatureAvx2;
        }
        if (cpu_info[1] & (1 << 3)) { // BMI2
            flags |= FeatureBmi2;
        }
    }
#elif defined(__x86_64__)
    unsigned int eax, ebx, ecx, edx;
    if (__get_cpuid_max(0, nullptr) >= 7) {
        __cpuid_count(7, 0, eax, ebx, ecx, edx);
        if (ebx & (1 << 5)) {
            flags |= FeatureAvx2;
        }
        if (ebx & (1 << 3)) {
            flags |= FeatureBmi2;
        }
    }
#endif
#endif

    return flags;
}

std::string explain_selection(const TunedCacheIdentity& candidate,
                              const TunedCacheIdentity& target_env,
                              std::uint32_t available_features) {
    std::ostringstream ss;
    InvalidationReason r = candidate.check_compatibility(target_env, available_features);

    ss << "=== Tuned Cache Identity Compatibility Report ===\n";
    ss << "Candidate Cache Key: 0x" << std::hex << candidate.compute_cache_key() << std::dec << "\n";
    ss << "Target Cache Key:    0x" << std::hex << target_env.compute_cache_key() << std::dec << "\n";
    ss << "Status: " << (r == InvalidationReason::None ? "COMPATIBLE (Cache Hit)" : "INCOMPATIBLE (Cache Miss / Invalidation)") << "\n";
    ss << "Reason Code: " << to_string(r) << "\n\n";

    ss << "Field Diagnostics:\n";
    ss << "  Schema Version:     " << candidate.schema_version << " vs " << target_env.schema_version
       << (candidate.schema_version == target_env.schema_version ? " [OK]" : " [MISMATCH]") << "\n";
    ss << "  Engine Version:     " << candidate.engine_version << " vs " << target_env.engine_version
       << (candidate.engine_version == target_env.engine_version ? " [OK]" : " [MISMATCH]") << "\n";
    ss << "  Source Fingerprint: " << candidate.source_fingerprint << " vs " << target_env.source_fingerprint
       << (candidate.source_fingerprint == target_env.source_fingerprint ? " [OK]" : " [MISMATCH]") << "\n";
    ss << "  Selector Scope:     " << candidate.selector_scope << " vs " << target_env.selector_scope
       << (candidate.selector_scope == target_env.selector_scope ? " [OK]" : " [MISMATCH]") << "\n";
    ss << "  Transform Identity: " << candidate.transform_identity << " vs " << target_env.transform_identity
       << (candidate.transform_identity == target_env.transform_identity ? " [OK]" : " [MISMATCH]") << "\n";
    ss << "  Toolchain:          " << candidate.toolchain << " vs " << target_env.toolchain
       << (candidate.toolchain == target_env.toolchain ? " [OK]" : " [MISMATCH]") << "\n";
    ss << "  Operator Policy:    " << candidate.operator_policy << " vs " << target_env.operator_policy
       << (candidate.operator_policy == target_env.operator_policy ? " [OK]" : " [MISMATCH]") << "\n";

    std::uint32_t missing = (candidate.required_features & ~available_features);
    ss << "  Required Features:  0x" << std::hex << candidate.required_features
       << ", Available: 0x" << available_features
       << (missing == 0 ? " [OK]" : " [MISSING REQUIRED FEATURES]") << std::dec << "\n";

    return ss.str();
}

} // namespace pergrep::cache
