#include "pergrep/platform_gate.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <thread>

namespace pergrep::platform_gate {

const char* to_string(PlatformTier tier) noexcept {
    switch (tier) {
        case PlatformTier::Tier1Supported: return "Tier1Supported";
        case PlatformTier::Tier2Experimental: return "Tier2Experimental";
        case PlatformTier::Unsupported: return "Unsupported";
    }
    return "Unknown";
}

EnvironmentInfo current_environment_info() noexcept {
    EnvironmentInfo info;

#if defined(_WIN32)
    info.os_name = "windows";
#elif defined(__linux__)
    info.os_name = "linux";
#elif defined(__APPLE__)
    info.os_name = "macos";
    info.is_untested_macos = true;
    info.tier = PlatformTier::Unsupported;
#else
    info.os_name = "unknown";
    info.tier = PlatformTier::Unsupported;
#endif

#if defined(__clang__)
    info.compiler_name = "clang-" + std::to_string(__clang_major__);
#elif defined(_MSC_VER)
    info.compiler_name = "msvc-" + std::to_string(_MSC_VER);
#elif defined(__GNUC__)
    info.compiler_name = "gcc-" + std::to_string(__GNUC__);
#else
    info.compiler_name = "unknown";
#endif

#if defined(__x86_64__) || defined(_M_X64)
    info.architecture = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    info.architecture = "arm64";
    if (info.tier == PlatformTier::Tier1Supported) {
        info.tier = PlatformTier::Tier2Experimental;
    }
#else
    info.architecture = "unknown";
    info.tier = PlatformTier::Unsupported;
#endif

    unsigned int hw = std::thread::hardware_concurrency();
    info.hardware_concurrency = hw ? hw : 1;

    return info;
}

bool validate_environment_safety(const EnvironmentInfo& env, std::string* error_msg) {
    if (env.is_untested_macos) {
        if (error_msg) *error_msg = "untested macOS platform behavior is disallowed";
        return false;
    }
    if (env.is_network_filesystem) {
        if (error_msg) *error_msg = "untested network / remote filesystems are disallowed";
        return false;
    }
    if (env.is_removable_media) {
        if (error_msg) *error_msg = "untested removable media environments are disallowed";
        return false;
    }
    if (env.tier == PlatformTier::Unsupported) {
        if (error_msg) *error_msg = "unsupported platform environment (" + env.os_name + "/" + env.architecture + ")";
        return false;
    }
    if (env.hardware_concurrency == 0 || env.hardware_concurrency > 1024) {
        if (error_msg) *error_msg = "invalid hardware concurrency: " + std::to_string(env.hardware_concurrency);
        return false;
    }
    return true;
}

PlatformComparisonReport evaluate_platform_reproducibility(
    const autotune::SearchResult& run_a,
    const autotune::SearchResult& run_b,
    const PlatformTolerances& tolerances) {

    PlatformComparisonReport rep;
    rep.platform_a = run_a.toolchain_info;
    rep.platform_b = run_b.toolchain_info;
    rep.evaluated_count_a = run_a.evaluated.size();
    rep.evaluated_count_b = run_b.evaluated.size();

    std::ostringstream ss;
    ss << "=== Platform Reproducibility Gate Evaluation ===\n";
    ss << "Platform A: " << rep.platform_a << " (" << rep.evaluated_count_a << " evaluated)\n";
    ss << "Platform B: " << rep.platform_b << " (" << rep.evaluated_count_b << " evaluated)\n";

    // 1. Candidate evaluation sequence check
    if (tolerances.require_exact_config_order) {
        if (run_a.evaluated.size() != run_b.evaluated.size()) {
            rep.config_sequence_matched = false;
            rep.errors.push_back("Evaluated candidate count mismatch: " +
                                 std::to_string(run_a.evaluated.size()) + " vs " +
                                 std::to_string(run_b.evaluated.size()));
        } else {
            for (std::size_t i = 0; i < run_a.evaluated.size(); ++i) {
                const auto& oa = run_a.evaluated[i].options;
                const auto& ob = run_b.evaluated[i].options;
                if (oa.chunk_bytes != ob.chunk_bytes ||
                    oa.chunk_overlap != ob.chunk_overlap ||
                    oa.positional_block_bytes != ob.positional_block_bytes ||
                    oa.planned_qgrams != ob.planned_qgrams) {
                    rep.config_sequence_matched = false;
                    rep.errors.push_back("Candidate sequence diverged at index " + std::to_string(i));
                    break;
                }
            }
        }
    }

    // 2. Best candidate configuration check
    if (run_a.best.has_value() && run_b.best.has_value()) {
        const auto& ba = run_a.best->options;
        const auto& bb = run_b.best->options;
        if (ba.chunk_bytes != bb.chunk_bytes ||
            ba.chunk_overlap != bb.chunk_overlap ||
            ba.positional_block_bytes != bb.positional_block_bytes ||
            ba.planned_qgrams != bb.planned_qgrams) {
            rep.best_config_matched = false;
            rep.warnings.push_back("Platform decision chose different best layout (hardware/compiler trade-off)");
        }

        double score_a = run_a.best->score;
        double score_b = run_b.best->score;
        double base_score = std::max(std::min(score_a, score_b), 1e-6);
        rep.best_score_delta_pct = std::abs(score_a - score_b) / base_score;

        if (rep.best_score_delta_pct > tolerances.score_relative_tolerance) {
            rep.warnings.push_back("Score relative delta exceeds tolerance (" +
                                  std::to_string(rep.best_score_delta_pct * 100.0) + "% > " +
                                  std::to_string(tolerances.score_relative_tolerance * 100.0) + "%)");
        }
    } else if (run_a.best.has_value() != run_b.best.has_value()) {
        rep.errors.push_back("One platform failed to produce a valid best configuration");
        rep.best_config_matched = false;
    }

    rep.passed = rep.errors.empty();

    ss << "Sequence Matched: " << (rep.config_sequence_matched ? "YES" : "NO") << "\n";
    ss << "Best Config Matched: " << (rep.best_config_matched ? "YES" : "NO") << "\n";
    ss << "Best Score Delta: " << std::fixed << std::setprecision(2) << (rep.best_score_delta_pct * 100.0) << "%\n";
    ss << "Gate Result: " << (rep.passed ? "PASS" : "FAIL") << "\n";

    if (!rep.warnings.empty()) {
        ss << "Warnings:\n";
        for (const auto& w : rep.warnings) ss << "  - " << w << "\n";
    }
    if (!rep.errors.empty()) {
        ss << "Errors:\n";
        for (const auto& e : rep.errors) ss << "  - " << e << "\n";
    }

    rep.detailed_log = ss.str();
    return rep;
}

} // namespace pergrep::platform_gate
