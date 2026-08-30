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
    double search_ms = 0.0;
    std::uint64_t verified_bytes = 0;
    std::uint64_t candidate_chunks = 0;
    std::uint64_t candidate_blocks = 0;
    std::uint64_t matches = 0;
};

struct RunTotals {
    double search_ms = 0.0;
    double build_ms = 0.0;
    std::uint64_t verified_bytes = 0;
    std::uint64_t candidate_chunks = 0;
    std::uint64_t candidate_blocks = 0;
    std::uint64_t matches = 0;
    std::vector<QueryTotals> per_query;
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
    }
    return true;
}

bool validate_scenario(const WorkloadScenario& scenario, const std::vector<Document>& documents,
                       const IndexOptions& options, const IndexOptions& ref_options) {
    auto indexed = Index::from_documents(documents, options);
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

void add_stats(RunTotals& totals, const SearchStats& stats, std::size_t match_count) {
    totals.verified_bytes += stats.verified_bytes;
    totals.candidate_chunks += stats.candidate_chunks;
    totals.candidate_blocks += stats.candidate_blocks;
    totals.matches += match_count;
}

void add_stats(QueryTotals& totals, const SearchStats& stats, std::size_t match_count,
               double elapsed_ms) {
    totals.search_ms += elapsed_ms;
    totals.verified_bytes += stats.verified_bytes;
    totals.candidate_chunks += stats.candidate_chunks;
    totals.candidate_blocks += stats.candidate_blocks;
    totals.matches += match_count;
}

RunTotals measure_scenario(const WorkloadScenario& scenario, const std::vector<Document>& documents,
                          const IndexOptions& options) {
    std::vector<Pattern> patterns;
    patterns.reserve(scenario.queries.size());
    for (const auto& profile : scenario.queries) {
        patterns.push_back(Pattern::compile(profile.expression, profile.pattern_options));
    }

    RunTotals totals;
    totals.per_query.resize(patterns.size());
    const auto run_queries = [&](const Index& index, RunTotals& run_totals, bool collect_query_metrics) {
        Searcher searcher(index);
        for (std::size_t query_index = 0; query_index < scenario.queries.size(); ++query_index) {
            SearchStats stats{};
            const auto query_start = std::chrono::steady_clock::now();
            const auto matches = searcher.find(patterns[query_index],
                                                scenario.queries[query_index].search_options, &stats);
            const auto query_end = std::chrono::steady_clock::now();
            const double elapsed_ms =
                std::chrono::duration<double, std::milli>(query_end - query_start).count();
            add_stats(run_totals, stats, matches.size());
            if (collect_query_metrics) {
                add_stats(run_totals.per_query[query_index], stats, matches.size(), elapsed_ms);
            }
        }
    };

    // Cold one-shot scenarios include index construction. Other classes build once, warm once,
    // and time only repeated searches.
    if (scenario.include_index_build) {
        for (std::size_t iteration = 0; iteration < scenario.iterations; ++iteration) {
            const auto build_start = std::chrono::steady_clock::now();
            auto index = Index::from_documents(documents, options);
            const auto build_end = std::chrono::steady_clock::now();
            totals.build_ms += std::chrono::duration<double, std::milli>(build_end - build_start).count();

            const auto search_start = std::chrono::steady_clock::now();
            run_queries(index, totals, true);
            const auto search_end = std::chrono::steady_clock::now();
            totals.search_ms += std::chrono::duration<double, std::milli>(search_end - search_start).count();
        }
        return totals;
    }

    const auto build_start = std::chrono::steady_clock::now();
    auto index = Index::from_documents(documents, options);
    const auto build_end = std::chrono::steady_clock::now();
    totals.build_ms = std::chrono::duration<double, std::milli>(build_end - build_start).count();

    // Warm-up is deliberately outside the measured repeated-search interval.
    RunTotals warmup_totals;
    warmup_totals.per_query.resize(patterns.size());
    run_queries(index, warmup_totals, false);
    const auto search_start = std::chrono::steady_clock::now();
    for (std::size_t iteration = 0; iteration < scenario.iterations; ++iteration) {
        run_queries(index, totals, true);
    }
    const auto search_end = std::chrono::steady_clock::now();
    totals.search_ms = std::chrono::duration<double, std::milli>(search_end - search_start).count();
    return totals;
}

} // namespace

int main() {
    const auto matrix = scenarios();
    std::uint64_t aggregate_corpus_bytes = 0;
    std::uint64_t aggregate_searched_bytes = 0;
    RunTotals aggregate;
    std::size_t aggregate_query_profiles = 0;
    std::size_t aggregate_iterations = 0;

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
        if (!validate_scenario(scenario, documents, options, ref_options)) return 1;

        const auto totals = measure_scenario(scenario, documents, options);
        aggregate.search_ms += totals.search_ms;
        aggregate.build_ms += totals.build_ms;
        aggregate.verified_bytes += totals.verified_bytes;
        aggregate.candidate_chunks += totals.candidate_chunks;
        aggregate.candidate_blocks += totals.candidate_blocks;
        aggregate.matches += totals.matches;

        const double measured_searches = double(scenario.queries.size() * scenario.iterations);
        const double search_ms_per_query = totals.search_ms / std::max(1.0, measured_searches);
        const double searched_mb = double(corpus_bytes) * measured_searches / (1024.0 * 1024.0);
        const double throughput = searched_mb / std::max(1e-9, totals.search_ms / 1000.0);

        std::cout << "SCENARIO name=" << scenario.name << " class=" << to_string(scenario.workload_class)
                  << " phase=" << to_string(scenario.phase) << " corpus=" << scenario.corpus.name
                  << " transform=" << to_string(scenario.corpus.transform)
                  << " selector=" << to_string(scenario.selector) << " queries=" << scenario.queries.size()
                  << " iterations=" << scenario.iterations << "\n";
        std::cout << std::fixed << std::setprecision(3);
        std::cout << "METRIC scenario=" << scenario.name << " corpus_bytes=" << corpus_bytes
                  << " index_build_ms=" << totals.build_ms << " search_time_ms=" << totals.search_ms
                  << " search_ms_per_query=" << search_ms_per_query << " throughput_mb_s=" << throughput
                  << " verified_kb=" << (double(totals.verified_bytes) / 1024.0)
                  << " candidate_chunks=" << totals.candidate_chunks
                  << " candidate_blocks=" << totals.candidate_blocks << " matches=" << totals.matches
                  << " correctness=pass\n";
        for (std::size_t query_index = 0; query_index < scenario.queries.size(); ++query_index) {
            const auto& query_totals = totals.per_query[query_index];
            std::cout << "METRIC query=" << scenario.name << "." << scenario.queries[query_index].name
                      << " search_time_ms=" << query_totals.search_ms
                      << " verified_kb=" << (double(query_totals.verified_bytes) / 1024.0)
                      << " candidate_chunks=" << query_totals.candidate_chunks
                      << " candidate_blocks=" << query_totals.candidate_blocks
                      << " matches=" << query_totals.matches << "\n";
        }
    }

    const double aggregate_searched_mb = double(aggregate_searched_bytes) / (1024.0 * 1024.0);
    const double aggregate_throughput = aggregate_searched_mb /
                                        std::max(1e-9, aggregate.search_ms / 1000.0);
    std::cout << "METRIC search_time_ms=" << aggregate.search_ms << "\n";
    std::cout << "METRIC throughput_mb_s=" << aggregate_throughput << "\n";
    std::cout << "METRIC verified_kb=" << (double(aggregate.verified_bytes) / 1024.0) << "\n";
    std::cout << "METRIC candidate_chunks=" << aggregate.candidate_chunks << "\n";
    std::cout << "METRIC candidate_blocks=" << aggregate.candidate_blocks << "\n";
    std::cout << "METRIC matches_count=" << aggregate.matches << "\n";
    std::cout << "ASI aggregate_corpus_bytes=" << aggregate_corpus_bytes << "\n";
    std::cout << "ASI aggregate_query_profiles=" << aggregate_query_profiles << "\n";
    std::cout << "ASI aggregate_iterations=" << aggregate_iterations << "\n";
    return 0;
}
