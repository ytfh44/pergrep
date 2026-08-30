#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pergrep {
struct PlanKey;
class Pattern;
struct PlanCandidateMetrics;
namespace detail { struct IndexData; struct QueryCost; enum class VerifierKind : std::uint8_t; std::vector<PlanCandidateMetrics> estimate_all_candidate_plans(const Pattern&, const IndexData&, unsigned char record_separator = '\n'); std::vector<PlanCandidateMetrics> estimate_all_candidate_plans(const PlanKey&, const IndexData&); }
enum class PatternKind { Regex, Fixed };
enum class CaseMode { Sensitive, Insensitive, Smart };
enum class Engine { Default, Pcre2Compat, Auto };

struct PatternOptions {
    PatternKind kind = PatternKind::Regex;
    CaseMode case_mode = CaseMode::Sensitive;
    Engine engine = Engine::Default;
    bool word = false;
    bool line = false;
    bool multiline = false;
    bool dotall = false;
    bool unicode = true;
    bool crlf = false;
};

class Pattern {
public:
    Pattern();
    static Pattern compile(std::string expression, PatternOptions options = {});
    const std::string& expression() const noexcept;
    const PatternOptions& options() const noexcept;
    bool is_fixed() const noexcept;
    std::vector<std::string> mandatory_literals() const;
private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
    explicit Pattern(std::shared_ptr<const Impl> impl);
    friend class Searcher;
    friend detail::QueryCost estimateCost(const Pattern&, const detail::IndexData&, unsigned char);
    friend detail::VerifierKind chooseVerifier(const Pattern&, const detail::IndexData&, unsigned char);
    friend std::string pick_rarest_branch_literal(const std::vector<std::vector<std::string>>&, const detail::IndexData&);
    friend std::vector<PlanCandidateMetrics> detail::estimate_all_candidate_plans(const Pattern&, const detail::IndexData&, unsigned char);
    friend detail::QueryCost estimateCost(const PlanKey&, const detail::IndexData&);
    friend detail::VerifierKind chooseVerifier(const PlanKey&, const detail::IndexData&);
    friend std::vector<PlanCandidateMetrics> detail::estimate_all_candidate_plans(const PlanKey&, const detail::IndexData&);
};

struct IndexOptions {
    std::size_t chunk_bytes = 32 * 1024;
    std::size_t chunk_overlap = 128;
    std::size_t positional_block_bytes = 256;
    // Default positional-layer memory ratio (~37.5% of chunk bytes as N-positional rows).
    double positional_budget_ratio = 0.50;
    std::size_t planned_qgrams = 4;
    bool include_hidden = true; // filtering is normally done by the CLI layer.
    bool follow_symlinks = false;
    // QO-5: on-disk corpus prototype. When false (default), Index::save persists
    // only filter structures and file metadata; Index::load re-reads each source
    // file via std::ifstream (O(corpus) I/O) to repopulate I->loaded. This keeps
    // index files small and backward-compatible (v5). When true, save also
    // persists the raw corpus bytes after the positional filter (v6) so load
    // restores content without touching the filesystem — prototype for a true
    // on-disk index that decouples filter persistence from corpus re-read.
    // Default false preserves backward compatibility; true trades larger index
    // for O(1) load without corpus re-read.
    bool persist_corpus = false;
};

struct FileInfo {
    std::string path;
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
    bool binary = false;
};

struct Document {
    std::string path;
    std::string content;
};

class Index {
public:
    Index();
    static Index build(const std::filesystem::path& root, IndexOptions options = {});
    static Index from_documents(std::vector<Document> documents, IndexOptions options = {});
    static Index load(const std::filesystem::path& file);
    void save(const std::filesystem::path& file) const;

    const std::filesystem::path& root() const noexcept;
    const IndexOptions& options() const noexcept;
    std::span<const FileInfo> files() const noexcept;
    std::uint64_t corpus_bytes() const noexcept;
    std::uint64_t index_bytes() const noexcept;
    bool is_snapshot() const noexcept;
    bool fresh() const;
    std::string_view content(std::size_t file_id) const;
    // QO-4 test hook: expose underlying IndexData for cost-model unit tests.
    // Returns nullptr if index is empty. Stable for the lifetime of the Index.
    const void* debug_index_data() const noexcept;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    explicit Index(std::shared_ptr<Impl> impl);
    friend class Searcher;
};

struct SearchOptions {
    bool overlapping = false;
    bool invert_match = false;
    bool files_with_matches = false;
    bool files_without_match = false;
    bool include_binary = false;
    std::uint64_t max_matches = 0; // 0 = unlimited
    unsigned char record_separator = '\n'; // logical record terminator; NUL for rg --null-data
    // Optional eligible file-ID scope. An empty span means all indexed files.
    // Callers must keep the referenced IDs alive for the duration of the search.
    std::span<const std::uint32_t> eligible_file_ids = {};
};

// M1.3 PlanKey: explicit, deterministic plan input. Captures all semantic
// inputs that influence planning/cost/selection so a plan estimated for one
// contract is never reused for another. Includes:
//   - PatternOptions fields (kind, case_mode, engine, word, line, multiline, dotall, unicode, crlf)
//   - SearchOptions fields (overlapping, invert_match, files_with/without_match,
//     max_matches, record_separator, include_binary, eligible_file_ids)
//   - Index capabilities (chunk_bytes, chunk_overlap, positional_block_bytes,
//     positional_budget_ratio, planned_qgrams, include_hidden, follow_symlinks,
//     persist_corpus, pos_block)
//   - transformed-input identity (hash of corpus preprocessing pipeline / encoding).
// No path assumes default newline, non-overlap, or positive matching.
// Equality and hash are deterministic (FNV-64, sorted eligible IDs, bitwise double).
struct PlanKey {
    std::string pattern_expression;
    PatternOptions pattern_options;
    bool overlapping = false;
    bool invert_match = false;
    bool files_with_matches = false;
    bool files_without_match = false;
    bool include_binary = false;
    std::uint64_t max_matches = 0;
    unsigned char record_separator = '\n';
    std::vector<std::uint32_t> eligible_file_ids; // sorted, deduped
    IndexOptions index_options;
    std::uint64_t transformed_input_identity = 0;
    bool operator==(const PlanKey& o) const noexcept;
    bool operator!=(const PlanKey& o) const noexcept { return !(*this == o); }
    std::uint64_t hash() const noexcept; // deterministic 64-bit FNV-1a
};

PlanKey make_plan_key(const Pattern& pattern, const SearchOptions& search_options,
                      const Index& index, std::uint64_t transformed_input_identity = 0);
PlanKey make_plan_key(const Pattern& pattern, const SearchOptions& search_options,
                      const IndexOptions& index_options, std::uint64_t transformed_input_identity = 0);


struct Capture {
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    bool matched = false;
    std::string name;
};

struct Match {
    std::uint32_t file_id = 0;
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    // Group 0 is the whole match; groups 1..N are capturing groups. Fixed-string
    // matches leave this empty to keep their hot path allocation-free.
    std::vector<Capture> captures;
};

// Search accounting uses source-byte offsets, not allocator or OS-I/O events.
// `physically_touched_bytes` is the sum of verifier slices (overlap is counted
// once per slice); `logical_unique_bytes` is the union of those slices within
// each file (overlap and repeated chunk/block lookahead are counted once).
// `index_probe_bytes`/`index_probe_operations` count bytes read and row probes
// against the in-memory candidate indexes. Candidate fields are counts of
// candidates emitted by the corresponding filter stage. `candidate_chunks` and
// `candidate_blocks` retain their legacy names and map directly to the new
// candidate chunk/block counts; `verified_bytes` maps to
// `physically_touched_bytes`. `verifier_cpu_ns` is process CPU time spent in
// candidate generation and exact verification after plan selection. Allocation
// and page-fault timing is intentionally not reported because this API has no
// portable per-search measurement for either event.
// The C API intentionally exposes only its historical four-field prefix until
// a size-aware statistics entry point is added; these extended fields are
// currently C++ and benchmark-only.
struct SearchStats {
    // Legacy fields retain source/header compatibility within this 0.1.0
    // tree; stable binary compatibility belongs to a future versioned stats API.
    std::uint64_t candidate_chunks = 0;
    std::uint64_t candidate_blocks = 0;
    std::uint64_t verified_bytes = 0;
    std::uint64_t matches = 0;
    std::uint64_t logical_unique_bytes = 0;
    std::uint64_t physically_touched_bytes = 0;
    std::uint64_t index_probe_bytes = 0;
    std::uint64_t index_probe_operations = 0;
    std::uint64_t candidate_files = 0;
    std::uint64_t verifier_cpu_ns = 0;
    // QO-4 cost model: which verifier was chosen for this search.
    // Set by Searcher::find(); default is FixedRareByte for fixed literals
    // and RegexBruteForce for regex with no pruning. Used for per-flavor
    // logging in bench/bench.cpp and tests. Values correspond to detail::VerifierKind.
    std::string verifier = {};
    double estimated_selectivity = 0.0;
    double estimated_cost = 0.0;
    double plan_regret = 0.0;
    bool verifier_fallback = false;
};

// QO-4: verifier kinds for the cost-based scheduler. Mirrors detail::VerifierKind
// for public visibility (bench/tests). FixedRareByte covers both whole-file and
// chunk-level rare-byte anchor scans; FixedPositional is the positional Bloom path.
enum class VerifierKind : std::uint8_t { FixedRareByte = 0, FixedPositional = 1, RegexChunk = 2, RegexBruteForce = 3 };
inline const char* to_string(VerifierKind k) noexcept {
    switch (k) {
        case VerifierKind::FixedRareByte: return "FixedRareByte";
        case VerifierKind::FixedPositional: return "FixedPositional";
        case VerifierKind::RegexChunk: return "RegexChunk";
        case VerifierKind::RegexBruteForce: return "RegexBruteForce";
    }
    return "Unknown";
}

struct PlanCandidateMetrics {
    std::string name;
    VerifierKind verifier = VerifierKind::FixedRareByte;
    double predicted_cost = 0.0;
    double predicted_selectivity = 1.0;
    double actual_cost = 0.0;
    double actual_time_ms = 0.0;
    std::uint64_t actual_verified_bytes = 0;
    std::uint64_t actual_candidate_chunks = 0;
    std::uint64_t actual_candidate_blocks = 0;
    bool is_fallback = false;
    bool chosen = false;
    bool actual_observed = false;
};

struct PlanRegret {
    std::string query_name;
    std::string chosen_plan;
    std::string optimal_plan;
    VerifierKind chosen_verifier = VerifierKind::FixedRareByte;
    VerifierKind optimal_verifier = VerifierKind::FixedRareByte;
    double predicted_cost = 0.0;
    double actual_cost = 0.0;
    double optimal_actual_cost = 0.0;
    double absolute_regret = 0.0;
    double relative_regret = 0.0;
    double prediction_error = 0.0;
    bool is_suboptimal = false;
    bool is_fallback = false;
    std::size_t rank_inversions = 0;
    std::vector<PlanCandidateMetrics> candidates;
};

struct ShadowPlanReport {
    std::size_t total_queries = 0;
    std::size_t suboptimal_plan_count = 0;
    std::size_t fallback_count = 0;
    double fallback_rate = 0.0;
    double mean_regret = 0.0;
    double p50_regret = 0.0;
    double p95_regret = 0.0;
    double max_regret = 0.0;
    double mean_prediction_error = 0.0;
    double p95_prediction_error = 0.0;
    double total_excess_cost = 0.0;
    std::vector<PlanRegret> query_regrets;
};

enum class GateStatus : std::uint8_t { Pass = 0, Warn = 1, Fail = 2, Rollback = 3 };
inline const char* to_string(GateStatus status) noexcept {
    switch (status) {
        case GateStatus::Pass: return "PASS";
        case GateStatus::Warn: return "WARN";
        case GateStatus::Fail: return "FAIL";
        case GateStatus::Rollback: return "ROLLBACK";
    }
    return "UNKNOWN";
}

enum class WorkloadClassification : std::uint8_t { Win = 0, Neutral = 1, Regression = 2 };
inline const char* to_string(WorkloadClassification c) noexcept {
    switch (c) {
        case WorkloadClassification::Win: return "WIN";
        case WorkloadClassification::Neutral: return "NEUTRAL";
        case WorkloadClassification::Regression: return "REGRESSION";
    }
    return "UNKNOWN";
}

struct PerformanceGateThresholds {
    double max_search_p50_ms = 100.0;
    double max_search_p95_ms = 300.0;
    double max_search_ms_per_query = 50.0;
    double min_throughput_mb_s = 1.0;

    double max_p50_regression_ratio = 1.05;
    double max_p95_regression_ratio = 1.10;
    double max_search_time_regression_ratio = 1.08;
    double max_memory_regression_ratio = 1.15;

    double max_fallback_rate = 0.25;
    double max_mean_regret_ratio = 0.15;
    double max_p95_regret_ratio = 0.30;
    double max_suboptimal_plan_ratio = 0.20;

    double rollback_p50_regression_ratio = 1.15;
    double rollback_p95_regression_ratio = 1.25;
    double rollback_fallback_rate = 0.50;
    double rollback_mean_regret_ratio = 0.40;
    double rollback_memory_regression_ratio = 1.30;

    bool require_correctness_pass = true;

    static PerformanceGateThresholds default_release_gate() {
        return PerformanceGateThresholds{};
    }

    static PerformanceGateThresholds strict_release_gate() {
        PerformanceGateThresholds t;
        t.max_search_p50_ms = 50.0;
        t.max_search_p95_ms = 150.0;
        t.max_search_ms_per_query = 25.0;
        t.min_throughput_mb_s = 5.0;
        t.max_p50_regression_ratio = 1.02;
        t.max_p95_regression_ratio = 1.05;
        t.max_search_time_regression_ratio = 1.03;
        t.max_memory_regression_ratio = 1.10;
        t.max_fallback_rate = 0.15;
        t.max_mean_regret_ratio = 0.10;
        t.max_p95_regret_ratio = 0.20;
        t.max_suboptimal_plan_ratio = 0.10;
        t.rollback_p50_regression_ratio = 1.10;
        t.rollback_p95_regression_ratio = 1.15;
        t.rollback_fallback_rate = 0.35;
        t.rollback_mean_regret_ratio = 0.25;
        return t;
    }
};

struct ScenarioGateVerdict {
    std::string scenario_name;
    std::string workload_class;
    GateStatus status = GateStatus::Pass;
    WorkloadClassification classification = WorkloadClassification::Neutral;
    double search_time_ms = 0.0;
    double baseline_search_time_ms = 0.0;
    double latency_ratio = 1.0;
    double p50_ms = 0.0;
    double baseline_p50_ms = 0.0;
    double p50_ratio = 1.0;
    double p95_ms = 0.0;
    double baseline_p95_ms = 0.0;
    double p95_ratio = 1.0;
    double fallback_rate = 0.0;
    double mean_regret = 0.0;
    double throughput_mb_s = 0.0;
    double baseline_throughput_mb_s = 0.0;
    std::uint64_t rss_kb = 0;
    std::uint64_t baseline_rss_kb = 0;
    double memory_ratio = 1.0;
    bool correctness_pass = true;
    std::vector<std::string> violations;
    std::vector<std::string> warnings;
};

struct GateEvaluation {
    GateStatus overall_status = GateStatus::Pass;
    bool passed = true;
    bool rollback_triggered = false;
    std::vector<std::string> rollback_reasons;
    std::vector<std::string> failure_reasons;
    std::vector<std::string> warning_reasons;
    std::vector<ScenarioGateVerdict> scenario_verdicts;
    ShadowPlanReport shadow_report;
    std::size_t wins_count = 0;
    std::size_t neutral_count = 0;
    std::size_t regressions_count = 0;

    std::string format_release_report() const;
};

PlanRegret compute_plan_regret(const PlanCandidateMetrics& chosen,
                               const std::vector<PlanCandidateMetrics>& candidates,
                               std::string query_name = "");

ShadowPlanReport evaluate_shadow_plans(const std::vector<PlanRegret>& query_regrets);

GateEvaluation evaluate_performance_gate(
    const std::vector<ScenarioGateVerdict>& scenario_verdicts,
    const PerformanceGateThresholds& thresholds = PerformanceGateThresholds::default_release_gate(),
    const ShadowPlanReport& shadow_report = {});

std::vector<PlanCandidateMetrics> estimate_candidate_plans(const Pattern& pattern, const Index& index, unsigned char record_separator = '\n');
// M1.3 PlanKey overloads: explicit selection/caching contract. Every semantic
// input is part of the key; no default newline/non-overlap/positive-match
// assumption. Distinct keys must not reuse a cached plan; implementations fall
// back to recompute when keys differ.
std::vector<PlanCandidateMetrics> estimate_candidate_plans(const PlanKey& key, const Index& index);
detail::QueryCost estimateCost(const Pattern& p, const detail::IndexData& I, unsigned char record_separator);
detail::VerifierKind chooseVerifier(const Pattern& p, const detail::IndexData& I, unsigned char record_separator);
detail::QueryCost estimateCost(const PlanKey& key, const detail::IndexData& I);
detail::VerifierKind chooseVerifier(const PlanKey& key, const detail::IndexData& I);

class Searcher {
public:
    explicit Searcher(std::shared_ptr<const Index> index);
    explicit Searcher(const Index& index);

    std::vector<Match> find(const Pattern& pattern, SearchOptions options = {}, SearchStats* stats = nullptr) const;
    std::vector<std::uint32_t> files(const Pattern& pattern, SearchOptions options = {}, SearchStats* stats = nullptr) const;
private:
    std::shared_ptr<const Index> owned_;
    const Index* index_ = nullptr;
};

std::string version();

} // namespace pergrep
namespace std {
template<> struct hash<pergrep::PlanKey> {
    size_t operator()(const pergrep::PlanKey& k) const noexcept { return static_cast<size_t>(k.hash()); }
};
} // namespace std
