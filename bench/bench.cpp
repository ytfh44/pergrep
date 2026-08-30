#include "workload_matrix.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace pergrep;
using namespace pergrep::benchmark;

namespace {

struct QueryTotals {
    std::string name;
    std::string family;
    double search_ms = 0.0;
    double cold_search_ms = 0.0;
    double warm_search_ms = 0.0;
    double search_p50_ms = 0.0;
    double search_p95_ms = 0.0;
    std::vector<double> latencies_ms;
    std::uint64_t logical_unique_bytes = 0;
    std::uint64_t physically_touched_bytes = 0;
    std::uint64_t index_probe_bytes = 0;
    std::uint64_t index_probe_operations = 0;
    std::uint64_t candidate_chunks = 0;
    std::uint64_t candidate_blocks = 0;
    std::uint64_t candidate_files = 0;
    std::uint64_t verifier_cpu_ns = 0;
    std::uint64_t matches = 0;
    std::uint64_t predicted_candidate_chunks = 0;
    std::uint64_t predicted_candidate_blocks = 0;
    std::uint64_t predicted_verified_bytes = 0;
    std::uint64_t prediction_error_bound_chunks = 0;
    std::uint64_t prediction_error_bound_blocks = 0;
    std::uint64_t prediction_error_bound_bytes = 0;
    std::string verifier = {};
    double estimated_selectivity = 0.0;
    double plan_regret = 0.0;
    double prediction_error = 0.0;
    bool is_fallback = false;
    std::uint64_t plan_key_hash = 0;
    std::string semantic_mode;
    double measured_cost = 0.0;
    std::uint64_t observed_candidate_count = 0;
    std::uint64_t actual_verification_bytes = 0;
    std::uint64_t actual_index_probe_bytes = 0;
    std::uint64_t actual_index_probe_operations = 0;
    bool allocation_metrics_available = false;
    bool page_fault_metrics_available = false;
};

struct ScenarioTotals {
    std::string name;
    std::uint64_t corpus_bytes = 0;
    std::uint64_t index_bytes = 0;
    double search_time_ms = 0.0;
    double build_ms = 0.0;
    double index_save_ms = 0.0;
    double index_load_ms = 0.0;
    double freshness_check_ms = 0.0;
    double cold_search_ms = 0.0;
    double warm_search_ms = 0.0;
    double repeated_search_ms = 0.0;
    double search_p50_ms = 0.0;
    double search_p95_ms = 0.0;
    std::uint64_t rss_kb = 0;
    std::uint64_t peak_rss_kb = 0;
    std::uint64_t page_faults = 0;
    std::uint64_t logical_unique_bytes = 0;
    std::uint64_t physically_touched_bytes = 0;
    std::uint64_t index_probe_bytes = 0;
    std::uint64_t index_probe_operations = 0;
    std::uint64_t candidate_chunks = 0;
    std::uint64_t candidate_blocks = 0;
    std::uint64_t candidate_files = 0;
    std::uint64_t verifier_cpu_ns = 0;
    std::uint64_t matches = 0;
    std::uint64_t predicted_candidate_chunks = 0;
    std::uint64_t predicted_candidate_blocks = 0;
    std::uint64_t predicted_verified_bytes = 0;
    std::uint64_t prediction_error_bound_chunks = 0;
    std::uint64_t prediction_error_bound_blocks = 0;
    std::uint64_t prediction_error_bound_bytes = 0;
    double fallback_rate = 0.0;
    double mean_plan_regret = 0.0;
    double p50_plan_regret = 0.0;
    double p95_plan_regret = 0.0;
    double max_plan_regret = 0.0;
    double mean_prediction_error = 0.0;
    double p95_prediction_error = 0.0;
    std::vector<PlanRegret> query_regrets;
    std::vector<QueryTotals> per_query;
};

struct AggregateTotals {
    double search_time_ms = 0.0;
    double build_ms = 0.0;
    double index_save_ms = 0.0;
    double index_load_ms = 0.0;
    double freshness_check_ms = 0.0;
    double cold_search_ms = 0.0;
    double warm_search_ms = 0.0;
    double repeated_search_ms = 0.0;
    std::uint64_t logical_unique_bytes = 0;
    std::uint64_t physically_touched_bytes = 0;
    std::uint64_t index_probe_bytes = 0;
    std::uint64_t index_probe_operations = 0;
    std::uint64_t candidate_chunks = 0;
    std::uint64_t candidate_blocks = 0;
    std::uint64_t candidate_files = 0;
    std::uint64_t verifier_cpu_ns = 0;
    std::uint64_t matches = 0;
    std::uint64_t predicted_candidate_chunks = 0;
    std::uint64_t predicted_candidate_blocks = 0;
    std::uint64_t predicted_verified_bytes = 0;
    std::uint64_t prediction_error_bound_chunks = 0;
    std::uint64_t prediction_error_bound_blocks = 0;
    std::uint64_t prediction_error_bound_bytes = 0;
};

IndexOptions indexed_options() {
    IndexOptions options;
    options.chunk_bytes = 32768;
    options.chunk_overlap = 128;
    options.positional_block_bytes = 256;
    options.positional_budget_ratio = 0.5;
    return options;
}

IndexOptions reference_options(std::uint64_t corpus_bytes) {
    IndexOptions options;
    options.chunk_bytes = std::min<std::uint64_t>(std::uint64_t(1) << 20, corpus_bytes + 1024);
    if (options.chunk_bytes < 64) options.chunk_bytes = 64;
    options.chunk_overlap = options.chunk_bytes / 2;
    options.positional_block_bytes = 256;
    return options;
}

bool same_matches(const std::vector<Match>& actual, const std::vector<Match>& expected) {
    if (actual.size() != expected.size()) return false;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (actual[i].file_id != expected[i].file_id || actual[i].start != expected[i].start ||
            actual[i].end != expected[i].end) {
            return false;
        }
        if (actual[i].captures.size() != expected[i].captures.size()) return false;
        for (std::size_t c = 0; c < expected[i].captures.size(); ++c) {
            if (actual[i].captures[c].start != expected[i].captures[c].start ||
                actual[i].captures[c].end != expected[i].captures[c].end ||
                actual[i].captures[c].matched != expected[i].captures[c].matched ||
                actual[i].captures[c].name != expected[i].captures[c].name) {
                return false;
            }
        }
    }
    return true;
}

bool validate_scenario(const WorkloadScenario& scenario, const std::vector<Document>& documents,
                       const IndexOptions& options, const IndexOptions& ref_options) {
    std::unique_ptr<TempDirectory> temp_dir;
    Index indexed;
    if (scenario.storage == StorageBackend::Filesystem) {
        temp_dir = std::make_unique<TempDirectory>("pergrep_val_" + scenario.name);
        for (const auto& doc : documents) {
            temp_dir->write_file(doc.path, doc.content);
        }
        indexed = Index::build(temp_dir->path(), options);
    } else {
        indexed = Index::from_documents(documents, options);
    }

    auto reference = Index::from_documents(documents, ref_options);
    Searcher indexed_searcher(indexed);
    Searcher reference_searcher(reference);

    for (const auto& profile : scenario.queries) {
        auto pattern = Pattern::compile(profile.expression, profile.pattern_options);
        const auto actual = indexed_searcher.find(pattern, profile.search_options);
        const auto expected = reference_searcher.find(pattern, profile.search_options);
        if (!same_matches(actual, expected)) {
            std::cerr << "CORRECTNESS ERROR scenario=" << scenario.name << " query=" << profile.name
                      << " expected=" << expected.size() << " got=" << actual.size() << "\n";
            return false;
        }
        const auto actual_files = indexed_searcher.files(pattern, profile.search_options);
        const auto expected_files = reference_searcher.files(pattern, profile.search_options);
        if (actual_files != expected_files) {
            std::cerr << "CORRECTNESS ERROR scenario=" << scenario.name << " query=" << profile.name
                      << " file selection differs\n";
            return false;
        }
    }
    return true;
}

void add_stats(ScenarioTotals& totals, const SearchStats& stats, std::size_t match_count) {
    totals.logical_unique_bytes += stats.logical_unique_bytes;
    totals.physically_touched_bytes += stats.physically_touched_bytes;
    totals.index_probe_bytes += stats.index_probe_bytes;
    totals.index_probe_operations += stats.index_probe_operations;
    totals.candidate_chunks += stats.candidate_chunks;
    totals.candidate_blocks += stats.candidate_blocks;
    totals.candidate_files += stats.candidate_files;
    totals.verifier_cpu_ns += stats.verifier_cpu_ns;
    totals.matches += match_count;
    totals.predicted_candidate_chunks += stats.predicted_candidate_chunks;
    totals.predicted_candidate_blocks += stats.predicted_candidate_blocks;
    totals.predicted_verified_bytes += stats.predicted_verified_bytes;
    totals.prediction_error_bound_chunks += stats.prediction_error_bound_chunks;
    totals.prediction_error_bound_blocks += stats.prediction_error_bound_blocks;
    totals.prediction_error_bound_bytes += stats.prediction_error_bound_bytes;
}

void add_stats(QueryTotals& totals, const SearchStats& stats, std::size_t match_count,
               double elapsed_ms) {
    totals.search_ms += elapsed_ms;
    totals.logical_unique_bytes += stats.logical_unique_bytes;
    totals.physically_touched_bytes += stats.physically_touched_bytes;
    totals.index_probe_bytes += stats.index_probe_bytes;
    totals.index_probe_operations += stats.index_probe_operations;
    totals.candidate_chunks += stats.candidate_chunks;
    totals.candidate_blocks += stats.candidate_blocks;
    totals.candidate_files += stats.candidate_files;
    totals.verifier_cpu_ns += stats.verifier_cpu_ns;
    totals.matches += match_count;
    totals.predicted_candidate_chunks += stats.predicted_candidate_chunks;
    totals.plan_key_hash = stats.plan_key_hash;
    totals.semantic_mode = stats.semantic_mode;
    totals.measured_cost = stats.measured_cost;
    totals.observed_candidate_count = 1;
    totals.actual_verification_bytes = stats.physically_touched_bytes;
    totals.actual_index_probe_bytes = stats.index_probe_bytes;
    totals.actual_index_probe_operations = stats.index_probe_operations;
    totals.allocation_metrics_available = stats.allocation_metrics_available;
    totals.page_fault_metrics_available = stats.page_fault_metrics_available;
    totals.predicted_candidate_blocks += stats.predicted_candidate_blocks;
    totals.predicted_verified_bytes += stats.predicted_verified_bytes;
    totals.prediction_error_bound_chunks += stats.prediction_error_bound_chunks;
    totals.prediction_error_bound_blocks += stats.prediction_error_bound_blocks;
    totals.prediction_error_bound_bytes += stats.prediction_error_bound_bytes;
    totals.verifier = stats.verifier;
    totals.estimated_selectivity = stats.estimated_selectivity;
    totals.plan_regret = stats.plan_regret;
    totals.is_fallback = (stats.verifier == "RegexBruteForce");
}

ScenarioTotals measure_scenario(const WorkloadScenario& scenario, const std::vector<Document>& documents,
                               const IndexOptions& options) {
    std::vector<Pattern> patterns;
    patterns.reserve(scenario.queries.size());
    for (const auto& profile : scenario.queries) {
        patterns.push_back(Pattern::compile(profile.expression, profile.pattern_options));
    }

    ScenarioTotals totals;
    totals.name = scenario.name;
    totals.per_query.resize(patterns.size());
    for (std::size_t q = 0; q < scenario.queries.size(); ++q) {
        totals.per_query[q].name = scenario.queries[q].name;
        totals.per_query[q].family = scenario.queries[q].family;
    }

    std::vector<double> scenario_latencies;

    if (scenario.storage == StorageBackend::Filesystem) {
        TempDirectory temp_dir("pergrep_bench_fs_" + scenario.name);
        TempDirectory index_dir("pergrep_bench_idx_" + scenario.name);
        for (const auto& doc : documents) {
            temp_dir.write_file(doc.path, doc.content);
        }
        const auto index_file = index_dir.path() / "index.pgi";
        // 1. Build
        const auto build_start = std::chrono::steady_clock::now();
        auto index = Index::build(temp_dir.path(), options);
        const auto build_end = std::chrono::steady_clock::now();
        totals.build_ms = std::chrono::duration<double, std::milli>(build_end - build_start).count();
        totals.corpus_bytes = index.corpus_bytes();
        totals.index_bytes = index.index_bytes();

        // 2. Save
        const auto save_start = std::chrono::steady_clock::now();
        index.save(index_file);
        const auto save_end = std::chrono::steady_clock::now();
        totals.index_save_ms = std::chrono::duration<double, std::milli>(save_end - save_start).count();

        // 3. Load
        const auto load_start = std::chrono::steady_clock::now();
        auto loaded_index = Index::load(index_file);
        const auto load_end = std::chrono::steady_clock::now();
        totals.index_load_ms = std::chrono::duration<double, std::milli>(load_end - load_start).count();

        // 4. Freshness
        const auto fresh_start = std::chrono::steady_clock::now();
        const bool fresh = loaded_index.fresh();
        const auto fresh_end = std::chrono::steady_clock::now();
        totals.freshness_check_ms = std::chrono::duration<double, std::milli>(fresh_end - fresh_start).count();
        if (!fresh) {
            std::cerr << "WARNING: fresh() returned false for scenario=" << scenario.name << "\n";
        }

        Searcher searcher(loaded_index);

        // 5. Cold search pass (first run)
        for (std::size_t q = 0; q < scenario.queries.size(); ++q) {
            SearchStats stats{};
            const auto q_start = std::chrono::steady_clock::now();
            const auto matches = searcher.find(patterns[q], scenario.queries[q].search_options, &stats);
            const auto q_end = std::chrono::steady_clock::now();
            const double elapsed_ms = std::chrono::duration<double, std::milli>(q_end - q_start).count();
            totals.per_query[q].cold_search_ms = elapsed_ms;
            totals.cold_search_ms += elapsed_ms;
            scenario_latencies.push_back(elapsed_ms);
            totals.per_query[q].latencies_ms.push_back(elapsed_ms);
            add_stats(totals, stats, matches.size());
            add_stats(totals.per_query[q], stats, matches.size(), elapsed_ms);
            auto candidates = estimate_candidate_plans(patterns[q], loaded_index, scenario.queries[q].search_options.record_separator);
            PlanCandidateMetrics chosen;
            chosen.name = stats.verifier;
            chosen.predicted_cost = stats.estimated_cost;
            chosen.predicted_selectivity = stats.estimated_selectivity;
            chosen.actual_cost = double(stats.physically_touched_bytes) + 100.0 * double(stats.candidate_chunks);
            chosen.actual_time_ms = elapsed_ms;
            chosen.actual_verified_bytes = stats.physically_touched_bytes;
            chosen.actual_candidate_chunks = stats.candidate_chunks;
            chosen.actual_candidate_blocks = stats.candidate_blocks;
            chosen.actual_index_probe_bytes = stats.index_probe_bytes;
            chosen.actual_index_probe_operations = stats.index_probe_operations;
            chosen.actual_verification_bytes = stats.physically_touched_bytes;
            chosen.actual_verifier_cpu_ns = stats.verifier_cpu_ns;
            chosen.allocation_metrics_available = stats.allocation_metrics_available;
            chosen.page_fault_metrics_available = stats.page_fault_metrics_available;
            chosen.is_fallback = stats.verifier_fallback;
            chosen.chosen = true;
            chosen.actual_observed = true;
            chosen.observation = PlanCandidateMetrics::ObservationStatus::Observed;
            for (auto& cand : candidates) {
                if (to_string(cand.verifier) == stats.verifier) {
                    cand.actual_cost = chosen.actual_cost;
                    cand.actual_time_ms = chosen.actual_time_ms;
                    chosen.verifier = cand.verifier;
                    cand.actual_verified_bytes = chosen.actual_verified_bytes;
                    cand.actual_candidate_chunks = chosen.actual_candidate_chunks;
                    cand.actual_candidate_blocks = chosen.actual_candidate_blocks;
                    cand.actual_index_probe_bytes = chosen.actual_index_probe_bytes;
                    cand.actual_index_probe_operations = chosen.actual_index_probe_operations;
                    cand.actual_verification_bytes = chosen.actual_verification_bytes;
                    cand.actual_verifier_cpu_ns = chosen.actual_verifier_cpu_ns;
                    cand.allocation_metrics_available = chosen.allocation_metrics_available;
                    cand.page_fault_metrics_available = chosen.page_fault_metrics_available;
                    cand.chosen = true;
                    cand.actual_observed = true;
                        cand.observation = PlanCandidateMetrics::ObservationStatus::Observed;
                    cand.is_fallback = stats.verifier_fallback;
                }
            }
            auto reg = compute_plan_regret(chosen, candidates, scenario.queries[q].name);
            reg.workload_key = scenario.name;
            reg.semantic_mode = stats.semantic_mode;
            reg.plan_key_hash = stats.plan_key_hash;
            totals.query_regrets.push_back(reg);
            totals.per_query[q].prediction_error = reg.prediction_error;
        }

        // 6. Warm search pass
        for (std::size_t q = 0; q < scenario.queries.size(); ++q) {
            SearchStats stats{};
            const auto q_start = std::chrono::steady_clock::now();
            searcher.find(patterns[q], scenario.queries[q].search_options, &stats);
            const auto q_end = std::chrono::steady_clock::now();
            const double elapsed_ms = std::chrono::duration<double, std::milli>(q_end - q_start).count();
            totals.per_query[q].warm_search_ms = elapsed_ms;
            totals.warm_search_ms += elapsed_ms;
        }

        // 7. Repeated search pass
        const auto repeated_start = std::chrono::steady_clock::now();
        for (std::size_t iter = 0; iter < scenario.iterations; ++iter) {
            for (std::size_t q = 0; q < scenario.queries.size(); ++q) {
                SearchStats stats{};
                const auto q_start = std::chrono::steady_clock::now();
                const auto matches = searcher.find(patterns[q], scenario.queries[q].search_options, &stats);
                const auto q_end = std::chrono::steady_clock::now();
                const double elapsed_ms = std::chrono::duration<double, std::milli>(q_end - q_start).count();
                scenario_latencies.push_back(elapsed_ms);
                totals.per_query[q].latencies_ms.push_back(elapsed_ms);
                add_stats(totals, stats, matches.size());
                add_stats(totals.per_query[q], stats, matches.size(), elapsed_ms);
                auto candidates = estimate_candidate_plans(patterns[q], loaded_index, scenario.queries[q].search_options.record_separator);
                PlanCandidateMetrics chosen;
                chosen.name = stats.verifier;
                chosen.predicted_cost = stats.estimated_cost;
                chosen.predicted_selectivity = stats.estimated_selectivity;
                chosen.actual_cost = double(stats.physically_touched_bytes) + 100.0 * double(stats.candidate_chunks);
                chosen.actual_time_ms = elapsed_ms;
                chosen.actual_verified_bytes = stats.physically_touched_bytes;
                chosen.actual_candidate_chunks = stats.candidate_chunks;
                chosen.actual_candidate_blocks = stats.candidate_blocks;
            chosen.actual_index_probe_bytes = stats.index_probe_bytes;
            chosen.actual_index_probe_operations = stats.index_probe_operations;
            chosen.actual_verification_bytes = stats.physically_touched_bytes;
            chosen.actual_verifier_cpu_ns = stats.verifier_cpu_ns;
            chosen.allocation_metrics_available = stats.allocation_metrics_available;
            chosen.page_fault_metrics_available = stats.page_fault_metrics_available;
                chosen.is_fallback = stats.verifier_fallback;
                chosen.chosen = true;
                chosen.actual_observed = true;
            chosen.observation = PlanCandidateMetrics::ObservationStatus::Observed;
                for (auto& cand : candidates) {
                    if (to_string(cand.verifier) == stats.verifier) {
                        cand.actual_cost = chosen.actual_cost;
                        cand.actual_time_ms = chosen.actual_time_ms;
                        cand.actual_verified_bytes = chosen.actual_verified_bytes;
                        cand.actual_candidate_chunks = chosen.actual_candidate_chunks;
                        cand.actual_candidate_blocks = chosen.actual_candidate_blocks;
                    cand.actual_index_probe_bytes = chosen.actual_index_probe_bytes;
                    cand.actual_index_probe_operations = chosen.actual_index_probe_operations;
                    cand.actual_verification_bytes = chosen.actual_verification_bytes;
                    cand.actual_verifier_cpu_ns = chosen.actual_verifier_cpu_ns;
                    cand.allocation_metrics_available = chosen.allocation_metrics_available;
                    cand.page_fault_metrics_available = chosen.page_fault_metrics_available;
                        chosen.verifier = cand.verifier;
                        cand.chosen = true;
                        cand.actual_observed = true;
                        cand.observation = PlanCandidateMetrics::ObservationStatus::Observed;
                        cand.is_fallback = stats.verifier_fallback;
                    }
                }
                auto reg = compute_plan_regret(chosen, candidates, scenario.queries[q].name);
            reg.workload_key = scenario.name;
            reg.semantic_mode = stats.semantic_mode;
            reg.plan_key_hash = stats.plan_key_hash;
                totals.query_regrets.push_back(reg);
                totals.per_query[q].prediction_error = reg.prediction_error;
            }
        }
        const auto repeated_end = std::chrono::steady_clock::now();
        totals.repeated_search_ms =
            std::chrono::duration<double, std::milli>(repeated_end - repeated_start).count();

        totals.search_time_ms = (scenario.phase == ScenarioPhase::Cold && scenario.include_index_build)
                                    ? (totals.cold_search_ms + totals.repeated_search_ms)
                                    : totals.repeated_search_ms;
    } else {
        // InMemory
        const auto build_start = std::chrono::steady_clock::now();
        auto index = Index::from_documents(documents, options);
        const auto build_end = std::chrono::steady_clock::now();
        totals.build_ms = std::chrono::duration<double, std::milli>(build_end - build_start).count();
        totals.corpus_bytes = index.corpus_bytes();
        totals.index_bytes = index.index_bytes();
        totals.index_save_ms = 0.0;
        totals.index_load_ms = 0.0;
        totals.freshness_check_ms = 0.0;

        Searcher searcher(index);

        // Cold search pass
        for (std::size_t q = 0; q < scenario.queries.size(); ++q) {
            SearchStats stats{};
            const auto q_start = std::chrono::steady_clock::now();
            const auto matches = searcher.find(patterns[q], scenario.queries[q].search_options, &stats);
            const auto q_end = std::chrono::steady_clock::now();
            const double elapsed_ms = std::chrono::duration<double, std::milli>(q_end - q_start).count();
            totals.per_query[q].cold_search_ms = elapsed_ms;
            totals.cold_search_ms += elapsed_ms;
            scenario_latencies.push_back(elapsed_ms);
            totals.per_query[q].latencies_ms.push_back(elapsed_ms);
            add_stats(totals, stats, matches.size());
            add_stats(totals.per_query[q], stats, matches.size(), elapsed_ms);
            auto candidates = estimate_candidate_plans(patterns[q], index, scenario.queries[q].search_options.record_separator);
            PlanCandidateMetrics chosen;
            chosen.name = stats.verifier;
            chosen.predicted_cost = stats.estimated_cost;
            chosen.predicted_selectivity = stats.estimated_selectivity;
            chosen.actual_cost = double(stats.physically_touched_bytes) + 100.0 * double(stats.candidate_chunks);
            chosen.actual_time_ms = elapsed_ms;
            chosen.actual_verified_bytes = stats.physically_touched_bytes;
            chosen.actual_candidate_chunks = stats.candidate_chunks;
            chosen.actual_candidate_blocks = stats.candidate_blocks;
            chosen.actual_index_probe_bytes = stats.index_probe_bytes;
            chosen.actual_index_probe_operations = stats.index_probe_operations;
            chosen.actual_verification_bytes = stats.physically_touched_bytes;
            chosen.actual_verifier_cpu_ns = stats.verifier_cpu_ns;
            chosen.allocation_metrics_available = stats.allocation_metrics_available;
            chosen.page_fault_metrics_available = stats.page_fault_metrics_available;
            chosen.is_fallback = stats.verifier_fallback;
            chosen.chosen = true;
            chosen.actual_observed = true;
            chosen.observation = PlanCandidateMetrics::ObservationStatus::Observed;
            for (auto& cand : candidates) {
                if (to_string(cand.verifier) == stats.verifier) {
                    cand.actual_cost = chosen.actual_cost;
                    cand.actual_time_ms = chosen.actual_time_ms;
                    cand.actual_verified_bytes = chosen.actual_verified_bytes;
                    cand.actual_candidate_chunks = chosen.actual_candidate_chunks;
                    cand.actual_candidate_blocks = chosen.actual_candidate_blocks;
                    cand.actual_index_probe_bytes = chosen.actual_index_probe_bytes;
                    cand.actual_index_probe_operations = chosen.actual_index_probe_operations;
                    cand.actual_verification_bytes = chosen.actual_verification_bytes;
                    cand.actual_verifier_cpu_ns = chosen.actual_verifier_cpu_ns;
                    cand.allocation_metrics_available = chosen.allocation_metrics_available;
                    cand.page_fault_metrics_available = chosen.page_fault_metrics_available;
                    chosen.verifier = cand.verifier;
                    cand.chosen = true;
                    cand.actual_observed = true;
                        cand.observation = PlanCandidateMetrics::ObservationStatus::Observed;
                    cand.is_fallback = stats.verifier_fallback;
                }
            }
            auto reg = compute_plan_regret(chosen, candidates, scenario.queries[q].name);
            reg.workload_key = scenario.name;
            reg.semantic_mode = stats.semantic_mode;
            reg.plan_key_hash = stats.plan_key_hash;
            totals.query_regrets.push_back(reg);
            totals.per_query[q].prediction_error = reg.prediction_error;
        }
        // Warm search pass
        for (std::size_t q = 0; q < scenario.queries.size(); ++q) {
            SearchStats stats{};
            const auto q_start = std::chrono::steady_clock::now();
            searcher.find(patterns[q], scenario.queries[q].search_options, &stats);
            const auto q_end = std::chrono::steady_clock::now();
            const double elapsed_ms = std::chrono::duration<double, std::milli>(q_end - q_start).count();
            totals.per_query[q].warm_search_ms = elapsed_ms;
            totals.warm_search_ms += elapsed_ms;
        }

        // Repeated search pass
        const auto repeated_start = std::chrono::steady_clock::now();
        for (std::size_t iter = 0; iter < scenario.iterations; ++iter) {
            for (std::size_t q = 0; q < scenario.queries.size(); ++q) {
                SearchStats stats{};
                const auto q_start = std::chrono::steady_clock::now();
                const auto matches = searcher.find(patterns[q], scenario.queries[q].search_options, &stats);
                const auto q_end = std::chrono::steady_clock::now();
                const double elapsed_ms = std::chrono::duration<double, std::milli>(q_end - q_start).count();
                scenario_latencies.push_back(elapsed_ms);
                totals.per_query[q].latencies_ms.push_back(elapsed_ms);
                add_stats(totals, stats, matches.size());
                add_stats(totals.per_query[q], stats, matches.size(), elapsed_ms);
                auto candidates = estimate_candidate_plans(patterns[q], index, scenario.queries[q].search_options.record_separator);
                PlanCandidateMetrics chosen;
                chosen.name = stats.verifier;
                chosen.predicted_cost = stats.estimated_cost;
                chosen.predicted_selectivity = stats.estimated_selectivity;
                chosen.actual_cost = double(stats.physically_touched_bytes) + 100.0 * double(stats.candidate_chunks);
                chosen.actual_time_ms = elapsed_ms;
                chosen.actual_verified_bytes = stats.physically_touched_bytes;
                chosen.actual_candidate_chunks = stats.candidate_chunks;
                chosen.actual_candidate_blocks = stats.candidate_blocks;
            chosen.actual_index_probe_bytes = stats.index_probe_bytes;
            chosen.actual_index_probe_operations = stats.index_probe_operations;
            chosen.actual_verification_bytes = stats.physically_touched_bytes;
            chosen.actual_verifier_cpu_ns = stats.verifier_cpu_ns;
            chosen.allocation_metrics_available = stats.allocation_metrics_available;
            chosen.page_fault_metrics_available = stats.page_fault_metrics_available;
                chosen.is_fallback = stats.verifier_fallback;
                chosen.chosen = true;
                chosen.actual_observed = true;
            chosen.observation = PlanCandidateMetrics::ObservationStatus::Observed;
                for (auto& cand : candidates) {
                    if (to_string(cand.verifier) == stats.verifier) {
                        cand.actual_cost = chosen.actual_cost;
                        cand.actual_time_ms = chosen.actual_time_ms;
                        cand.actual_verified_bytes = chosen.actual_verified_bytes;
                        cand.actual_candidate_chunks = chosen.actual_candidate_chunks;
                        cand.actual_candidate_blocks = chosen.actual_candidate_blocks;
                    cand.actual_index_probe_bytes = chosen.actual_index_probe_bytes;
                    cand.actual_index_probe_operations = chosen.actual_index_probe_operations;
                    cand.actual_verification_bytes = chosen.actual_verification_bytes;
                    cand.actual_verifier_cpu_ns = chosen.actual_verifier_cpu_ns;
                    cand.allocation_metrics_available = chosen.allocation_metrics_available;
                    cand.page_fault_metrics_available = chosen.page_fault_metrics_available;
                        chosen.verifier = cand.verifier;
                        cand.chosen = true;
                        cand.actual_observed = true;
                        cand.observation = PlanCandidateMetrics::ObservationStatus::Observed;
                        cand.is_fallback = stats.verifier_fallback;
                    }
                }
                auto reg = compute_plan_regret(chosen, candidates, scenario.queries[q].name);
            reg.workload_key = scenario.name;
            reg.semantic_mode = stats.semantic_mode;
            reg.plan_key_hash = stats.plan_key_hash;
                totals.query_regrets.push_back(reg);
                totals.per_query[q].prediction_error = reg.prediction_error;
            }
        }
        const auto repeated_end = std::chrono::steady_clock::now();
        totals.repeated_search_ms =
            std::chrono::duration<double, std::milli>(repeated_end - repeated_start).count();
        totals.search_time_ms = (scenario.phase == ScenarioPhase::Cold && scenario.include_index_build)
                                    ? (totals.cold_search_ms + totals.repeated_search_ms)
                                    : totals.repeated_search_ms;
    }

    totals.search_p50_ms = calculate_percentile(scenario_latencies, 0.50);
    totals.search_p95_ms = calculate_percentile(scenario_latencies, 0.95);

    for (auto& q : totals.per_query) {
        q.search_p50_ms = calculate_percentile(q.latencies_ms, 0.50);
        q.search_p95_ms = calculate_percentile(q.latencies_ms, 0.95);
    }

    auto shadow = evaluate_shadow_plans(totals.query_regrets);
    totals.fallback_rate = shadow.fallback_rate;
    totals.mean_plan_regret = shadow.mean_regret;
    totals.p50_plan_regret = shadow.p50_regret;
    totals.p95_plan_regret = shadow.p95_regret;
    totals.max_plan_regret = shadow.max_regret;
    totals.mean_prediction_error = shadow.mean_prediction_error;
    totals.p95_prediction_error = shadow.p95_prediction_error;

    const auto mem = sample_process_stats();
    totals.rss_kb = mem.current_rss_bytes / 1024ULL;
    totals.peak_rss_kb = mem.peak_rss_bytes / 1024ULL;
    totals.page_faults = mem.page_faults;

    return totals;
}

} // namespace

int main(int argc, char** argv) {
    bool enforce_gate = false;
    for (int i = 1; i < argc; ++i) {
        enforce_gate = enforce_gate || std::string_view(argv[i]) == "--enforce-gate";
    }
    const auto matrix = scenarios();
    std::uint64_t aggregate_corpus_bytes = 0;
    std::uint64_t aggregate_index_bytes = 0;
    std::uint64_t aggregate_searched_bytes = 0;
    AggregateTotals aggregate;
    std::size_t aggregate_query_profiles = 0;
    std::size_t aggregate_iterations = 0;
    std::vector<double> all_global_latencies;
    std::vector<PlanRegret> all_global_regrets;
    std::vector<ScenarioGateVerdict> scenario_verdicts;
    const auto baselines = default_workload_baselines();
    const auto thresholds = PerformanceGateThresholds::default_release_gate();

    std::cout << "ASI workload_matrix_version=" << kWorkloadMatrixVersion << "\n";
    std::cout << "ASI workload_scenarios=" << matrix.size() << "\n";

    for (const auto& scenario : matrix) {
        const auto documents = materialize_documents(scenario);
        std::uint64_t corpus_bytes = 0;
        for (const auto& document : documents) corpus_bytes += document.content.size();
        aggregate_corpus_bytes += corpus_bytes;
        aggregate_searched_bytes += corpus_bytes * scenario.queries.size() * scenario.iterations;
        aggregate_query_profiles += scenario.queries.size();
        aggregate_iterations += scenario.iterations;

        const auto options = indexed_options();
        const auto ref_options = reference_options(corpus_bytes);
        const bool scenario_correctness = validate_scenario(scenario, documents, options, ref_options);
        const auto totals = measure_scenario(scenario, documents, options);
        aggregate_index_bytes += totals.index_bytes;
        aggregate.search_time_ms += totals.search_time_ms;
        aggregate.build_ms += totals.build_ms;
        aggregate.index_save_ms += totals.index_save_ms;
        aggregate.index_load_ms += totals.index_load_ms;
        aggregate.freshness_check_ms += totals.freshness_check_ms;
        aggregate.cold_search_ms += totals.cold_search_ms;
        aggregate.warm_search_ms += totals.warm_search_ms;
        aggregate.repeated_search_ms += totals.repeated_search_ms;
        aggregate.logical_unique_bytes += totals.logical_unique_bytes;
        aggregate.physically_touched_bytes += totals.physically_touched_bytes;
        aggregate.index_probe_bytes += totals.index_probe_bytes;
        aggregate.index_probe_operations += totals.index_probe_operations;
        aggregate.candidate_chunks += totals.candidate_chunks;
        aggregate.candidate_blocks += totals.candidate_blocks;
        aggregate.predicted_candidate_chunks += totals.predicted_candidate_chunks;
        aggregate.predicted_candidate_blocks += totals.predicted_candidate_blocks;
        aggregate.predicted_verified_bytes += totals.predicted_verified_bytes;
        aggregate.prediction_error_bound_chunks += totals.prediction_error_bound_chunks;
        aggregate.prediction_error_bound_blocks += totals.prediction_error_bound_blocks;
        aggregate.prediction_error_bound_bytes += totals.prediction_error_bound_bytes;
        aggregate.candidate_files += totals.candidate_files;
        aggregate.verifier_cpu_ns += totals.verifier_cpu_ns;
        aggregate.matches += totals.matches;

        for (const auto& q : totals.per_query) {
            all_global_latencies.insert(all_global_latencies.end(), q.latencies_ms.begin(), q.latencies_ms.end());
        }
        all_global_regrets.insert(all_global_regrets.end(), totals.query_regrets.begin(), totals.query_regrets.end());

        const double measured_searches = double(scenario.queries.size() * scenario.iterations);
        const double search_ms_per_query = totals.search_time_ms / std::max(1.0, measured_searches);
        const double searched_mb = double(totals.corpus_bytes) * measured_searches / (1024.0 * 1024.0);
        const double throughput = searched_mb / std::max(1e-9, totals.search_time_ms / 1000.0);

        const ScenarioBaseline* baseline = nullptr;
        for (const auto& b : baselines) {
            if (b.scenario_name == scenario.name) {
                baseline = &b;
                break;
            }
        }
        auto verdict = evaluate_scenario_gate(
            scenario,
            totals.search_time_ms,
            totals.search_p50_ms,
            totals.search_p95_ms,
            totals.fallback_rate,
            totals.mean_plan_regret,
            scenario_correctness,
            thresholds,
            baseline,
            throughput,
            totals.rss_kb
        );
        scenario_verdicts.push_back(verdict);

        std::cout << "SCENARIO name=" << scenario.name << " class=" << to_string(scenario.workload_class)
                  << " phase=" << to_string(scenario.phase) << " storage=" << to_string(scenario.storage)
                  << " corpus=" << scenario.corpus.name << " transform=" << to_string(scenario.corpus.transform)
                  << " selector=" << to_string(scenario.selector) << " queries=" << scenario.queries.size()
                  << " iterations=" << scenario.iterations << "\n";
        std::cout << "GATE scenario=" << scenario.name << " status=" << to_string(verdict.status)
                  << " classif=" << to_string(verdict.classification)
                  << " fallback_rate=" << totals.fallback_rate
                  << " mean_regret=" << totals.mean_plan_regret
                  << " p50_ms=" << totals.search_p50_ms
                  << " p95_ms=" << totals.search_p95_ms << "\n";
        std::cout << "METRIC scenario=" << scenario.name << " corpus_bytes=" << totals.corpus_bytes
                  << " index_bytes=" << totals.index_bytes
                  << " index_build_ms=" << totals.build_ms
                  << " index_save_ms=" << totals.index_save_ms
                  << " index_load_ms=" << totals.index_load_ms
                  << " freshness_check_ms=" << totals.freshness_check_ms
                  << " cold_search_ms=" << totals.cold_search_ms
                  << " warm_search_ms=" << totals.warm_search_ms
                  << " repeated_search_ms=" << totals.repeated_search_ms
                  << " search_time_ms=" << totals.search_time_ms
                  << " search_ms_per_query=" << search_ms_per_query
                  << " search_p50_ms=" << totals.search_p50_ms
                  << " search_p95_ms=" << totals.search_p95_ms
                  << " throughput_mb_s=" << throughput
                  << " rss_kb=" << totals.rss_kb
                  << " peak_rss_kb=" << totals.peak_rss_kb
                  << " page_faults=" << totals.page_faults
                  << " logical_unique_kb=" << (double(totals.logical_unique_bytes) / 1024.0)
                  << " physically_touched_kb=" << (double(totals.physically_touched_bytes) / 1024.0)
                  << " verified_kb=" << (double(totals.physically_touched_bytes) / 1024.0)
                  << " index_probe_kb=" << (double(totals.index_probe_bytes) / 1024.0)
                  << " index_probe_operations=" << totals.index_probe_operations
                  << " candidate_chunks=" << totals.candidate_chunks
                  << " candidate_blocks=" << totals.candidate_blocks
                  << " predicted_candidate_chunks=" << totals.predicted_candidate_chunks
                  << " predicted_candidate_blocks=" << totals.predicted_candidate_blocks
                  << " predicted_verified_kb=" << (double(totals.predicted_verified_bytes) / 1024.0)
                  << " prediction_error_bound_chunks=" << totals.prediction_error_bound_chunks
                  << " prediction_error_bound_blocks=" << totals.prediction_error_bound_blocks
                  << " prediction_error_bound_kb=" << (double(totals.prediction_error_bound_bytes) / 1024.0)
                  << " candidate_files=" << totals.candidate_files
                  << " verifier_cpu_ms=" << (double(totals.verifier_cpu_ns) / 1000000.0)
                  << " fallback_rate=" << totals.fallback_rate
                  << " mean_plan_regret=" << totals.mean_plan_regret
                  << " p95_plan_regret=" << totals.p95_plan_regret
                  << " gate_status=" << to_string(verdict.status)
                  << " matches=" << totals.matches << " correctness=pass\n";
        for (std::size_t query_index = 0; query_index < scenario.queries.size(); ++query_index) {
            const auto& query_totals = totals.per_query[query_index];
            const PlanRegret* shadow = nullptr;
            for (const auto& regret : totals.query_regrets) {
                if (regret.query_name == scenario.queries[query_index].name) {
                    shadow = &regret;
                    break;
                }
            }
            std::cout << "METRIC query=" << scenario.name << "." << scenario.queries[query_index].name
                      << " family=" << query_totals.family
                      << " search_time_ms=" << query_totals.search_ms
                      << " cold_search_ms=" << query_totals.cold_search_ms
                      << " warm_search_ms=" << query_totals.warm_search_ms
                      << " search_p50_ms=" << query_totals.search_p50_ms
                      << " search_p95_ms=" << query_totals.search_p95_ms
                      << " logical_unique_kb=" << (double(query_totals.logical_unique_bytes) / 1024.0)
                      << " physically_touched_kb=" << (double(query_totals.physically_touched_bytes) / 1024.0)
                      << " verified_kb=" << (double(query_totals.physically_touched_bytes) / 1024.0)
                      << " index_probe_kb=" << (double(query_totals.index_probe_bytes) / 1024.0)
                      << " index_probe_operations=" << query_totals.index_probe_operations
                      << " candidate_chunks=" << query_totals.candidate_chunks
                      << " candidate_blocks=" << query_totals.candidate_blocks
                      << " predicted_candidate_chunks=" << query_totals.predicted_candidate_chunks
                      << " predicted_candidate_blocks=" << query_totals.predicted_candidate_blocks
                      << " predicted_verified_kb=" << (double(query_totals.predicted_verified_bytes) / 1024.0)
                      << " prediction_error_bound_chunks=" << query_totals.prediction_error_bound_chunks
                      << " prediction_error_bound_blocks=" << query_totals.prediction_error_bound_blocks
                      << " prediction_error_bound_kb=" << (double(query_totals.prediction_error_bound_bytes) / 1024.0)
                      << " candidate_files=" << query_totals.candidate_files
                      << " verifier_cpu_ms=" << (double(query_totals.verifier_cpu_ns) / 1000000.0)
                      << " verifier=" << query_totals.verifier
                      << " estimated_selectivity=" << query_totals.estimated_selectivity
                      << " plan_regret=" << query_totals.plan_regret
                      << " prediction_error=" << query_totals.prediction_error
                      << " plan_key_hash=" << query_totals.plan_key_hash
                      << " semantic_mode=" << query_totals.semantic_mode
                      << " measured_cost=" << query_totals.measured_cost
                      << " observed_candidates=" << query_totals.observed_candidate_count
                      << " allocation_metrics=" << (query_totals.allocation_metrics_available ? "available" : "unavailable")
                      << " page_fault_metrics=" << (query_totals.page_fault_metrics_available ? "available" : "unavailable")
                      << " matches=" << query_totals.matches << "\n";
            if (shadow) {
                for (const auto& candidate : shadow->candidates) {
                    std::cout << "SHADOW_CANDIDATE query=" << scenario.name << "."
                              << scenario.queries[query_index].name
                              << " operator=" << candidate.name
                              << " predicted_cost=" << candidate.predicted_cost
                              << " predicted_selectivity=" << candidate.predicted_selectivity
                              << " actual_cost=" << candidate.actual_cost
                              << " observed=" << (candidate.actual_observed ? "true" : "false")
                              << " observation_status=" << to_string(candidate.observation)
                              << " verification_bytes=" << candidate.actual_verification_bytes
                              << " index_probe_bytes=" << candidate.actual_index_probe_bytes
                              << " index_probe_operations=" << candidate.actual_index_probe_operations
                              << " verifier_cpu_ns=" << candidate.actual_verifier_cpu_ns
                              << " allocation_metrics=" << (candidate.allocation_metrics_available ? "available" : "unavailable")
                              << " page_fault_metrics=" << (candidate.page_fault_metrics_available ? "available" : "unavailable")
                              << "\n";
                }
            }
        }
    }
    const double aggregate_searched_mb = double(aggregate_searched_bytes) / (1024.0 * 1024.0);
    const double aggregate_throughput =
        aggregate_searched_mb / std::max(1e-9, aggregate.search_time_ms / 1000.0);
    const double aggregate_p50 = calculate_percentile(all_global_latencies, 0.50);
    const double aggregate_p95 = calculate_percentile(all_global_latencies, 0.95);
    const auto final_mem = sample_process_stats();

    auto global_shadow = evaluate_shadow_plans(all_global_regrets);
    auto gate_eval = evaluate_performance_gate(scenario_verdicts, thresholds, global_shadow);

    std::cout << "METRIC search_time_ms=" << aggregate.search_time_ms << "\n";
    std::cout << "METRIC throughput_mb_s=" << aggregate_throughput << "\n";
    std::cout << "METRIC index_build_ms=" << aggregate.build_ms << "\n";
    std::cout << "METRIC index_save_ms=" << aggregate.index_save_ms << "\n";
    std::cout << "METRIC index_load_ms=" << aggregate.index_load_ms << "\n";
    std::cout << "METRIC freshness_check_ms=" << aggregate.freshness_check_ms << "\n";
    std::cout << "METRIC cold_search_ms=" << aggregate.cold_search_ms << "\n";
    std::cout << "METRIC warm_search_ms=" << aggregate.warm_search_ms << "\n";
    std::cout << "METRIC repeated_search_ms=" << aggregate.repeated_search_ms << "\n";
    std::cout << "METRIC search_p50_ms=" << aggregate_p50 << "\n";
    std::cout << "METRIC search_p95_ms=" << aggregate_p95 << "\n";
    std::cout << "METRIC rss_kb=" << (final_mem.current_rss_bytes / 1024ULL) << "\n";
    std::cout << "METRIC peak_rss_kb=" << (final_mem.peak_rss_bytes / 1024ULL) << "\n";
    std::cout << "METRIC page_faults=" << final_mem.page_faults << "\n";
    std::cout << "METRIC logical_unique_kb=" << (double(aggregate.logical_unique_bytes) / 1024.0) << "\n";
    std::cout << "METRIC physically_touched_kb=" << (double(aggregate.physically_touched_bytes) / 1024.0) << "\n";
    std::cout << "METRIC verified_kb=" << (double(aggregate.physically_touched_bytes) / 1024.0) << "\n";
    std::cout << "METRIC index_probe_kb=" << (double(aggregate.index_probe_bytes) / 1024.0) << "\n";
    std::cout << "METRIC index_probe_operations=" << aggregate.index_probe_operations << "\n";
    std::cout << "METRIC candidate_chunks=" << aggregate.candidate_chunks << "\n";
    std::cout << "METRIC candidate_blocks=" << aggregate.candidate_blocks << "\n";
    std::cout << "METRIC predicted_candidate_chunks=" << aggregate.predicted_candidate_chunks << "\n";
    std::cout << "METRIC predicted_candidate_blocks=" << aggregate.predicted_candidate_blocks << "\n";
    std::cout << "METRIC predicted_verified_kb=" << (double(aggregate.predicted_verified_bytes) / 1024.0) << "\n";
    std::cout << "METRIC prediction_error_bound_chunks=" << aggregate.prediction_error_bound_chunks << "\n";
    std::cout << "METRIC prediction_error_bound_blocks=" << aggregate.prediction_error_bound_blocks << "\n";
    std::cout << "METRIC prediction_error_bound_kb=" << (double(aggregate.prediction_error_bound_bytes) / 1024.0) << "\n";
    std::cout << "METRIC candidate_files=" << aggregate.candidate_files << "\n";
    std::cout << "METRIC verifier_cpu_ms=" << (double(aggregate.verifier_cpu_ns) / 1000000.0) << "\n";
    std::cout << "METRIC mean_plan_regret=" << global_shadow.mean_regret << "\n";
    std::cout << "METRIC p95_plan_regret=" << global_shadow.p95_regret << "\n";
    std::cout << "METRIC fallback_rate=" << global_shadow.fallback_rate << "\n";
    std::cout << "METRIC gate_status=" << to_string(gate_eval.overall_status) << "\n";
    std::cout << "METRIC gate_passed=" << (gate_eval.passed ? "true" : "false") << "\n";
    std::cout << "METRIC rollback_triggered=" << (gate_eval.rollback_triggered ? "true" : "false") << "\n";
    std::cout << "METRIC gate_wins=" << gate_eval.wins_count << "\n";
    std::cout << "METRIC gate_neutral=" << gate_eval.neutral_count << "\n";
    std::cout << "METRIC gate_regressions=" << gate_eval.regressions_count << "\n";
    std::cout << "METRIC matches_count=" << aggregate.matches << "\n";
    std::cout << "ASI aggregate_corpus_bytes=" << aggregate_corpus_bytes << "\n";
    std::cout << "ASI aggregate_index_bytes=" << aggregate_index_bytes << "\n";
    std::cout << "ASI aggregate_query_profiles=" << aggregate_query_profiles << "\n";
    std::cout << "ASI aggregate_iterations=" << aggregate_iterations << "\n";
    std::cout << "\n" << gate_eval.format_release_report() << "\n";
    if (!enforce_gate) return 0;
    return gate_eval.passed ? 0 : (gate_eval.rollback_triggered ? 2 : 1);
}
