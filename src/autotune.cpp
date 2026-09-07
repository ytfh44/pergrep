#include "pergrep/autotune.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace pergrep::autotune {

std::string current_toolchain_info() {
    std::string s;
#if defined(__clang__)
    s = "clang-" + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__);
#elif defined(_MSC_VER)
    s = "msvc-" + std::to_string(_MSC_VER);
#elif defined(__GNUC__)
    s = "gcc-" + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__);
#else
    s = "unknown-compiler";
#endif

#if defined(__x86_64__) || defined(_M_X64)
    s += "-x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    s += "-arm64";
#elif defined(__arm__) || defined(_M_ARM)
    s += "-arm";
#else
    s += "-unknown-arch";
#endif

#if defined(_WIN32)
    s += "-windows";
#elif defined(__linux__)
    s += "-linux";
#elif defined(__APPLE__)
    s += "-darwin";
#endif
    return s;
}

SearchResult explore_parameter_space(
    const std::vector<Document>& corpus,
    const std::vector<Pattern>& queries,
    const ParameterBounds& bounds,
    std::string operator_policy) {
    using clk = std::chrono::steady_clock;
    const auto search_start = clk::now();

    SearchResult result;
    result.toolchain_info = current_toolchain_info();

    if (corpus.empty()) return result;

    for (auto chunk_bytes : bounds.chunk_bytes_candidates) {
        for (auto overlap : bounds.chunk_overlap_candidates) {
            if (overlap >= chunk_bytes) {
                ++result.invalid_configurations_skipped;
                continue;
            }
            for (auto block_bytes : bounds.positional_block_bytes_candidates) {
                if (block_bytes > chunk_bytes) {
                    ++result.invalid_configurations_skipped;
                    continue;
                }
                for (auto budget_ratio : bounds.positional_budget_ratio_candidates) {
                    for (auto planned_q : bounds.planned_qgrams_candidates) {
                        // Check evaluation budget
                        if (result.evaluated.size() >= bounds.max_evaluations) {
                            result.budget_exhausted = true;
                            return result;
                        }

                        // Check time budget
                        auto now = clk::now();
                        double elapsed_ms = std::chrono::duration<double, std::milli>(now - search_start).count();
                        if (elapsed_ms >= bounds.max_search_time_budget_ms) {
                            result.budget_exhausted = true;
                            return result;
                        }

                        IndexOptions opt;
                        opt.chunk_bytes = chunk_bytes;
                        opt.chunk_overlap = overlap;
                        opt.positional_block_bytes = block_bytes;
                        opt.positional_budget_ratio = budget_ratio;
                        opt.planned_qgrams = planned_q;

                        TunedCandidate cand;
                        cand.options = opt;
                        cand.toolchain = result.toolchain_info;
                        cand.operator_policy = operator_policy;

                        ++result.total_configurations_explored;

                        try {
                            const auto t0 = clk::now();
                            auto index = Index::from_documents(corpus, opt);
                            const auto t1 = clk::now();
                            cand.build_time_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                            cand.build_bytes = index.index_bytes();

                            Searcher s(index);
                            std::vector<double> latencies;
                            for (const auto& q : queries) {
                                const auto qt0 = clk::now();
                                auto m = s.find(q);
                                const auto qt1 = clk::now();
                                latencies.push_back(std::chrono::duration<double, std::milli>(qt1 - qt0).count());
                            }

                            if (!latencies.empty()) {
                                std::sort(latencies.begin(), latencies.end());
                                cand.search_p50_ms = latencies[latencies.size() / 2];
                                cand.search_p95_ms = latencies[static_cast<std::size_t>(latencies.size() * 0.95)];
                            }

                            // Score: balance search latency (primary) with index size and build time (secondary)
                            cand.score = cand.search_p50_ms * 100.0 + cand.search_p95_ms * 20.0 +
                                         static_cast<double>(cand.build_bytes) / (1024.0 * 1024.0) +
                                         cand.build_time_ms * 0.05;
                            cand.valid = true;
                        } catch (const std::exception& ex) {
                            cand.valid = false;
                            cand.terminated_early = true;
                            cand.termination_reason = ex.what();
                        }

                        if (cand.valid) {
                            if (!result.best.has_value() || cand.score < result.best->score) {
                                result.best = cand;
                            }
                        }
                        result.evaluated.push_back(std::move(cand));
                    }
                }
            }
        }
    }

    return result;
}

} // namespace pergrep::autotune
