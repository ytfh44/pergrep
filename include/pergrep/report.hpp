#pragma once

#include "pergrep/pergrep.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pergrep::report {

// Evidence classification distinguishing correctness checks from empirical perf data (M9.6)
enum class EvidenceKind {
    CorrectnessSmoke,    // Smoke verification pass
    PerformanceEvidence  // Formally benchmarked performance evidence
};

const char* to_string(EvidenceKind kind) noexcept;

// Query metric summary in report
struct QueryExecutionRecord {
    std::string pattern_expression;
    std::string query_family;
    double p50_latency_ms = 0.0;
    double p95_latency_ms = 0.0;
    std::size_t match_count = 0;
    std::uint64_t verified_bytes = 0;
    std::uint64_t candidate_chunks = 0;
    std::uint64_t candidate_blocks = 0;
};

// Machine-readable and human-readable benchmark report with full provenance (M9.6)
struct BenchmarkReport {
    std::string report_id;
    std::string schema_version = "pergrep-bench-report-v1";
    std::string engine_version = "0.1.0";
    std::string timestamp_iso;
    EvidenceKind evidence_kind = EvidenceKind::PerformanceEvidence;

    // Environment & hardware provenance
    std::string os_name;
    std::string compiler_name;
    std::string architecture;
    std::size_t hardware_concurrency = 1;

    // Workload & corpus provenance
    std::string profile_id;
    std::uint64_t corpus_seed = 0;
    std::size_t corpus_files = 0;
    std::size_t corpus_bytes = 0;
    std::string cache_state = "cold"; // "cold", "warm", "resident"
    std::size_t iteration_count = 1;

    // Build and resource measurements
    double build_time_ms = 0.0;
    double load_time_ms = 0.0;
    std::uint64_t index_bytes = 0;
    std::uint64_t peak_rss_bytes = 0;
    std::uint64_t page_faults = 0;

    // Aggregate search metrics
    double aggregate_search_p50_ms = 0.0;
    double aggregate_search_p95_ms = 0.0;
    double fallback_rate = 0.0;
    bool correctness_status = true;

    std::vector<QueryExecutionRecord> queries;

    // Serializes report to formatted JSON (machine-readable)
    std::string to_json() const;

    // Serializes report to human-readable Markdown table with provenance summary
    std::string to_markdown() const;

    // Validates that report has complete workload and environment provenance
    bool validate_provenance(std::string* error_msg = nullptr) const;

    // Deserializes report from JSON representation
    static BenchmarkReport from_json(std::string_view json_text);
};

// Builder to produce a validated benchmark report
BenchmarkReport create_report_from_measurements(
    std::string profile_id,
    std::uint64_t corpus_seed,
    std::size_t corpus_files,
    std::size_t corpus_bytes,
    double build_time_ms,
    std::uint64_t index_bytes,
    const std::vector<QueryExecutionRecord>& query_results,
    EvidenceKind evidence_kind = EvidenceKind::PerformanceEvidence);

} // namespace pergrep::report
