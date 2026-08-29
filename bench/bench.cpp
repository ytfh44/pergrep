#include <pergrep/pergrep.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>
using namespace pergrep;

// Synthetic deterministic corpus generator (~1 MB total corpus)
static std::vector<Document> generate_benchmark_corpus() {
    std::mt19937 rng(1337);
    const std::vector<std::string> dict = {
        "function", "return", "int", "double", "float", "string", "vector",
        "class", "struct", "template", "typename", "public", "private",
        "protected", "virtual", "override", "const", "static", "constexpr",
        "namespace", "using", "include", "pragma", "define", "ifdef", "endif",
        "error", "warning", "info", "debug", "critical", "alert", "panic",
        "connection", "socket", "listener", "buffer", "packet", "stream",
        "request", "response", "header", "payload", "status", "timeout",
        "success", "failure", "retry", "abort", "exception", "handler"
    };

    std::vector<Document> docs;
    docs.reserve(10);

    for (int doc_idx = 0; doc_idx < 10; ++doc_idx) {
        std::string content;
        content.reserve(100 * 1024);
        size_t line_len = 0;
        while (content.size() < 100 * 1024) {
            uint32_t r = rng();
            if (r % 15 == 0) {
                if (r % 60 == 0) content += " 0x" + std::to_string(r & 0xFFFF) + " ";
                else if (r % 45 == 0) content += " ID_" + std::to_string(r % 100000) + " ";
                else if (r % 30 == 0) content += " connection_reset_by_peer ";
                else content += " std::vector<int> ";
            } else {
                content += dict[r % dict.size()] + " ";
            }
            line_len += 10;
            if (line_len >= 80) {
                content += "\n";
                line_len = 0;
            }
        }
        docs.push_back({"doc_" + std::to_string(doc_idx) + ".cpp", std::move(content)});
    }
    return docs;
}

struct BenchCase {
    std::string name;
    std::string pattern;
    PatternOptions popt;
    SearchOptions sopt;
};

int main() {
    auto docs = generate_benchmark_corpus();
    uint64_t total_corpus_bytes = 0;
    for (const auto& d : docs) total_corpus_bytes += d.content.size();

    IndexOptions opt;
    opt.chunk_bytes = 32768;
    opt.chunk_overlap = 128;
    opt.positional_block_bytes = 256;
    opt.positional_budget_ratio = 0.5;

    auto index = Index::from_documents(docs, opt);
    Searcher searcher(index);

    // Reference whole-corpus brute force searcher: single chunk per file.
    // Must respect IndexOptions bounds (chunk_overlap <= chunk_bytes/2, block <= 1MiB).
    IndexOptions ref_opt;
    ref_opt.chunk_bytes = std::min<uint64_t>(uint64_t(1) << 20, total_corpus_bytes + 1024);
    if (ref_opt.chunk_bytes < 64) ref_opt.chunk_bytes = 64;
    ref_opt.chunk_overlap = ref_opt.chunk_bytes / 2;
    ref_opt.positional_block_bytes = 256;
    auto ref_index = Index::from_documents(docs, ref_opt);
    Searcher ref_searcher(ref_index);
    std::vector<BenchCase> cases = {
        // Flavor 1: Multi-literal alternation (Disjunctions)
        {"alt_common", "error|warning|fatal|critical|alert|panic", {}, {}},
        {"alt_protocols", "socket|listener|stream|payload|response", {}, {}},
        {"alt_keywords", "constexpr|override|typename|template", {}, {}},

        // Flavor 2: Fixed prefix / suffix / anchor patterns
        {"prefix_var", "connection_[a-z_]+", {}, {}},
        {"prefix_id", "ID_[0-9]+", {}, {}},
        {"hex_literal", "0x[0-9a-fA-F]{4}", {}, {}},

        // Flavor 3: Pure literal-equivalent regex
        {"lit_scoped", "std::vector", {}, {}},
        {"lit_escaped", "class\\s+struct", {}, {}},

        // Flavor 4: Case-insensitive regex search
        {"icase_term", "connection_reset_by_peer", {.case_mode = CaseMode::Insensitive}, {}},
        {"icase_alt", "error|fatal|warning", {.case_mode = CaseMode::Insensitive}, {}},

        // Flavor 5: Bounded repetitions and character classes
        {"bounded_digits", "[0-9]{4,6}", {}, {}},
        {"word_boundary", "\\bhandler\\b", {}, {}},
        {"line_anchor", "^.*timeout.*$", {}, {}}
    };

    // Warm-up
    for (const auto& c : cases) {
        auto pat = Pattern::compile(c.pattern, c.popt);
        (void)searcher.find(pat, c.sopt);
    }

    // Correctness validation pass: exact agreement between indexed and brute-force
    for (const auto& c : cases) {
        auto pat = Pattern::compile(c.pattern, c.popt);
        auto actual = searcher.find(pat, c.sopt);
        auto expected = ref_searcher.find(pat, c.sopt);
        if (actual.size() != expected.size()) {
            std::cerr << "CORRECTNESS ERROR on case: " << c.name << " expected " << expected.size()
                      << " matches but got " << actual.size() << "\n";
            return 1;
        }
        for (size_t i = 0; i < expected.size(); ++i) {
            if (actual[i].file_id != expected[i].file_id ||
                actual[i].start != expected[i].start ||
                actual[i].end != expected[i].end) {
                std::cerr << "CORRECTNESS MISMATCH on case: " << c.name << " at match " << i << "\n";
                return 1;
            }
        }
    }

    // Benchmark loop (multiple iterations for stable timing)
    const int ITERATIONS = 5;
    uint64_t total_verified_bytes = 0;
    uint64_t total_candidate_chunks = 0;
    uint64_t total_matches = 0;
    // Per-flavor candidate_chunks logging for QO-2 rarity analysis + QO-4 cost model verifier
    std::vector<uint64_t> per_case_candidate_chunks(cases.size(), 0);
    std::vector<std::string> per_case_verifier(cases.size());
    std::vector<double> per_case_selectivity(cases.size(), 0.0);
    std::vector<uint64_t> per_case_blocks(cases.size(), 0);

    auto t0 = std::chrono::steady_clock::now();
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        for (size_t ci = 0; ci < cases.size(); ++ci) {
            const auto& c = cases[ci];
            auto pat = Pattern::compile(c.pattern, c.popt);
            SearchStats stats{};
            auto ms = searcher.find(pat, c.sopt, &stats);
            total_verified_bytes += stats.verified_bytes;
            total_candidate_chunks += stats.candidate_chunks;
            per_case_candidate_chunks[ci] += stats.candidate_chunks;
            per_case_blocks[ci] += stats.candidate_blocks;
            // QO-4 cost model: capture verifier and selectivity chosen by estimateCost/chooseVerifier
            if (iter == 0) {
                per_case_verifier[ci] = stats.verifier.empty() ? "Unknown" : stats.verifier;
                per_case_selectivity[ci] = stats.estimated_selectivity;
            }
            total_matches += ms.size();
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double avg_ms = total_ms / ITERATIONS;
    double corpus_mb = double(total_corpus_bytes) / (1024.0 * 1024.0);
    double total_searched_mb = corpus_mb * cases.size() * ITERATIONS;
    double throughput = total_searched_mb / (total_ms / 1000.0);

    double total_possible_chunks = double(index.corpus_bytes() / opt.chunk_bytes) * cases.size() * ITERATIONS;
    double pruned_pct = 100.0 * (1.0 - std::min(1.0, double(total_candidate_chunks) / std::max(1.0, total_possible_chunks)));

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "METRIC search_time_ms=" << avg_ms << "\n";
    std::cout << "METRIC throughput_mb_s=" << throughput << "\n";
    std::cout << "METRIC pruned_chunks_pct=" << pruned_pct << "\n";
    std::cout << "METRIC verified_kb=" << (double(total_verified_bytes) / 1024.0 / ITERATIONS) << "\n";
    std::cout << "METRIC matches_count=" << (total_matches / ITERATIONS) << "\n";
    std::cout << "ASI cases_count=" << cases.size() << "\n";
    std::cout << "ASI corpus_size_mb=" << corpus_mb << "\n";
    // Per-flavor candidate_chunks (averaged over iterations) for rarity planner visibility + QO-4 verifier
    for (size_t ci = 0; ci < cases.size(); ++ci) {
        uint64_t avg_cc = per_case_candidate_chunks[ci] / ITERATIONS;
        uint64_t avg_blocks = per_case_blocks[ci] / ITERATIONS;
        std::cout << "METRIC candidate_chunks[" << cases[ci].name << "]=" << avg_cc << "\n";
        std::cout << "METRIC candidate_blocks[" << cases[ci].name << "]=" << avg_blocks << "\n";
        std::cout << "METRIC verifier[" << cases[ci].name << "]=" << per_case_verifier[ci] << "\n";
        std::cout << "METRIC selectivity[" << cases[ci].name << "]=" << std::fixed << std::setprecision(6) << per_case_selectivity[ci] << "\n";
        // is_pure_literal dispatch check: lit_scoped / lit_escaped should be Fixed* (pure literal fast path)
        // when (multiline || !contains sep) holds; bench ensures per-flavor candidate_chunks measured.
    }
    return 0;
}
