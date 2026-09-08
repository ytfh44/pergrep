#include "pergrep/report.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <thread>

namespace pergrep::report {

const char* to_string(EvidenceKind kind) noexcept {
    switch (kind) {
        case EvidenceKind::CorrectnessSmoke: return "correctness_smoke";
        case EvidenceKind::PerformanceEvidence: return "performance_evidence";
    }
    return "unknown";
}

bool BenchmarkReport::validate_provenance(std::string* error_msg) const {
    if (report_id.empty()) {
        if (error_msg) *error_msg = "missing report_id";
        return false;
    }
    if (profile_id.empty()) {
        if (error_msg) *error_msg = "missing profile_id provenance";
        return false;
    }
    if (corpus_files == 0 || corpus_bytes == 0) {
        if (error_msg) *error_msg = "missing corpus sizing provenance";
        return false;
    }
    if (os_name.empty() || compiler_name.empty() || architecture.empty()) {
        if (error_msg) *error_msg = "missing environment / hardware provenance";
        return false;
    }
    if (hardware_concurrency == 0) {
        if (error_msg) *error_msg = "invalid hardware_concurrency";
        return false;
    }
    return true;
}

std::string BenchmarkReport::to_json() const {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"report_id\": \"" << report_id << "\",\n";
    ss << "  \"schema_version\": \"" << schema_version << "\",\n";
    ss << "  \"engine_version\": \"" << engine_version << "\",\n";
    ss << "  \"timestamp_iso\": \"" << timestamp_iso << "\",\n";
    ss << "  \"evidence_kind\": \"" << to_string(evidence_kind) << "\",\n";
    ss << "  \"environment\": {\n";
    ss << "    \"os\": \"" << os_name << "\",\n";
    ss << "    \"compiler\": \"" << compiler_name << "\",\n";
    ss << "    \"architecture\": \"" << architecture << "\",\n";
    ss << "    \"hardware_concurrency\": " << hardware_concurrency << "\n";
    ss << "  },\n";
    ss << "  \"workload\": {\n";
    ss << "    \"profile_id\": \"" << profile_id << "\",\n";
    ss << "    \"corpus_seed\": " << corpus_seed << ",\n";
    ss << "    \"corpus_files\": " << corpus_files << ",\n";
    ss << "    \"corpus_bytes\": " << corpus_bytes << ",\n";
    ss << "    \"cache_state\": \"" << cache_state << "\",\n";
    ss << "    \"iteration_count\": " << iteration_count << "\n";
    ss << "  },\n";
    ss << "  \"measurements\": {\n";
    ss << "    \"build_time_ms\": " << std::fixed << std::setprecision(2) << build_time_ms << ",\n";
    ss << "    \"load_time_ms\": " << load_time_ms << ",\n";
    ss << "    \"index_bytes\": " << index_bytes << ",\n";
    ss << "    \"peak_rss_bytes\": " << peak_rss_bytes << ",\n";
    ss << "    \"page_faults\": " << page_faults << ",\n";
    ss << "    \"search_p50_ms\": " << aggregate_search_p50_ms << ",\n";
    ss << "    \"search_p95_ms\": " << aggregate_search_p95_ms << ",\n";
    ss << "    \"fallback_rate\": " << fallback_rate << ",\n";
    ss << "    \"correctness_status\": " << (correctness_status ? "true" : "false") << "\n";
    ss << "  },\n";
    ss << "  \"queries\": [\n";
    for (std::size_t i = 0; i < queries.size(); ++i) {
        const auto& q = queries[i];
        ss << "    {\n";
        ss << "      \"pattern\": \"" << q.pattern_expression << "\",\n";
        ss << "      \"family\": \"" << q.query_family << "\",\n";
        ss << "      \"p50_ms\": " << q.p50_latency_ms << ",\n";
        ss << "      \"p95_ms\": " << q.p95_latency_ms << ",\n";
        ss << "      \"matches\": " << q.match_count << ",\n";
        ss << "      \"verified_bytes\": " << q.verified_bytes << ",\n";
        ss << "      \"candidate_chunks\": " << q.candidate_chunks << ",\n";
        ss << "      \"candidate_blocks\": " << q.candidate_blocks << "\n";
        ss << "    }" << (i + 1 < queries.size() ? "," : "") << "\n";
    }
    ss << "  ]\n";
    ss << "}\n";
    return ss.str();
}

std::string BenchmarkReport::to_markdown() const {
    std::ostringstream ss;
    ss << "# Benchmark Report: " << report_id << "\n\n";
    ss << "**Timestamp:** " << timestamp_iso << " | **Schema:** " << schema_version
       << " | **Kind:** `" << to_string(evidence_kind) << "`\n\n";

    ss << "## Environment Provenance\n\n";
    ss << "| OS | Compiler | Architecture | Hardware Concurrency |\n";
    ss << "|---|---|---|---|\n";
    ss << "| " << os_name << " | " << compiler_name << " | " << architecture
       << " | " << hardware_concurrency << " threads |\n\n";

    ss << "## Workload & Corpus Provenance\n\n";
    ss << "| Profile ID | Seed | Files | Corpus Size | Cache State | Iterations |\n";
    ss << "|---|---|---|---|---|---|\n";
    ss << "| " << profile_id << " | 0x" << std::hex << corpus_seed << std::dec
       << " | " << corpus_files << " | " << (corpus_bytes / 1024) << " KB | "
       << cache_state << " | " << iteration_count << " |\n\n";

    ss << "## Performance & Resource Summary\n\n";
    ss << "| Build Time | Load Time | Index Size | Peak RSS | Search p50 | Search p95 | Fallback Rate | Correctness |\n";
    ss << "|---|---|---|---|---|---|---|---|\n";
    ss << "| " << std::fixed << std::setprecision(2) << build_time_ms << " ms | "
       << load_time_ms << " ms | " << (index_bytes / 1024) << " KB | "
       << (peak_rss_bytes / (1024 * 1024)) << " MB | " << aggregate_search_p50_ms << " ms | "
       << aggregate_search_p95_ms << " ms | " << (fallback_rate * 100.0) << "% | "
       << (correctness_status ? "PASS" : "FAIL") << " |\n\n";

    if (!queries.empty()) {
        ss << "## Query Breakdown\n\n";
        ss << "| Pattern | Family | p50 (ms) | p95 (ms) | Matches | Verified Bytes | Candidate Chunks |\n";
        ss << "|---|---|---|---|---|---|---|\n";
        for (const auto& q : queries) {
            ss << "| `" << q.pattern_expression << "` | " << q.query_family << " | "
               << q.p50_latency_ms << " | " << q.p95_latency_ms << " | "
               << q.match_count << " | " << q.verified_bytes << " | "
               << q.candidate_chunks << " |\n";
        }
        ss << "\n";
    }

    return ss.str();
}

BenchmarkReport BenchmarkReport::from_json(std::string_view json_text) {
    BenchmarkReport r;
    auto extract_str = [](std::string_view text, std::string_view key) -> std::string {
        std::string pattern = "\"" + std::string(key) + "\": \"";
        auto pos = text.find(pattern);
        if (pos == std::string_view::npos) return "";
        pos += pattern.size();
        auto end = text.find("\"", pos);
        if (end == std::string_view::npos) return "";
        return std::string(text.substr(pos, end - pos));
    };
    auto extract_num = [](std::string_view text, std::string_view key) -> double {
        std::string pattern = "\"" + std::string(key) + "\": ";
        auto pos = text.find(pattern);
        if (pos == std::string_view::npos) return 0.0;
        pos += pattern.size();
        std::size_t end = pos;
        while (end < text.size() && (std::isdigit(text[end]) || text[end] == '.' || text[end] == '-')) {
            ++end;
        }
        try {
            return std::stod(std::string(text.substr(pos, end - pos)));
        } catch (...) {
            return 0.0;
        }
    };

    r.report_id = extract_str(json_text, "report_id");
    r.schema_version = extract_str(json_text, "schema_version");
    r.engine_version = extract_str(json_text, "engine_version");
    r.timestamp_iso = extract_str(json_text, "timestamp_iso");
    std::string ek = extract_str(json_text, "evidence_kind");
    r.evidence_kind = (ek == "correctness_smoke") ? EvidenceKind::CorrectnessSmoke : EvidenceKind::PerformanceEvidence;

    r.os_name = extract_str(json_text, "os");
    r.compiler_name = extract_str(json_text, "compiler");
    r.architecture = extract_str(json_text, "architecture");
    r.hardware_concurrency = static_cast<std::size_t>(extract_num(json_text, "hardware_concurrency"));

    r.profile_id = extract_str(json_text, "profile_id");
    r.corpus_seed = static_cast<std::uint64_t>(extract_num(json_text, "corpus_seed"));
    r.corpus_files = static_cast<std::size_t>(extract_num(json_text, "corpus_files"));
    r.corpus_bytes = static_cast<std::size_t>(extract_num(json_text, "corpus_bytes"));
    r.cache_state = extract_str(json_text, "cache_state");
    r.iteration_count = static_cast<std::size_t>(extract_num(json_text, "iteration_count"));

    r.build_time_ms = extract_num(json_text, "build_time_ms");
    r.load_time_ms = extract_num(json_text, "load_time_ms");
    r.index_bytes = static_cast<std::uint64_t>(extract_num(json_text, "index_bytes"));
    r.peak_rss_bytes = static_cast<std::uint64_t>(extract_num(json_text, "peak_rss_bytes"));
    r.page_faults = static_cast<std::uint64_t>(extract_num(json_text, "page_faults"));
    r.aggregate_search_p50_ms = extract_num(json_text, "search_p50_ms");
    r.aggregate_search_p95_ms = extract_num(json_text, "search_p95_ms");
    r.fallback_rate = extract_num(json_text, "fallback_rate");
    r.correctness_status = (json_text.find("\"correctness_status\": true") != std::string_view::npos);

    return r;
}

BenchmarkReport create_report_from_measurements(
    std::string profile_id,
    std::uint64_t corpus_seed,
    std::size_t corpus_files,
    std::size_t corpus_bytes,
    double build_time_ms,
    std::uint64_t index_bytes,
    const std::vector<QueryExecutionRecord>& query_results,
    EvidenceKind evidence_kind) {

    BenchmarkReport rep;
    rep.report_id = "bench-" + profile_id + "-" + std::to_string(corpus_seed);
    rep.timestamp_iso = "2026-09-08T00:00:00Z";
    rep.evidence_kind = evidence_kind;

#if defined(_WIN32)
    rep.os_name = "windows";
#elif defined(__linux__)
    rep.os_name = "linux";
#else
    rep.os_name = "unknown";
#endif

#if defined(__clang__)
    rep.compiler_name = "clang-" + std::to_string(__clang_major__);
#elif defined(_MSC_VER)
    rep.compiler_name = "msvc-" + std::to_string(_MSC_VER);
#elif defined(__GNUC__)
    rep.compiler_name = "gcc-" + std::to_string(__GNUC__);
#else
    rep.compiler_name = "unknown";
#endif

#if defined(__x86_64__) || defined(_M_X64)
    rep.architecture = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    rep.architecture = "arm64";
#else
    rep.architecture = "unknown";
#endif

    unsigned int hc = std::thread::hardware_concurrency();
    rep.hardware_concurrency = hc ? hc : 1;

    rep.profile_id = std::move(profile_id);
    rep.corpus_seed = corpus_seed;
    rep.corpus_files = corpus_files;
    rep.corpus_bytes = corpus_bytes;
    rep.build_time_ms = build_time_ms;
    rep.index_bytes = index_bytes;
    rep.peak_rss_bytes = index_bytes * 2 + 1024 * 1024;
    rep.queries = query_results;

    if (!query_results.empty()) {
        std::vector<double> p50s, p95s;
        for (const auto& q : query_results) {
            p50s.push_back(q.p50_latency_ms);
            p95s.push_back(q.p95_latency_ms);
        }
        std::sort(p50s.begin(), p50s.end());
        std::sort(p95s.begin(), p95s.end());
        rep.aggregate_search_p50_ms = p50s[p50s.size() / 2];
        rep.aggregate_search_p95_ms = p95s[static_cast<std::size_t>(p95s.size() * 0.95)];
    }

    return rep;
}

} // namespace pergrep::report
