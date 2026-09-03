#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pergrep {
// Current on-disk snapshot identity. Index::save always emits v7, the portable
// little-endian format. Index::load retains read compatibility with legacy v5/v6
// host-representation snapshots; those legacy formats are not portable across
// differing host byte order or data representation.
inline constexpr std::uint32_t kIndexFormatVersion = 7;
inline constexpr std::uint32_t kIndexFormatSchema = 2;
inline constexpr std::string_view kIndexFormatIdentity = "pergrep-index-format-v7-schema2";
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
    // Maximum number of distinct 4-byte q-gram hash rows probed per query.
    // 0 means auto: use every available query q-gram. Values above the
    // available q-gram count clamp to that count; no hidden low cap applies.
    std::size_t planned_qgrams = 4;
    bool include_hidden = true; // filtering is normally done by the CLI layer.
    bool follow_symlinks = false;
    // On-disk snapshots are always emitted as v7 by Index::save. This option
    // selects whether v7 includes its optional persisted-corpus section. When
    // false (default), save persists filter structures and file metadata; load
    // re-attaches each source file through a read-only mmap provider (with a
    // resident fallback). When true, save also persists raw corpus bytes
    // and load restores content without touching the filesystem. Legacy v5/v6
    // files remain readable as host-representation compatibility formats only.
    bool persist_corpus = false;
};

struct FileInfo {
    std::string path;
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
    bool binary = false;
};

// Cache metadata is deliberately kept separate from the index payload. A
// caller may provide only the fields that define its cache request; unset
// fields are populated on save and treated as wildcards on load.
struct CacheManifest {
    std::optional<std::uint32_t> schema_version;
    std::optional<std::uint64_t> source_identity;
    std::optional<std::string> source_root;
    std::optional<std::uint64_t> selector_identity;
    std::optional<IndexOptions> index_options;
    std::optional<std::uint64_t> transform_identity;
    std::optional<std::uint64_t> corpus_files;
    std::optional<std::uint64_t> corpus_bytes;
    std::optional<std::uint64_t> generation;
};

// M4.2: an appended immutable segment's identity. Unlike CacheManifest (which
// describes a whole on-disk snapshot), a SegmentManifest is carried in memory
// alongside a merged Index and records which documents an append introduced or
// replaced, plus the post-merge totals and generation ownership. On-disk
// segment files are deferred to M4.7 (crash-safe publication).
//
// M4.3: tombstones remove documents from the merged view. A tombstoned path is
// excluded before changed/new documents are applied, so a deleted document is
// never reachable by search and a rename is encoded as tombstone(old) +
// changed{new} (the old path vanishes, the new path appears exactly once).
// Tombstoning a path absent from the base is a harmless no-op, not an error.
struct SegmentManifest {
    std::vector<std::string> paths;      // changed/new document relative paths (UTF-8)
    std::vector<std::string> tombstones; // deleted/renamed-away document relative paths (UTF-8)
    std::uint64_t source_identity = 0;   // fingerprint of the NEW full source set (base + segment)
    std::uint64_t generation = 0;        // monotonic generation counter (reuse root_mtime_ns semantics)
    std::uint64_t corpus_files = 0;      // total files after merge (0 = wildcard)
    std::uint64_t corpus_bytes = 0;      // total corpus bytes after merge (0 = wildcard)
};
// M4.5: bound compaction cost and segment read amplification. There are no
// on-disk segment files yet (deferred to M4.7), so a "segment" is one logical
// accumulated append; "compaction" materializes the accumulated append chain
// into a single fresh base (equivalent to a full from_documents rebuild of the
// merged set). Compaction is triggered when the number of accumulated appended
// segments exceeds kMaxSegmentFanout, or when the accumulated changed/
// replaced/tombstoned bytes exceed kCompactionByteRatio of the live corpus.
inline constexpr std::size_t kMaxSegmentFanout = 64;
inline constexpr double kCompactionByteRatio = 0.25;
// Reflects the accumulated append manifest chain carried in memory alongside a
// merged Index. segment_count is the number of logical segments appends have
// accumulated; appended_bytes is the summed changed/replaced/tombstoned bytes;
// read_amplification is appended_bytes / corpus_bytes (0 when corpus is empty).
struct CompactionStats {
    std::size_t segment_count = 0;
    std::uint64_t appended_bytes = 0;
    double read_amplification = 0.0;
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
    // M4.2: append changed documents as immutable segments. Rebuilds a merged
    // document vector from the base's files (replace-by-path, else append) and
    // materializes an ephemeral resident index over it. The manifest records
    // which documents were introduced/replaced and the post-merge totals.
    static Index append(const Index& base, std::vector<Document> changed,
                        const SegmentManifest& manifest, IndexOptions options = {});
    static Index load(const std::filesystem::path& file);
    static Index load(const std::filesystem::path& file, const CacheManifest& expected);
    void save(const std::filesystem::path& file) const;
    void save(const std::filesystem::path& file, const CacheManifest& manifest) const;

    const std::filesystem::path& root() const noexcept;
    const IndexOptions& options() const noexcept;
    std::span<const FileInfo> files() const noexcept;
    std::uint64_t corpus_bytes() const noexcept;
    std::uint64_t index_bytes() const noexcept;
    bool is_snapshot() const noexcept;
    bool fresh() const;
    // The returned view is borrowed from this Index and remains valid while this Index
    // (or a copy sharing its storage) is alive; callers must not retain it after destruction.
    std::string_view content(std::size_t file_id) const;
    // QO-4 test hook: expose underlying IndexData for cost-model unit tests.
    // Returns nullptr if index is empty. Stable for the lifetime of the Index.
    const void* debug_index_data() const noexcept;
    // M4.5: reflect the accumulated append segment chain (logical segments) and
    // its read amplification. Zero after a fresh full build / from_documents /
    // load and after compaction; bumped by append().
    CompactionStats compaction_stats() const noexcept;
    // M4.5: compaction trigger predicate. True when the accumulated segment
    // count exceeds kMaxSegmentFanout or the accumulated appended bytes exceed
    // kCompactionByteRatio of the live corpus. Declared constants are used; the
    // predicate performs no I/O and is safe to call from any thread.
    static bool should_compact(std::uint64_t corpus_bytes, std::size_t segments,
                               std::uint64_t appended_bytes) noexcept;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    explicit Index(std::shared_ptr<Impl> impl);
    static Index load_impl(const std::filesystem::path& file, const CacheManifest& expected, bool manifest_aware);
    friend class Searcher;
};

// Search execution objective. Exhaustive preserves the complete ordered result set.
// FirstHit permits a cooperative stop after the first ordered result (or first
// selected file result in files-with/without mode). OrderedPrefix permits stopping
// after max_matches while retaining the traversal order.
enum class SearchObjective : std::uint8_t { Exhaustive = 0, FirstHit = 1, OrderedPrefix = 2 };
inline const char* to_string(SearchObjective value) noexcept {
    switch (value) {
        case SearchObjective::Exhaustive: return "exhaustive";
        case SearchObjective::FirstHit: return "first-hit";
        case SearchObjective::OrderedPrefix: return "ordered-prefix";
    }
    return "unknown";
}

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
    // Optional cooperative cancellation hook. It is consulted only at safe
    // candidate/record boundaries; absent means no cancellation is possible.
    std::function<bool()> should_cancel = {};
    SearchObjective objective = SearchObjective::Exhaustive;
};

// M1.3 PlanKey: explicit, deterministic plan input. Captures all semantic
// inputs that influence planning/cost/selection so a plan estimated for one
// contract is never reused for another. Includes:
//   - PatternOptions fields (kind, case_mode, engine, word, line, multiline, dotall, unicode, crlf)
//   - SearchOptions fields (objective, overlapping, invert_match, files_with/without_match,
//     max_matches, record_separator, include_binary, eligible_file_ids)
//   - cancellation callbacks are execution-only and are never part of a plan key
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
    SearchObjective objective = SearchObjective::Exhaustive;
    bool operator==(const PlanKey& o) const noexcept;
    bool operator!=(const PlanKey& o) const noexcept { return !(*this == o); }
    std::uint64_t hash() const noexcept; // deterministic 64-bit FNV-1a
};

PlanKey make_plan_key(const Pattern& pattern, const SearchOptions& search_options,
                      const Index& index, std::uint64_t transformed_input_identity = 0);
PlanKey make_plan_key(const Pattern& pattern, const SearchOptions& search_options,
                      const IndexOptions& index_options, std::uint64_t transformed_input_identity = 0);
// Canonical identity used by shadow reports and workload aggregation. It is
// derived from every PlanKey field (including semantic flags and capabilities)
// and is independent of pointer addresses or execution order.
std::string semantic_mode_key(const PlanKey& key);


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
// verifier_cpu_ns is process CPU time spent in candidate generation and exact
// verification after plan selection. Allocation and page-fault counters are
// C++-only optional metrics: the availability bits are false when the platform
// cannot collect a per-search value, and zero must not be interpreted as measured
// zero work.
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
    std::uint64_t plan_key_hash = 0;
    std::string semantic_mode = {};
    double estimated_selectivity = 0.0;
    double measured_cost = 0.0;
    double estimated_cost = 0.0;
    double plan_regret = 0.0;
    bool verifier_fallback = false;
    // M1.7: execution backend and whether the guarded fixed dispatcher ran.
    // physical_operator is more specific than the public verifier enum: it
    // distinguishes chunk-level from whole-file FixedRareByte scans.
    std::string physical_operator = {};
    bool guarded_dispatch_used = false;
    // Planner predictions are candidate-work units, not corpus-byte
    // occurrence proxies. Bounds are conservative upper bounds widened for
    // legacy hash collisions; observed values may be lower.
    std::uint64_t predicted_candidate_chunks = 0;
    std::uint64_t predicted_candidate_blocks = 0;
    std::uint64_t predicted_verified_bytes = 0;
    std::uint64_t prediction_error_bound_chunks = 0;
    std::uint64_t prediction_error_bound_blocks = 0;
    std::uint64_t prediction_error_bound_bytes = 0;
    std::uint64_t allocation_count = 0;
    std::uint64_t allocation_bytes = 0;
    std::uint64_t page_faults = 0;
    bool allocation_metrics_available = false;
    bool page_fault_metrics_available = false;
    // M1.8 objective-aware execution telemetry. These fields are C++-only; the
    // C ABI continues to expose its historical size-stable statistics prefix.
    std::string objective = "exhaustive";
    std::string candidate_order = "file-id,offset";
    std::string early_stop_reason = {};
    std::uint64_t time_to_first_hit_ns = 0;
    bool first_hit_observed = false;
    bool early_stopped = false;
    bool candidate_order_preserved = true;
    bool cancellation_requested = false;
    // M1.9 q-gram probe telemetry. These fields are C++-only and appended
    // after the historical statistics prefix; the C ABI is unchanged.
    std::uint64_t configured_planned_qgrams = 0;
    std::uint64_t effective_k = 0;
    std::uint64_t selected_qgram_count = 0;
    std::uint64_t selected_qgram_rows = 0;
    std::uint64_t positional_probe_bytes = 0;
    std::uint64_t positional_probe_operations = 0;
    std::uint64_t chunk_probe_bytes = 0;
    std::uint64_t chunk_probe_operations = 0;
    std::string qgram_fallback_reason = "none";
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
    // Explicit observation state prevents estimates for unexecuted alternatives
    // from being mistaken for measurements. actual_observed is retained for
    // source compatibility and is equivalent to Observed when true.
    enum class ObservationStatus : std::uint8_t { Unobserved = 0, Observed = 1, CounterfactualEstimate = 2 };
    ObservationStatus observation = ObservationStatus::Unobserved;
    std::uint64_t actual_index_probe_bytes = 0;
    std::uint64_t actual_index_probe_operations = 0;
    std::uint64_t actual_verification_bytes = 0;
    std::uint64_t actual_verifier_cpu_ns = 0;
    std::uint64_t actual_allocation_count = 0;
    std::uint64_t actual_allocation_bytes = 0;
    std::uint64_t actual_page_faults = 0;
    bool allocation_metrics_available = false;
    bool page_fault_metrics_available = false;
};
inline const char* to_string(PlanCandidateMetrics::ObservationStatus status) noexcept {
    switch (status) {
        case PlanCandidateMetrics::ObservationStatus::Unobserved: return "unobserved";
        case PlanCandidateMetrics::ObservationStatus::Observed: return "observed";
        case PlanCandidateMetrics::ObservationStatus::CounterfactualEstimate: return "counterfactual-estimate";
    }
    return "unknown";
}

struct PlanRegret {
    std::string workload_key;
    std::string semantic_mode;
    std::uint64_t plan_key_hash = 0;
    std::size_t candidate_count = 0;
    std::size_t observed_candidate_count = 0;
    bool observed_fallback_loss = false;
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
struct ShadowPlanGroupMetrics {
    std::string workload_key;
    std::string semantic_mode;
    std::size_t query_count = 0;
    std::size_t observed_query_count = 0;
    std::size_t suboptimal_plan_count = 0;
    double mean_regret = 0.0;
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
    std::size_t observed_query_count = 0;
    std::size_t measured_fallback_loss_count = 0;
    std::vector<ShadowPlanGroupMetrics> groups;
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
