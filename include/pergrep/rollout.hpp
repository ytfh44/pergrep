#pragma once

#include "pergrep/pergrep.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

namespace pergrep::rollout {

// Deployment lifecycle state for tuned configurations and experimental operators (M9.7)
enum class RolloutState {
    DefaultBaseline,     // Fully qualified conservative default
    CanaryExperimental,  // Opt-in / experimental evaluation
    RolloutActive,       // Actively promoted candidate
    RollbackEnforced     // Reverted to baseline due to detected regression or operator failure
};

const char* to_string(RolloutState state) noexcept;

// Fine-grained feature toggle flags allowing surgical disablement of any experimental component
struct RolloutFeatureFlags {
    bool enable_simd_bitmaps = true;
    bool enable_positional_encoding = true;
    bool enable_sparse_postings = true;
    bool enable_aho_corasick_prefilter = true;
    bool enable_autotuned_layout = true;
    bool enable_parallel_build = true;
    bool enable_lazy_dfa = true;

    // Resets all flags to conservative baseline defaults (all experimental optimizations disabled)
    void disable_all_experimental() noexcept {
        enable_simd_bitmaps = false;
        enable_positional_encoding = false;
        enable_sparse_postings = false;
        enable_aho_corasick_prefilter = false;
        enable_autotuned_layout = false;
        enable_parallel_build = false;
        enable_lazy_dfa = false;
    }
};

// Records an operator fallback or recovery incident
struct FallbackEvent {
    std::string operator_name;
    std::string fallback_reason;
    std::string query_pattern;
    double timestamp_ms = 0.0;
};

// Telemetry tracker monitoring operator safety and triggering automated rollback guards
class RolloutTelemetry {
public:
    RolloutTelemetry(std::size_t max_allowed_fallbacks = 10, double max_fallback_ratio = 0.05)
        : max_allowed_fallbacks_(max_allowed_fallbacks), max_fallback_ratio_(max_fallback_ratio) {}

    void record_query(bool used_fallback, std::string_view op_name = "", std::string_view reason = "") {
        ++total_queries_;
        if (used_fallback) {
            ++total_fallbacks_;
            events_.push_back(FallbackEvent{std::string(op_name), std::string(reason), "", 0.0});
        }
    }

    std::size_t total_queries() const noexcept { return total_queries_; }
    std::size_t total_fallbacks() const noexcept { return total_fallbacks_; }
    double fallback_ratio() const noexcept {
        return total_queries_ > 0 ? (static_cast<double>(total_fallbacks_) / static_cast<double>(total_queries_)) : 0.0;
    }

    // Evaluates whether fallback frequency has breached safety threshold requiring rollback
    bool should_trigger_rollback() const noexcept {
        if (total_queries_ >= 20 && fallback_ratio() > max_fallback_ratio_) return true;
        if (total_fallbacks_ > max_allowed_fallbacks_) return true;
        return false;
    }

    const std::vector<FallbackEvent>& events() const noexcept { return events_; }

private:
    std::size_t max_allowed_fallbacks_;
    double max_fallback_ratio_;
    std::size_t total_queries_ = 0;
    std::size_t total_fallbacks_ = 0;
    std::vector<FallbackEvent> events_;
};

// Rollout and rollback management controller
struct RolloutPolicyController {
    std::string profile_id = "pergrep-production-v1";
    RolloutState state = RolloutState::DefaultBaseline;
    RolloutFeatureFlags flags;
    RolloutTelemetry telemetry;
    std::string rollback_reason;

    // Checks telemetry and automatically triggers rollback if thresholds are breached
    bool evaluate_safety_guard() {
        if (state == RolloutState::RolloutActive || state == RolloutState::CanaryExperimental) {
            if (telemetry.should_trigger_rollback()) {
                state = RolloutState::RollbackEnforced;
                rollback_reason = "Automated safety rollback: fallback ratio (" +
                                  std::to_string(telemetry.fallback_ratio() * 100.0) +
                                  "%) exceeded safety threshold.";
                flags.disable_all_experimental();
                return true;
            }
        }
        return false;
    }

    // Explicit manual rollback trigger
    void enforce_rollback(std::string_view reason) {
        state = RolloutState::RollbackEnforced;
        rollback_reason = std::string(reason);
        flags.disable_all_experimental();
    }

    // Promotes candidate to active rollout
    void promote_to_rollout() {
        if (state != RolloutState::RollbackEnforced) {
            state = RolloutState::RolloutActive;
        }
    }

    // Generates human-readable release notes and operational runbook for changed behaviors
    std::string generate_release_notes() const;
};

} // namespace pergrep::rollout
