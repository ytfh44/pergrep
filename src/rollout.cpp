#include "pergrep/rollout.hpp"
#include <iomanip>
#include <sstream>

namespace pergrep::rollout {

const char* to_string(RolloutState state) noexcept {
    switch (state) {
        case RolloutState::DefaultBaseline: return "default_baseline";
        case RolloutState::CanaryExperimental: return "canary_experimental";
        case RolloutState::RolloutActive: return "rollout_active";
        case RolloutState::RollbackEnforced: return "rollback_enforced";
    }
    return "unknown";
}

std::string RolloutPolicyController::generate_release_notes() const {
    std::ostringstream ss;
    ss << "# Release Notes & Operational Policy Runbook\n\n";
    ss << "**Profile ID:** " << profile_id << "\n";
    ss << "**Current State:** `" << to_string(state) << "`\n";
    if (state == RolloutState::RollbackEnforced) {
        ss << "**Rollback Reason:** " << rollback_reason << "\n";
    }
    ss << "\n## Feature Flags Status\n\n";
    ss << "| Feature Component | Enabled | Fallback Strategy |\n";
    ss << "|---|---|---|\n";
    ss << "| SIMD Bitmaps | " << (flags.enable_simd_bitmaps ? "YES" : "NO (Scalar Fallback)") << " | Guarded scalar intersection |\n";
    ss << "| Positional Matrix | " << (flags.enable_positional_encoding ? "YES" : "NO (Document Filter)") << " | Full document verification |\n";
    ss << "| Sparse Postings | " << (flags.enable_sparse_postings ? "YES" : "NO (Dense Bitmaps)") << " | Dense bitset traversal |\n";
    ss << "| Aho-Corasick Multi-Pattern | " << (flags.enable_aho_corasick_prefilter ? "YES" : "NO (Sequential Scan)") << " | Single-pattern loop |\n";
    ss << "| Autotuned Index Layout | " << (flags.enable_autotuned_layout ? "YES" : "NO (Conservative Default)") << " | Fixed 16KB chunk size |\n";
    ss << "| Parallel Build | " << (flags.enable_parallel_build ? "YES" : "NO (Single-Threaded)") << " | Deterministic serial build |\n";
    ss << "| Bounded Lazy DFA | " << (flags.enable_lazy_dfa ? "YES" : "NO (NFA Engine)") << " | PikeVM regex verification |\n\n";

    ss << "## Fallback & Error Telemetry\n\n";
    ss << "- Total Queries Observed: " << telemetry.total_queries() << "\n";
    ss << "- Total Fallbacks Triggered: " << telemetry.total_fallbacks() << "\n";
    ss << "- Fallback Rate: " << std::fixed << std::setprecision(2) << (telemetry.fallback_ratio() * 100.0) << "%\n";
    ss << "- Safety Threshold: 5.00%\n\n";

    ss << "## Rollback Runbook\n\n";
    ss << "To manually revert to the conservative baseline path without data loss:\n";
    ss << "1. Set environment variable `PERGREP_ROLLOUT_PROFILE=default_baseline`.\n";
    ss << "2. Or invoke `controller.enforce_rollback(\"<reason>\")` in application code.\n";
    ss << "3. All cached data remain compatible; query execution transitions immediately to zero-risk reference paths.\n";

    return ss.str();
}

} // namespace pergrep::rollout
