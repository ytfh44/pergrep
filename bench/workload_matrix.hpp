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

// M0.3 workload contract. Increment only when class or measurement semantics change.
inline constexpr std::uint32_t kWorkloadMatrixVersion = 2;

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

} // namespace pergrep::benchmark
