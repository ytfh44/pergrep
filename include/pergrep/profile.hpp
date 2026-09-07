#pragma once

#include "pergrep/pergrep.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pergrep::profile {

// Categories of real-world search workloads (M9.2)
enum class CorpusCategory {
    Code,
    Logs,
    GeneratedMinified,
    UnicodeText,
    BinaryHeavy,
    LargeFiles,
    HighDuplication
};

const char* to_string(CorpusCategory cat) noexcept;

// Distribution of file sizes
enum class FileSizeDistribution {
    Uniform,
    SmallFiles,   // 1KB - 16KB
    MediumFiles,  // 16KB - 128KB
    Bimodal,      // Tiny files + occasional huge files
    LongTail      // Power-law distribution
};

const char* to_string(FileSizeDistribution dist) noexcept;

// File scope and path distribution
struct ScopeProfile {
    std::string base_directory = "repo";
    std::vector<std::string> file_extensions = {".cpp", ".hpp", ".h", ".c"};
    std::vector<std::string> directory_structure = {"src", "include", "tests", "docs", "third_party"};
    std::vector<std::string> path_globs = {"**/*.cpp", "**/*.hpp", "**/*"};
};

// Query characteristics for profiling
struct QueryTemplate {
    std::string name;
    std::string pattern;
    std::string family; // "literal", "prefix", "suffix", "alternation", "bounded_regex", "unicode", "pathological"
    double expected_selectivity = 0.01; // Expected match frequency (fraction of files)
};

// Complete specification for a reproducible corpus profile
struct CorpusProfileSpec {
    std::string profile_id;
    std::string version = "1.0.0";
    CorpusCategory category = CorpusCategory::Code;
    std::size_t file_count = 10;
    std::size_t total_target_bytes = 64 * 1024;
    std::uint64_t seed = 0x12345678ULL;
    FileSizeDistribution size_distribution = FileSizeDistribution::MediumFiles;
    ScopeProfile scope;
    std::vector<QueryTemplate> query_distribution;
    double target_byte_entropy = 4.5;
    double target_qgram_entropy = 8.0;
};

// Statistics calculated over a generated or existing corpus
struct CorpusSummary {
    std::string profile_id;
    std::string version;
    std::size_t file_count = 0;
    std::size_t total_bytes = 0;
    std::size_t min_file_bytes = 0;
    std::size_t max_file_bytes = 0;
    double avg_file_bytes = 0.0;
    double byte_entropy = 0.0;
    double qgram_entropy = 0.0;
    std::size_t unique_qgrams = 0;
    std::size_t duplicate_document_pairs = 0;
    std::size_t binary_file_count = 0;
};

// Entropy calculation helpers
double calculate_byte_entropy(std::string_view data);
double calculate_qgram_entropy(std::string_view data, std::size_t q = 4);

// Summarize an in-memory corpus
CorpusSummary analyze_corpus(const std::vector<Document>& corpus,
                             std::string profile_id = "custom",
                             std::string version = "1.0.0");

// Deterministic procedural generation of corpus documents based on spec.
// Zero network calls, completely reproducible across compilers, architectures, and OS platforms.
std::vector<Document> generate_profile_corpus(const CorpusProfileSpec& spec);

// Canonical predefined profile factories (version 1.0.0)
CorpusProfileSpec canonical_code_profile(std::size_t scale = 1);
CorpusProfileSpec canonical_logs_profile(std::size_t scale = 1);
CorpusProfileSpec canonical_minified_profile(std::size_t scale = 1);
CorpusProfileSpec canonical_unicode_profile(std::size_t scale = 1);
CorpusProfileSpec canonical_binary_heavy_profile(std::size_t scale = 1);
CorpusProfileSpec canonical_large_file_profile(std::size_t scale = 1);
CorpusProfileSpec canonical_duplication_profile(std::size_t scale = 1);

// Return all canonical profiles
std::vector<CorpusProfileSpec> all_canonical_profiles(std::size_t scale = 1);

} // namespace pergrep::profile
