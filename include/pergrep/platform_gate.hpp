#pragma once

#include "pergrep/autotune.hpp"
#include "pergrep/pergrep.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pergrep::platform_gate {

// Target platform environment classification (M9.5)
enum class PlatformTier {
    Tier1Supported,      // Linux (GCC/Clang), Windows MSVC, Windows Clang-cl (CI gated)
    Tier2Experimental,   // Linux ARM64, Windows ARM64 (CI verified smoke)
    Unsupported          // Untested macOS, network/remote filesystems, removable media
};

const char* to_string(PlatformTier tier) noexcept;

// Documented tolerances across platforms for autotuned candidate metrics
struct PlatformTolerances {
    double relative_timing_tolerance = 0.15; // 15% latency/timing variance allowed across platforms
    double score_relative_tolerance = 0.10;  // 10% objective score variance allowed across platforms
    std::size_t thread_count_min = 1;        // Supported thread bounds
    std::size_t thread_count_max = 64;
    bool require_exact_config_order = true;  // Candidate exploration order must be 100% deterministic
    bool require_case_sensitive_sort = true; // File enumeration must sort deterministically
};

// Platform environment inspection
struct EnvironmentInfo {
    std::string os_name;
    std::string compiler_name;
    std::string architecture;
    PlatformTier tier = PlatformTier::Tier1Supported;
    bool is_network_filesystem = false;
    bool is_removable_media = false;
    bool is_untested_macos = false;
    std::size_t hardware_concurrency = 1;
};

EnvironmentInfo current_environment_info() noexcept;

// Comparison and reproducibility audit report between two platform runs
struct PlatformComparisonReport {
    bool passed = true;
    std::string platform_a;
    std::string platform_b;
    std::size_t evaluated_count_a = 0;
    std::size_t evaluated_count_b = 0;
    bool config_sequence_matched = true;
    bool best_config_matched = true;
    double best_score_delta_pct = 0.0;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    std::string detailed_log;
};

// Gate evaluation comparing two autotune runs across platforms or within platform
PlatformComparisonReport evaluate_platform_reproducibility(
    const autotune::SearchResult& run_a,
    const autotune::SearchResult& run_b,
    const PlatformTolerances& tolerances = PlatformTolerances{});

// Validates that an environment does not assume unsupported or unsafe runtime conditions
bool validate_environment_safety(const EnvironmentInfo& env, std::string* error_msg = nullptr);

} // namespace pergrep::platform_gate
