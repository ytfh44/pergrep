#pragma once

#include "pergrep/pergrep.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <optional>

namespace pergrep::pareto {

// Workload metrics evaluated across build and search phases (M9.4)
struct WorkloadMetrics {
    double build_time_ms = 0.0;
    std::uint64_t index_size_bytes = 0;
    std::uint64_t peak_rss_bytes = 0;
    double cold_p50_ms = 0.0;
    double warm_p50_ms = 0.0;
    double p95_ms = 0.0;
    std::uint64_t verified_bytes = 0;
    std::uint64_t touched_bytes = 0;
    double fallback_rate = 0.0; // 0.0 to 1.0 (fraction of queries falling back)
};

// Inviolable hard limit constraints that no optimization may violate (M9.4)
struct HardConstraintBounds {
    double max_build_time_ms = 30000.0;
    std::uint64_t max_index_size_bytes = 512 * 1024 * 1024;
    std::uint64_t max_peak_rss_bytes = 1024 * 1024 * 1024;
    double max_cold_p50_ms = 200.0;
    double max_warm_p50_ms = 25.0;
    double max_p95_ms = 50.0;
    double max_fallback_rate = 0.15; // Max 15% fallback allowed

    // Returns true if all hard limits are satisfied; otherwise false and writes explanation
    bool check_satisfaction(const WorkloadMetrics& m, std::string* violation_msg = nullptr) const;
};

// Weighting parameters for multi-criteria loss evaluation
struct ObjectiveWeights {
    double weight_build_time = 0.05;
    double weight_index_size = 0.15;
    double weight_peak_rss = 0.10;
    double weight_cold_latency = 0.15;
    double weight_warm_latency = 0.35;
    double weight_p95_latency = 0.10;
    double weight_verified_bytes = 0.05;
    double weight_fallback_rate = 0.05;

    // Normalizes weights so sum equals 1.0
    void normalize() noexcept;

    // Computes relative weighted loss against baseline metrics
    double compute_loss(const WorkloadMetrics& m, const WorkloadMetrics& baseline) const noexcept;
};

// Product tuning profile defining specialized trade-offs
struct ProductTuningProfile {
    std::string profile_name;
    std::string description;
    HardConstraintBounds hard_bounds;
    ObjectiveWeights weights;
    WorkloadMetrics baseline;
};

// Candidate configuration under Pareto analysis
struct CandidateEvaluation {
    std::string candidate_id;
    IndexOptions options;
    std::string operator_policy = "default";
    WorkloadMetrics metrics;
    bool satisfies_hard_limits = true;
    std::string hard_limit_violation;
    double weighted_loss = 0.0;
};

// Result of Pareto frontier analysis and selection
struct ParetoSelectionResult {
    std::string chosen_candidate_id;
    std::optional<CandidateEvaluation> selected;
    std::vector<CandidateEvaluation> pareto_frontier;
    std::vector<CandidateEvaluation> rejected_hard_limits;
    std::vector<CandidateEvaluation> dominated_candidates;
    std::string rationale_report;
};

// Determines if metric vector A Pareto-dominates vector B (A <= B in all, A < B in at least one)
bool pareto_dominates(const WorkloadMetrics& a, const WorkloadMetrics& b) noexcept;

// Computes the non-dominated Pareto frontier among candidates satisfying hard limits
std::vector<CandidateEvaluation> compute_pareto_frontier(
    std::vector<CandidateEvaluation> candidates,
    const HardConstraintBounds& hard_bounds);

// Evaluates and selects the optimal configuration with full audit rationale
ParetoSelectionResult select_optimal_configuration(
    std::vector<CandidateEvaluation> candidates,
    const ProductTuningProfile& profile);

// Predefined canonical product tuning profiles
ProductTuningProfile profile_balanced_standard();
ProductTuningProfile profile_interactive_low_latency();
ProductTuningProfile profile_embedded_low_memory();
ProductTuningProfile profile_batch_high_throughput();

} // namespace pergrep::pareto
