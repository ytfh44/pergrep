#pragma once

#include <pergrep/pergrep.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <process.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/resource.h>
#include <unistd.h>
#elif defined(__linux__) || defined(__unix__)
#include <sys/resource.h>
#include <unistd.h>
#endif
namespace pergrep::benchmark {

// M1.5/M1.6: candidate predictions and shadow observation metrics are versioned;
// increment when workload classes or measurement semantics change.
inline constexpr std::uint32_t kWorkloadMatrixVersion = 4;

// The classes are intentionally closed: later milestones may add cases, but must not
// silently change what a class measures.
enum class WorkloadClass { OneShot, WarmRepeated, InteractiveLargeRepository, BatchMultiPattern };
enum class ScenarioPhase { Cold, Warm, Repeated, FilteredScope, TransformedInput };
enum class InputTransform { Native, CrLf, NulRecords };
enum class StorageBackend { InMemory, Filesystem };

enum class ScopeSelector { AllFiles, CppFiles, EveryOtherFile };

struct CorpusProfile {
    std::string name;
    std::size_t document_count = 0;
    std::size_t bytes_per_document = 0;
    std::uint32_t seed = 0;
    InputTransform transform = InputTransform::Native;
};

struct QueryProfile {
    std::string name;
    std::string expression;
    PatternOptions pattern_options{};
    SearchOptions search_options{};
    std::string family;
};

struct WorkloadScenario {
    std::string name;
    WorkloadClass workload_class = WorkloadClass::OneShot;
    ScenarioPhase phase = ScenarioPhase::Cold;
    CorpusProfile corpus;
    std::vector<QueryProfile> queries;
    std::size_t iterations = 1;
    bool include_index_build = false;
    ScopeSelector selector = ScopeSelector::AllFiles;
    StorageBackend storage = StorageBackend::InMemory;
};

inline const char* to_string(StorageBackend value) noexcept {
    switch (value) {
        case StorageBackend::InMemory: return "in-memory";
        case StorageBackend::Filesystem: return "filesystem";
    }
    return "unknown";
}
inline const char* to_string(WorkloadClass value) noexcept {
    switch (value) {
        case WorkloadClass::OneShot: return "one-shot";
        case WorkloadClass::WarmRepeated: return "warm-repeated";
        case WorkloadClass::InteractiveLargeRepository: return "interactive-large-repository";
        case WorkloadClass::BatchMultiPattern: return "batch-multi-pattern";
    }
    return "unknown";
}

inline const char* to_string(ScenarioPhase value) noexcept {
    switch (value) {
        case ScenarioPhase::Cold: return "cold";
        case ScenarioPhase::Warm: return "warm";
        case ScenarioPhase::Repeated: return "repeated";
        case ScenarioPhase::FilteredScope: return "filtered-scope";
        case ScenarioPhase::TransformedInput: return "transformed-input";
    }
    return "unknown";
}

inline const char* to_string(InputTransform value) noexcept {
    switch (value) {
        case InputTransform::Native: return "native";
        case InputTransform::CrLf: return "crlf";
        case InputTransform::NulRecords: return "nul-records";
    }
    return "unknown";
}

inline const char* to_string(ScopeSelector value) noexcept {
    switch (value) {
        case ScopeSelector::AllFiles: return "all-files";
        case ScopeSelector::CppFiles: return "glob:**/*.cpp";
        case ScopeSelector::EveryOtherFile: return "every-other-file";
    }
    return "unknown";
}

inline QueryProfile query(std::string name, std::string expression, std::string family,
                          PatternOptions pattern_options = {}, SearchOptions search_options = {}) {
    return {std::move(name), std::move(expression), pattern_options, search_options, std::move(family)};
}

inline CorpusProfile corpus(std::string name, std::size_t documents, std::size_t bytes,
                            std::uint32_t seed, InputTransform transform = InputTransform::Native) {
    return {std::move(name), documents, bytes, seed, transform};
}

// All query families are represented by the public API. Patterns which need a CLI-only
// feature (for example multiple -e values) are represented as independent QueryProfiles
// in the batch class; the batch runner aggregates them without changing search semantics.
inline std::vector<WorkloadScenario> scenarios() {
    PatternOptions fixed;
    fixed.kind = PatternKind::Fixed;

    PatternOptions icase;
    icase.case_mode = CaseMode::Insensitive;

    PatternOptions multiline_crlf;
    multiline_crlf.multiline = true;
    multiline_crlf.crlf = true;

    PatternOptions nul_records;
    nul_records.multiline = true;

    const auto small = corpus("small-repository", 8, 64 * 1024, 0x0BADC0DEu);
    const auto medium = corpus("medium-repository", 12, 128 * 1024, 0x13579BDFu);
    const auto large = corpus("large-repository", 32, 256 * 1024, 0x2468ACE0u);
    const auto transformed_crlf = corpus("transformed-crlf", 6, 64 * 1024, 0x55AA7733u,
                                         InputTransform::CrLf);
    const auto transformed_nul = corpus("transformed-nul", 6, 64 * 1024, 0x55AA7733u,
                                        InputTransform::NulRecords);

    const auto rare_short = query("rare-short-fixed", "RARE_TOKEN_X9", "rare-short", fixed);
    const auto common_short = query("common-short-regex", "error", "common-short");
    const auto rare_long = query("rare-long-fixed", "RARE_LONG_LITERAL_connection_reset_by_peer_2026",
                                 "rare-long", fixed);
    const auto alternation = query("common-alternation", "error|warning|critical|panic", "alternation");
    const auto bounded = query("bounded-regex", "ID_[0-9]{4,6}", "bounded-regex");
    const auto unbounded = query("unbounded-regex", ".*timeout.*", "unbounded-regex");
    const auto unicode = query("unicode-case-insensitive", "Σίσυφος", "unicode-case-insensitive", icase);
    const auto line = query("multiline-crlf-line", "^timeout=.*$", "multiline-crlf", multiline_crlf);
    const auto nul = query("nul-record", "NUL_SENTINEL", "nul-record", nul_records,
                           [] {
                               SearchOptions options;
                               options.record_separator = '\0';
                               options.include_binary = true;
                               return options;
                           }());
    SearchOptions overlap_options;
    overlap_options.overlapping = true;
    overlap_options.max_matches = 4;
    const auto overlap = query("overlap-with-max", "aba", "overlap-max", fixed, overlap_options);
    const auto prefix = query("interactive-prefix", "connection_[a-z_]+", "prefix");

    return {
        {"oneshot.cold.rare-short", WorkloadClass::OneShot, ScenarioPhase::Cold, small,
         {rare_short}, 1, true, ScopeSelector::AllFiles, StorageBackend::InMemory},
        {"oneshot.filtered-scope.common-and-alternation", WorkloadClass::OneShot,
         ScenarioPhase::FilteredScope, small, {common_short, alternation}, 1, true,
         ScopeSelector::CppFiles, StorageBackend::InMemory},
        {"oneshot.transformed.crlf", WorkloadClass::OneShot, ScenarioPhase::TransformedInput,
         transformed_crlf, {line}, 1, true, ScopeSelector::AllFiles, StorageBackend::InMemory},
        {"oneshot.transformed.nul", WorkloadClass::OneShot, ScenarioPhase::TransformedInput,
         transformed_nul, {nul}, 1, true, ScopeSelector::AllFiles, StorageBackend::InMemory},
        {"warm-repeated.medium.rare-long-unicode", WorkloadClass::WarmRepeated,
         ScenarioPhase::Warm, medium, {rare_long, unicode, bounded}, 8, false, ScopeSelector::AllFiles,
         StorageBackend::InMemory},
        {"interactive.large-repository.filtered", WorkloadClass::InteractiveLargeRepository,
         ScenarioPhase::Repeated, large, {prefix, unbounded, unicode, line}, 3, false,
         ScopeSelector::CppFiles, StorageBackend::InMemory},
        {"batch.multi-pattern.mixed", WorkloadClass::BatchMultiPattern, ScenarioPhase::Repeated,
         medium, {common_short, rare_short, alternation, bounded, overlap, unbounded}, 2, false,
         ScopeSelector::AllFiles, StorageBackend::InMemory},
        {"filesystem.cold.roundtrip", WorkloadClass::OneShot, ScenarioPhase::Cold, small,
         {rare_short, common_short}, 1, true, ScopeSelector::AllFiles, StorageBackend::Filesystem},
        {"filesystem.warm-repeated.medium", WorkloadClass::WarmRepeated, ScenarioPhase::Warm,
         medium, {rare_long, bounded}, 4, false, ScopeSelector::AllFiles, StorageBackend::Filesystem},
    };
}

struct ProcessStats {
    std::uint64_t current_rss_bytes = 0;
    std::uint64_t peak_rss_bytes = 0;
    std::uint64_t page_faults = 0;
    bool supported = false;
};

inline ProcessStats sample_process_stats() {
    ProcessStats stats{};
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (::GetProcessMemoryInfo(::GetCurrentProcess(),
                               reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                               sizeof(pmc))) {
        stats.current_rss_bytes = static_cast<std::uint64_t>(pmc.WorkingSetSize);
        stats.peak_rss_bytes = static_cast<std::uint64_t>(pmc.PeakWorkingSetSize);
        stats.page_faults = static_cast<std::uint64_t>(pmc.PageFaultCount);
        stats.supported = true;
    }
#elif defined(__APPLE__)
    struct rusage ru{};
    if (::getrusage(RUSAGE_SELF, &ru) == 0) {
        stats.peak_rss_bytes = static_cast<std::uint64_t>(ru.ru_maxrss);
        stats.page_faults = static_cast<std::uint64_t>(ru.ru_minflt + ru.ru_majflt);
        stats.supported = true;
    }
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (::task_info(::mach_task_self(), MACH_TASK_BASIC_INFO,
                    reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        stats.current_rss_bytes = static_cast<std::uint64_t>(info.resident_size);
    } else {
        stats.current_rss_bytes = stats.peak_rss_bytes;
    }
#elif defined(__linux__) || defined(__unix__)
    struct rusage ru{};
    if (::getrusage(RUSAGE_SELF, &ru) == 0) {
        stats.peak_rss_bytes = static_cast<std::uint64_t>(ru.ru_maxrss) * 1024ULL;
        stats.page_faults = static_cast<std::uint64_t>(ru.ru_minflt + ru.ru_majflt);
        stats.supported = true;
    }
    std::ifstream statm("/proc/self/statm");
    if (statm) {
        unsigned long size_pages = 0, resident_pages = 0;
        if (statm >> size_pages >> resident_pages) {
            long page_size = ::sysconf(_SC_PAGESIZE);
            if (page_size > 0) {
                stats.current_rss_bytes = static_cast<std::uint64_t>(resident_pages) * static_cast<std::uint64_t>(page_size);
            }
        }
    }
    if (stats.current_rss_bytes == 0) {
        stats.current_rss_bytes = stats.peak_rss_bytes;
    }
#else
    stats.supported = false;
#endif
    return stats;
}

inline double calculate_percentile(std::vector<double> samples, double p) {
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    if (samples.size() == 1) return samples[0];
    const double rank = p * static_cast<double>(samples.size() - 1);
    const std::size_t lower = static_cast<std::size_t>(std::floor(rank));
    const std::size_t upper = static_cast<std::size_t>(std::ceil(rank));
    const double weight = rank - static_cast<double>(lower);
    if (upper >= samples.size()) return samples.back();
    return samples[lower] * (1.0 - weight) + samples[upper] * weight;
}

class TempDirectory {
public:
    explicit TempDirectory(std::string_view prefix) {
        static std::atomic<std::uint64_t> counter{0};
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto id = counter.fetch_add(1);
        path_ = std::filesystem::temp_directory_path() /
                (std::string(prefix) + "_" + std::to_string(now) + "_" + std::to_string(id));
        std::filesystem::create_directories(path_);
    }

    ~TempDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    TempDirectory(TempDirectory&& other) noexcept : path_(std::move(other.path_)) {
        other.path_.clear();
    }
    TempDirectory& operator=(TempDirectory&& other) noexcept {
        if (this != &other) {
            std::error_code ec;
            if (!path_.empty()) std::filesystem::remove_all(path_, ec);
            path_ = std::move(other.path_);
            other.path_.clear();
        }
        return *this;
    }

    const std::filesystem::path& path() const noexcept { return path_; }

    void write_file(const std::string& relative_path, std::string_view content) const {
        const auto full_path = path_ / relative_path;
        if (!full_path.parent_path().empty()) {
            std::filesystem::create_directories(full_path.parent_path());
        }
        std::ofstream out(full_path, std::ios::binary);
        if (!out) {
            throw std::runtime_error("failed to create temp file: " + full_path.string());
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
    }

private:
    std::filesystem::path path_;
};

inline std::vector<Document> generate_corpus(const CorpusProfile& profile) {
    static const std::vector<std::string_view> dictionary = {
        "function", "return", "int", "double", "float", "string", "vector", "class", "struct",
        "template", "typename", "public", "private", "protected", "virtual", "override", "const",
        "static", "constexpr", "namespace", "using", "include", "pragma", "define", "ifdef",
        "endif", "error", "warning", "info", "debug", "critical", "alert", "panic", "connection",
        "socket", "listener", "buffer", "packet", "stream", "request", "response", "header",
        "payload", "status", "timeout", "success", "failure", "retry", "abort", "exception", "handler",
    };

    std::mt19937 rng(profile.seed);
    std::vector<Document> documents;
    documents.reserve(profile.document_count);
    for (std::size_t document_index = 0; document_index < profile.document_count; ++document_index) {
        std::string content;
        content.reserve(profile.bytes_per_document + 256);
        std::size_t line_length = 0;
        while (content.size() < profile.bytes_per_document) {
            const std::uint32_t random = rng();
            if (random % 15 == 0) {
                if (random % 60 == 0) content += " ID_" + std::to_string(random % 100000) + " ";
                else if (random % 45 == 0) content += " connection_reset_by_peer ";
                else content += " std::vector<int> ";
            } else {
                content += dictionary[random % dictionary.size()];
                content.push_back(' ');
            }
            line_length += 10;
            if (line_length >= 80) {
                content.push_back('\n');
                line_length = 0;
            }
        }
        if (document_index == 0) {
            content += "RARE_TOKEN_X9\nRARE_LONG_LITERAL_connection_reset_by_peer_2026\n";
            content += "timeout=connection_reset_by_peer\n";
        }
        if (document_index == 1) content += "Σίσυφος\n";
        if (document_index == 2) content += "NUL_SENTINEL\0tail\n";
        if (document_index == 3) content += "aba\n";
        const char* extension = (document_index % 2 == 0) ? ".cpp" : ".md";
        documents.push_back({"src/file_" + std::to_string(document_index) + extension, std::move(content)});
    }
    return documents;
}

inline std::vector<Document> apply_transform(std::vector<Document> documents, InputTransform transform) {
    if (transform == InputTransform::Native) return documents;
    for (auto& document : documents) {
        std::string transformed;
        transformed.reserve(document.content.size() + document.content.size() / 16);
        for (char byte : document.content) {
            if (byte == '\n' && transform == InputTransform::CrLf) {
                transformed += "\r\n";
            } else if (byte == '\n' && transform == InputTransform::NulRecords) {
                transformed.push_back('\0');
            } else {
                transformed.push_back(byte);
            }
        }
        document.content = std::move(transformed);
    }
    return documents;
}

inline std::vector<Document> materialize_documents(const WorkloadScenario& scenario) {
    auto documents = apply_transform(generate_corpus(scenario.corpus), scenario.corpus.transform);
    if (scenario.selector == ScopeSelector::AllFiles) return documents;

    std::vector<Document> selected;
    selected.reserve(documents.size());
    for (std::size_t index = 0; index < documents.size(); ++index) {
        const bool keep = scenario.selector == ScopeSelector::CppFiles
                              ? documents[index].path.ends_with(".cpp")
                              : (index % 2 == 0);
        if (keep) selected.push_back(std::move(documents[index]));
    }
    return selected;
}

// M0.7 Scenario baseline representation for release regression gates.
struct ScenarioBaseline {
    std::string scenario_name;
    double search_time_ms = 0.0;
    double search_p50_ms = 0.0;
    double search_p95_ms = 0.0;
    double throughput_mb_s = 0.0;
    std::uint64_t rss_kb = 0;
};

inline std::vector<ScenarioBaseline> default_workload_baselines() {
    return {
        {"oneshot.cold.rare-short", 15.0, 1.0, 3.0, 20.0, 16384},
        {"oneshot.filtered-scope.common-and-alternation", 20.0, 2.0, 5.0, 15.0, 16384},
        {"oneshot.transformed.crlf", 18.0, 1.5, 4.0, 18.0, 16384},
        {"oneshot.transformed.nul", 18.0, 1.5, 4.0, 18.0, 16384},
        {"warm-repeated.medium.rare-long-unicode", 30.0, 0.8, 2.5, 40.0, 32768},
        {"interactive.large-repository.filtered", 60.0, 1.5, 4.5, 50.0, 65536},
        {"batch.multi-pattern.mixed", 45.0, 1.2, 3.8, 35.0, 32768},
        {"filesystem.cold.roundtrip", 25.0, 1.8, 4.5, 12.0, 20480},
        {"filesystem.warm-repeated.medium", 35.0, 1.0, 3.0, 35.0, 32768},
    };
}

inline ScenarioGateVerdict evaluate_scenario_gate(
    const WorkloadScenario& scenario,
    double search_time_ms,
    double p50_ms,
    double p95_ms,
    double fallback_rate,
    double mean_regret,
    bool correctness_pass,
    const PerformanceGateThresholds& thresholds,
    const ScenarioBaseline* baseline = nullptr,
    double throughput_mb_s = 0.0,
    std::uint64_t rss_kb = 0) {
    ScenarioGateVerdict verdict;
    verdict.scenario_name = scenario.name;
    verdict.workload_class = to_string(scenario.workload_class);
    verdict.search_time_ms = search_time_ms;
    verdict.p50_ms = p50_ms;
    verdict.p95_ms = p95_ms;
    verdict.fallback_rate = fallback_rate;
    verdict.mean_regret = mean_regret;
    verdict.throughput_mb_s = throughput_mb_s;
    verdict.rss_kb = rss_kb;
    verdict.correctness_pass = correctness_pass;

    if (!correctness_pass) {
        verdict.status = GateStatus::Rollback;
        verdict.classification = WorkloadClassification::Regression;
        verdict.violations.push_back("Correctness check failed (search results mismatch vs reference oracle)");
        return verdict;
    }

    // Absolute threshold enforcement (always enforced)
    if (p50_ms > thresholds.max_search_p50_ms) {
        if (verdict.status != GateStatus::Rollback) verdict.status = GateStatus::Fail;
        verdict.violations.push_back("p50 latency (" + std::to_string(p50_ms) + " ms) exceeds absolute threshold (" +
            std::to_string(thresholds.max_search_p50_ms) + " ms)");
    }
    if (p95_ms > thresholds.max_search_p95_ms) {
        if (verdict.status != GateStatus::Rollback) verdict.status = GateStatus::Fail;
        verdict.violations.push_back("p95 latency (" + std::to_string(p95_ms) + " ms) exceeds absolute threshold (" +
            std::to_string(thresholds.max_search_p95_ms) + " ms)");
    }
    if (!scenario.queries.empty()) {
        const double searches_count = double(scenario.queries.size() * std::max<std::size_t>(1, scenario.iterations));
        const double ms_per_query = search_time_ms / std::max(1.0, searches_count);
        if (ms_per_query > thresholds.max_search_ms_per_query) {
            if (verdict.status != GateStatus::Rollback) verdict.status = GateStatus::Fail;
            verdict.violations.push_back("Search time per query (" + std::to_string(ms_per_query) +
                " ms) exceeds absolute threshold (" + std::to_string(thresholds.max_search_ms_per_query) + " ms)");
        }
    }
    if (throughput_mb_s > 0.0 && throughput_mb_s < thresholds.min_throughput_mb_s) {
        if (verdict.status != GateStatus::Rollback) verdict.status = GateStatus::Fail;
        verdict.violations.push_back("Throughput (" + std::to_string(throughput_mb_s) +
            " MB/s) below minimum threshold (" + std::to_string(thresholds.min_throughput_mb_s) + " MB/s)");
    }

    if (baseline && baseline->search_time_ms > 0.0) {
        verdict.baseline_search_time_ms = baseline->search_time_ms;
        verdict.baseline_p50_ms = baseline->search_p50_ms;
        verdict.baseline_p95_ms = baseline->search_p95_ms;
        verdict.baseline_throughput_mb_s = baseline->throughput_mb_s;
        verdict.baseline_rss_kb = baseline->rss_kb;

        verdict.latency_ratio = search_time_ms / std::max(1e-9, baseline->search_time_ms);
        verdict.p50_ratio = (baseline->search_p50_ms > 0.0) ? (p50_ms / baseline->search_p50_ms) : 1.0;
        verdict.p95_ratio = (baseline->search_p95_ms > 0.0) ? (p95_ms / baseline->search_p95_ms) : 1.0;
        if (baseline->rss_kb > 0 && rss_kb > 0) {
            verdict.memory_ratio = double(rss_kb) / double(baseline->rss_kb);
            if (verdict.memory_ratio > thresholds.rollback_memory_regression_ratio) {
                verdict.status = GateStatus::Rollback;
                verdict.violations.push_back("Memory RSS ratio (" + std::to_string(verdict.memory_ratio) +
                    ") exceeds rollback threshold (" + std::to_string(thresholds.rollback_memory_regression_ratio) + ")");
            } else if (verdict.memory_ratio > thresholds.max_memory_regression_ratio) {
                if (verdict.status != GateStatus::Rollback) verdict.status = GateStatus::Fail;
                verdict.violations.push_back("Memory RSS ratio (" + std::to_string(verdict.memory_ratio) +
                    ") exceeds regression threshold (" + std::to_string(thresholds.max_memory_regression_ratio) + ")");
            }
        }

        // Classify
        if (verdict.p95_ratio < 0.95 && verdict.p50_ratio < 0.95) {
            verdict.classification = WorkloadClassification::Win;
        } else if (verdict.p95_ratio > 1.05 || verdict.p50_ratio > 1.05) {
            verdict.classification = WorkloadClassification::Regression;
        } else {
            verdict.classification = WorkloadClassification::Neutral;
        }

        // Check rollback triggers
        if (verdict.p95_ratio > thresholds.rollback_p95_regression_ratio) {
            verdict.status = GateStatus::Rollback;
            verdict.violations.push_back("p95 latency ratio (" + std::to_string(verdict.p95_ratio) +
                ") exceeds rollback threshold (" + std::to_string(thresholds.rollback_p95_regression_ratio) + ")");
        } else if (verdict.p50_ratio > thresholds.rollback_p50_regression_ratio) {
            verdict.status = GateStatus::Rollback;
            verdict.violations.push_back("p50 latency ratio (" + std::to_string(verdict.p50_ratio) +
                ") exceeds rollback threshold (" + std::to_string(thresholds.rollback_p50_regression_ratio) + ")");
        } else if (verdict.p95_ratio > thresholds.max_p95_regression_ratio) {
            if (verdict.status != GateStatus::Rollback) verdict.status = GateStatus::Fail;
            verdict.violations.push_back("p95 latency ratio (" + std::to_string(verdict.p95_ratio) +
                ") exceeds regression threshold (" + std::to_string(thresholds.max_p95_regression_ratio) + ")");
        } else if (verdict.p50_ratio > thresholds.max_p50_regression_ratio) {
            if (verdict.status != GateStatus::Rollback) verdict.status = GateStatus::Fail;
            verdict.violations.push_back("p50 latency ratio (" + std::to_string(verdict.p50_ratio) +
                ") exceeds regression threshold (" + std::to_string(thresholds.max_p50_regression_ratio) + ")");
        } else if (verdict.latency_ratio > thresholds.max_search_time_regression_ratio) {
            if (verdict.status != GateStatus::Rollback) verdict.status = GateStatus::Fail;
            verdict.violations.push_back("Total search time ratio (" + std::to_string(verdict.latency_ratio) +
                ") exceeds regression threshold (" + std::to_string(thresholds.max_search_time_regression_ratio) + ")");
        }
    }

    // Fallback rate checks
    if (fallback_rate > thresholds.rollback_fallback_rate) {
        verdict.status = GateStatus::Rollback;
        verdict.violations.push_back("Fallback rate (" + std::to_string(fallback_rate * 100.0) +
            "%) exceeds rollback threshold (" + std::to_string(thresholds.rollback_fallback_rate * 100.0) + "%)");
    } else if (fallback_rate > thresholds.max_fallback_rate) {
        if (verdict.status != GateStatus::Rollback) verdict.status = GateStatus::Fail;
        verdict.violations.push_back("Fallback rate (" + std::to_string(fallback_rate * 100.0) +
            "%) exceeds threshold (" + std::to_string(thresholds.max_fallback_rate * 100.0) + "%)");
    }

    // Mean regret checks
    if (mean_regret > thresholds.rollback_mean_regret_ratio) {
        verdict.status = GateStatus::Rollback;
        verdict.violations.push_back("Mean relative plan regret (" + std::to_string(mean_regret * 100.0) +
            "%) exceeds rollback threshold (" + std::to_string(thresholds.rollback_mean_regret_ratio * 100.0) + "%)");
    } else if (mean_regret > thresholds.max_mean_regret_ratio) {
        if (verdict.status != GateStatus::Rollback) verdict.status = GateStatus::Fail;
        verdict.violations.push_back("Mean relative plan regret (" + std::to_string(mean_regret * 100.0) +
            "%) exceeds threshold (" + std::to_string(thresholds.max_mean_regret_ratio * 100.0) + "%)");
    }

    return verdict;
}

} // namespace pergrep::benchmark
