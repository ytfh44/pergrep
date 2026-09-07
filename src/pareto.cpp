#include "pergrep/pareto.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace pergrep::pareto {

bool HardConstraintBounds::check_satisfaction(const WorkloadMetrics& m, std::string* violation_msg) const {
    if (m.build_time_ms > max_build_time_ms) {
        if (violation_msg) *violation_msg = "build_time_ms (" + std::to_string(m.build_time_ms) + " > " + std::to_string(max_build_time_ms) + ")";
        return false;
    }
    if (m.index_size_bytes > max_index_size_bytes) {
        if (violation_msg) *violation_msg = "index_size_bytes (" + std::to_string(m.index_size_bytes) + " > " + std::to_string(max_index_size_bytes) + ")";
        return false;
    }
    if (m.peak_rss_bytes > max_peak_rss_bytes) {
        if (violation_msg) *violation_msg = "peak_rss_bytes (" + std::to_string(m.peak_rss_bytes) + " > " + std::to_string(max_peak_rss_bytes) + ")";
        return false;
    }
    if (m.cold_p50_ms > max_cold_p50_ms) {
        if (violation_msg) *violation_msg = "cold_p50_ms (" + std::to_string(m.cold_p50_ms) + " > " + std::to_string(max_cold_p50_ms) + ")";
        return false;
    }
    if (m.warm_p50_ms > max_warm_p50_ms) {
        if (violation_msg) *violation_msg = "warm_p50_ms (" + std::to_string(m.warm_p50_ms) + " > " + std::to_string(max_warm_p50_ms) + ")";
        return false;
    }
    if (m.p95_ms > max_p95_ms) {
        if (violation_msg) *violation_msg = "p95_ms (" + std::to_string(m.p95_ms) + " > " + std::to_string(max_p95_ms) + ")";
        return false;
    }
    if (m.fallback_rate > max_fallback_rate) {
        if (violation_msg) *violation_msg = "fallback_rate (" + std::to_string(m.fallback_rate) + " > " + std::to_string(max_fallback_rate) + ")";
        return false;
    }
    return true;
}

void ObjectiveWeights::normalize() noexcept {
    double total = weight_build_time + weight_index_size + weight_peak_rss +
                   weight_cold_latency + weight_warm_latency + weight_p95_latency +
                   weight_verified_bytes + weight_fallback_rate;
    if (total <= 0.0) total = 1.0;
    weight_build_time /= total;
    weight_index_size /= total;
    weight_peak_rss /= total;
    weight_cold_latency /= total;
    weight_warm_latency /= total;
    weight_p95_latency /= total;
    weight_verified_bytes /= total;
    weight_fallback_rate /= total;
}

double ObjectiveWeights::compute_loss(const WorkloadMetrics& m, const WorkloadMetrics& base) const noexcept {
    auto ratio = [](double val, double b) noexcept {
        return (b > 0.0) ? (val / b) : val;
    };

    double loss = 0.0;
    loss += weight_build_time * ratio(m.build_time_ms, base.build_time_ms);
    loss += weight_index_size * ratio(static_cast<double>(m.index_size_bytes), static_cast<double>(base.index_size_bytes));
    loss += weight_peak_rss * ratio(static_cast<double>(m.peak_rss_bytes), static_cast<double>(base.peak_rss_bytes));
    loss += weight_cold_latency * ratio(m.cold_p50_ms, base.cold_p50_ms);
    loss += weight_warm_latency * ratio(m.warm_p50_ms, base.warm_p50_ms);
    loss += weight_p95_latency * ratio(m.p95_ms, base.p95_ms);
    loss += weight_verified_bytes * ratio(static_cast<double>(m.verified_bytes), static_cast<double>(base.verified_bytes));
    loss += weight_fallback_rate * ratio(m.fallback_rate, std::max(base.fallback_rate, 0.01));
    return loss;
}

bool pareto_dominates(const WorkloadMetrics& a, const WorkloadMetrics& b) noexcept {
    bool strictly_better = false;

    if (a.build_time_ms > b.build_time_ms) return false;
    if (a.build_time_ms < b.build_time_ms) strictly_better = true;

    if (a.index_size_bytes > b.index_size_bytes) return false;
    if (a.index_size_bytes < b.index_size_bytes) strictly_better = true;

    if (a.peak_rss_bytes > b.peak_rss_bytes) return false;
    if (a.peak_rss_bytes < b.peak_rss_bytes) strictly_better = true;

    if (a.cold_p50_ms > b.cold_p50_ms) return false;
    if (a.cold_p50_ms < b.cold_p50_ms) strictly_better = true;

    if (a.warm_p50_ms > b.warm_p50_ms) return false;
    if (a.warm_p50_ms < b.warm_p50_ms) strictly_better = true;

    if (a.p95_ms > b.p95_ms) return false;
    if (a.p95_ms < b.p95_ms) strictly_better = true;

    if (a.verified_bytes > b.verified_bytes) return false;
    if (a.verified_bytes < b.verified_bytes) strictly_better = true;

    if (a.fallback_rate > b.fallback_rate) return false;
    if (a.fallback_rate < b.fallback_rate) strictly_better = true;

    return strictly_better;
}

std::vector<CandidateEvaluation> compute_pareto_frontier(
    std::vector<CandidateEvaluation> candidates,
    const HardConstraintBounds& hard_bounds) {

    // First filter out invalid candidates
    std::vector<CandidateEvaluation> valid;
    for (auto& c : candidates) {
        std::string violation;
        if (hard_bounds.check_satisfaction(c.metrics, &violation)) {
            c.satisfies_hard_limits = true;
            valid.push_back(std::move(c));
        }
    }

    std::vector<CandidateEvaluation> frontier;
    for (std::size_t i = 0; i < valid.size(); ++i) {
        bool dominated = false;
        for (std::size_t j = 0; j < valid.size(); ++j) {
            if (i != j && pareto_dominates(valid[j].metrics, valid[i].metrics)) {
                dominated = true;
                break;
            }
        }
        if (!dominated) {
            frontier.push_back(valid[i]);
        }
    }
    return frontier;
}

ParetoSelectionResult select_optimal_configuration(
    std::vector<CandidateEvaluation> candidates,
    const ProductTuningProfile& profile) {

    ParetoSelectionResult res;

    // 1. Hard constraint evaluation
    std::vector<CandidateEvaluation> valid;
    for (auto& c : candidates) {
        std::string violation;
        if (!profile.hard_bounds.check_satisfaction(c.metrics, &violation)) {
            c.satisfies_hard_limits = false;
            c.hard_limit_violation = violation;
            res.rejected_hard_limits.push_back(std::move(c));
        } else {
            c.satisfies_hard_limits = true;
            valid.push_back(std::move(c));
        }
    }

    if (valid.empty()) {
        res.rationale_report = "Error: All candidate configurations violated hard product constraints.";
        return res;
    }

    // 2. Compute non-dominated Pareto frontier
    for (std::size_t i = 0; i < valid.size(); ++i) {
        bool dominated = false;
        for (std::size_t j = 0; j < valid.size(); ++j) {
            if (i != j && pareto_dominates(valid[j].metrics, valid[i].metrics)) {
                dominated = true;
                break;
            }
        }
        if (!dominated) {
            res.pareto_frontier.push_back(valid[i]);
        } else {
            res.dominated_candidates.push_back(valid[i]);
        }
    }

    // 3. Compute weighted loss across frontier candidates
    auto weights = profile.weights;
    weights.normalize();

    double best_loss = std::numeric_limits<double>::infinity();
    std::size_t best_idx = 0;

    for (std::size_t i = 0; i < res.pareto_frontier.size(); ++i) {
        res.pareto_frontier[i].weighted_loss = weights.compute_loss(res.pareto_frontier[i].metrics, profile.baseline);
        if (res.pareto_frontier[i].weighted_loss < best_loss) {
            best_loss = res.pareto_frontier[i].weighted_loss;
            best_idx = i;
        }
    }

    res.selected = res.pareto_frontier[best_idx];
    res.chosen_candidate_id = res.selected->candidate_id;

    // 4. Generate audit rationale report
    std::ostringstream ss;
    ss << "=== Pareto Configuration Selection Audit ===\n";
    ss << "Profile: " << profile.profile_name << " (" << profile.description << ")\n";
    ss << "Total Candidates Evaluated: " << candidates.size() << "\n";
    ss << "Rejected (Hard Limit Violations): " << res.rejected_hard_limits.size() << "\n";
    for (const auto& r : res.rejected_hard_limits) {
        ss << "  - [" << r.candidate_id << "] REJECTED: " << r.hard_limit_violation << "\n";
    }
    ss << "Dominated Candidates Filtered: " << res.dominated_candidates.size() << "\n";
    ss << "Pareto Frontier Size: " << res.pareto_frontier.size() << "\n";
    for (const auto& f : res.pareto_frontier) {
        ss << "  * [" << f.candidate_id << "] loss=" << std::fixed << std::setprecision(4)
           << f.weighted_loss << " warm_p50=" << f.metrics.warm_p50_ms
           << "ms index_bytes=" << f.metrics.index_size_bytes << "\n";
    }
    ss << "\nSELECTED OPTIMAL CANDIDATE: " << res.chosen_candidate_id << "\n";
    ss << "Rationale: Satisfies all hard limits; belongs to non-dominated Pareto frontier; "
       << "achieved minimal normalized weighted loss (" << best_loss << ") according to profile priorities.\n";

    res.rationale_report = ss.str();
    return res;
}

ProductTuningProfile profile_balanced_standard() {
    ProductTuningProfile p;
    p.profile_name = "balanced-standard";
    p.description = "General-purpose trade-off between search speed and memory footprint";
    p.hard_bounds.max_build_time_ms = 10000.0;
    p.hard_bounds.max_index_size_bytes = 256 * 1024 * 1024;
    p.hard_bounds.max_peak_rss_bytes = 512 * 1024 * 1024;
    p.hard_bounds.max_cold_p50_ms = 50.0;
    p.hard_bounds.max_warm_p50_ms = 10.0;
    p.hard_bounds.max_p95_ms = 25.0;
    p.hard_bounds.max_fallback_rate = 0.10;

    p.weights.weight_build_time = 0.10;
    p.weights.weight_index_size = 0.20;
    p.weights.weight_peak_rss = 0.10;
    p.weights.weight_cold_latency = 0.15;
    p.weights.weight_warm_latency = 0.30;
    p.weights.weight_p95_latency = 0.10;
    p.weights.weight_verified_bytes = 0.02;
    p.weights.weight_fallback_rate = 0.03;

    p.baseline.build_time_ms = 2000.0;
    p.baseline.index_size_bytes = 64 * 1024 * 1024;
    p.baseline.peak_rss_bytes = 128 * 1024 * 1024;
    p.baseline.cold_p50_ms = 15.0;
    p.baseline.warm_p50_ms = 2.0;
    p.baseline.p95_ms = 5.0;
    p.baseline.verified_bytes = 1024 * 1024;
    p.baseline.fallback_rate = 0.02;
    return p;
}

ProductTuningProfile profile_interactive_low_latency() {
    ProductTuningProfile p = profile_balanced_standard();
    p.profile_name = "interactive-low-latency";
    p.description = "Prioritizes minimum search response latency, tolerates higher index overhead";
    p.hard_bounds.max_warm_p50_ms = 3.0;
    p.hard_bounds.max_p95_ms = 8.0;

    p.weights.weight_build_time = 0.02;
    p.weights.weight_index_size = 0.05;
    p.weights.weight_peak_rss = 0.03;
    p.weights.weight_cold_latency = 0.15;
    p.weights.weight_warm_latency = 0.50;
    p.weights.weight_p95_latency = 0.25;
    return p;
}

ProductTuningProfile profile_embedded_low_memory() {
    ProductTuningProfile p = profile_balanced_standard();
    p.profile_name = "embedded-low-memory";
    p.description = "Strict memory and index size bounds for constrained runtime environments";
    p.hard_bounds.max_index_size_bytes = 32 * 1024 * 1024;
    p.hard_bounds.max_peak_rss_bytes = 64 * 1024 * 1024;
    p.hard_bounds.max_warm_p50_ms = 20.0; // Tolerates higher latency

    p.weights.weight_build_time = 0.05;
    p.weights.weight_index_size = 0.40;
    p.weights.weight_peak_rss = 0.30;
    p.weights.weight_cold_latency = 0.05;
    p.weights.weight_warm_latency = 0.15;
    p.weights.weight_p95_latency = 0.05;
    return p;
}

ProductTuningProfile profile_batch_high_throughput() {
    ProductTuningProfile p = profile_balanced_standard();
    p.profile_name = "batch-high-throughput";
    p.description = "Optimized for continuous batch multi-query verification throughput";
    p.hard_bounds.max_build_time_ms = 60000.0;
    p.hard_bounds.max_index_size_bytes = 1024 * 1024 * 1024;

    p.weights.weight_build_time = 0.05;
    p.weights.weight_index_size = 0.10;
    p.weights.weight_peak_rss = 0.10;
    p.weights.weight_cold_latency = 0.10;
    p.weights.weight_warm_latency = 0.35;
    p.weights.weight_p95_latency = 0.10;
    p.weights.weight_verified_bytes = 0.20;
    return p;
}

} // namespace pergrep::pareto
