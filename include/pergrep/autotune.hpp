#pragma once

#include "pergrep/pergrep.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pergrep::autotune {

// M9.1: Bounded parameter ranges for offline exploration.
struct ParameterBounds {
    std::vector<std::size_t> chunk_bytes_candidates = {8192, 16384, 32768, 65536};
    std::vector<std::size_t> chunk_overlap_candidates = {64, 128, 256};
    std::vector<std::size_t> positional_block_bytes_candidates = {128, 256, 512};
    std::vector<double> positional_budget_ratio_candidates = {0.25, 0.50, 0.75};
    std::vector<std::uint64_t> planned_qgrams_candidates = {0, 1, 2, 4};
    std::size_t max_evaluations = 50;
    double max_search_time_budget_ms = 10000.0; // 10s total wall-clock budget
};

// Evaluated candidate configuration and its performance measurements.
struct TunedCandidate {
    IndexOptions options;
    std::string toolchain;
    std::string operator_policy;
    std::uint64_t build_bytes = 0;
    double build_time_ms = 0.0;
    double search_p50_ms = 0.0;
    double search_p95_ms = 0.0;
    double score = 0.0; // Combined objective score (lower is better)
    bool valid = true;
    bool terminated_early = false;
    std::string termination_reason = "completed";
};

// Result of an offline parameter search pass.
struct SearchResult {
    std::vector<TunedCandidate> evaluated;
    std::optional<TunedCandidate> best;
    std::string toolchain_info;
    std::size_t total_configurations_explored = 0;
    std::size_t invalid_configurations_skipped = 0;
    bool budget_exhausted = false;
};

// Returns a human-readable identifier for the current compiler, architecture, and toolchain.
std::string current_toolchain_info();

// Explores parameter space offline across sample documents and queries.
// Never mutates runtime global state or default options.
SearchResult explore_parameter_space(
    const std::vector<Document>& corpus,
    const std::vector<Pattern>& queries,
    const ParameterBounds& bounds = ParameterBounds{},
    std::string operator_policy = "default");

} // namespace pergrep::autotune
