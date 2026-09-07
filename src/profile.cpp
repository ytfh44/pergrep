#include "pergrep/profile.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace pergrep::profile {

const char* to_string(CorpusCategory cat) noexcept {
    switch (cat) {
        case CorpusCategory::Code: return "code";
        case CorpusCategory::Logs: return "logs";
        case CorpusCategory::GeneratedMinified: return "generated-minified";
        case CorpusCategory::UnicodeText: return "unicode-text";
        case CorpusCategory::BinaryHeavy: return "binary-heavy";
        case CorpusCategory::LargeFiles: return "large-files";
        case CorpusCategory::HighDuplication: return "high-duplication";
    }
    return "unknown";
}

const char* to_string(FileSizeDistribution dist) noexcept {
    switch (dist) {
        case FileSizeDistribution::Uniform: return "uniform";
        case FileSizeDistribution::SmallFiles: return "small-files";
        case FileSizeDistribution::MediumFiles: return "medium-files";
        case FileSizeDistribution::Bimodal: return "bimodal";
        case FileSizeDistribution::LongTail: return "long-tail";
    }
    return "unknown";
}

// Minimal, completely deterministic 64-bit LCG / SplitMix-like PRNG.
// Standard C++ mt19937 output can vary across platform implementations.
// A fixed LCG guarantees 100% byte-for-byte reproducibility on all platforms.
class DeterministicRng {
public:
    explicit DeterministicRng(std::uint64_t seed = 0x853c49e6748fea9bULL) : state_(seed ? seed : 1) {}

    std::uint64_t next_u64() noexcept {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return state_;
    }

    std::uint32_t next_u32() noexcept {
        return static_cast<std::uint32_t>(next_u64() >> 32);
    }

    std::size_t next_range(std::size_t min_val, std::size_t max_val) noexcept {
        if (min_val >= max_val) return min_val;
        return min_val + static_cast<std::size_t>(next_u64() % (max_val - min_val + 1));
    }

    double next_double() noexcept {
        return static_cast<double>(next_u64() >> 11) * (1.0 / 9007199254740992.0);
    }

private:
    std::uint64_t state_;
};

double calculate_byte_entropy(std::string_view data) {
    if (data.empty()) return 0.0;

    std::array<std::size_t, 256> counts{};
    for (unsigned char c : data) {
        ++counts[c];
    }

    double entropy = 0.0;
    const double n = static_cast<double>(data.size());
    for (std::size_t count : counts) {
        if (count > 0) {
            double p = static_cast<double>(count) / n;
            entropy -= p * std::log2(p);
        }
    }
    return entropy;
}

double calculate_qgram_entropy(std::string_view data, std::size_t q) {
    if (data.size() < q || q == 0) return 0.0;

    std::unordered_map<std::string_view, std::size_t> counts;
    const std::size_t total_qgrams = data.size() - q + 1;
    for (std::size_t i = 0; i < total_qgrams; ++i) {
        ++counts[data.substr(i, q)];
    }

    double entropy = 0.0;
    const double n = static_cast<double>(total_qgrams);
    for (const auto& [_, count] : counts) {
        double p = static_cast<double>(count) / n;
        entropy -= p * std::log2(p);
    }
    return entropy;
}

CorpusSummary analyze_corpus(const std::vector<Document>& corpus,
                             std::string profile_id,
                             std::string version) {
    CorpusSummary summary;
    summary.profile_id = std::move(profile_id);
    summary.version = std::move(version);
    summary.file_count = corpus.size();

    if (corpus.empty()) return summary;

    summary.min_file_bytes = corpus.front().content.size();
    summary.max_file_bytes = corpus.front().content.size();

    std::string combined_sample;
    std::unordered_set<std::string> doc_contents;
    std::unordered_set<std::string> all_qgrams;

    for (const auto& doc : corpus) {
        const std::size_t sz = doc.content.size();
        summary.total_bytes += sz;
        summary.min_file_bytes = std::min(summary.min_file_bytes, sz);
        summary.max_file_bytes = std::max(summary.max_file_bytes, sz);

        if (!doc_contents.insert(doc.content).second) {
            ++summary.duplicate_document_pairs;
        }

        bool is_binary = false;
        for (unsigned char c : doc.content) {
            if (c == 0) {
                is_binary = true;
                break;
            }
        }
        if (is_binary) {
            ++summary.binary_file_count;
        }

        // Collect sample for overall entropy and unique q-grams (bound memory)
        if (combined_sample.size() < 128 * 1024) {
            combined_sample.append(doc.content.substr(0, std::min<std::size_t>(doc.content.size(), 8192)));
        }

        if (all_qgrams.size() < 50000 && sz >= 4) {
            for (std::size_t i = 0; i + 4 <= sz && all_qgrams.size() < 50000; ++i) {
                all_qgrams.insert(doc.content.substr(i, 4));
            }
        }
    }

    summary.avg_file_bytes = static_cast<double>(summary.total_bytes) / static_cast<double>(corpus.size());
    summary.byte_entropy = calculate_byte_entropy(combined_sample);
    summary.qgram_entropy = calculate_qgram_entropy(combined_sample, 4);
    summary.unique_qgrams = all_qgrams.size();

    return summary;
}

namespace {

std::string generate_code_content(DeterministicRng& rng, std::size_t target_bytes) {
    static const char* keywords[] = {
        "namespace", "template", "typename", "struct", "class", "public:", "private:",
        "virtual", "override", "final", "const", "constexpr", "noexcept", "auto",
        "return", "if", "else", "for", "while", "switch", "case", "default:",
        "std::vector", "std::string", "std::string_view", "std::optional", "std::uint64_t",
        "static_cast", "reinterpret_cast", "std::move", "std::forward", "using"
    };
    static const char* id_prefixes[] = {"parse_", "compute_", "find_", "match_", "verify_", "handle_", "build_"};
    static const char* id_suffixes[] = {"_token", "_state", "_buffer", "_index", "_result", "_context", "_node"};

    std::string out;
    out.reserve(target_bytes + 256);

    out.append("// Generated synthetic benchmark source module\n");
    out.append("#include <vector>\n#include <string>\n#include <cstdint>\n\n");

    std::size_t indent = 0;
    while (out.size() < target_bytes) {
        int choice = static_cast<int>(rng.next_range(0, 9));
        out.append(indent * 4, ' ');

        if (choice == 0) {
            out.append(keywords[rng.next_range(0, sizeof(keywords)/sizeof(keywords[0]) - 1)]);
            out.append(" ");
        } else if (choice <= 2) {
            out.append("auto ");
            out.append(id_prefixes[rng.next_range(0, sizeof(id_prefixes)/sizeof(id_prefixes[0]) - 1)]);
            out.append(id_suffixes[rng.next_range(0, sizeof(id_suffixes)/sizeof(id_suffixes[0]) - 1)]);
            out.append(" = ");
            out.append(std::to_string(rng.next_range(0, 100000)));
            out.append(";\n");
        } else if (choice <= 4) {
            out.append("if (");
            out.append(id_prefixes[rng.next_range(0, sizeof(id_prefixes)/sizeof(id_prefixes[0]) - 1)]);
            out.append(id_suffixes[rng.next_range(0, sizeof(id_suffixes)/sizeof(id_suffixes[0]) - 1)]);
            out.append(" > 0) {\n");
            indent = std::min<std::size_t>(indent + 1, 4);
        } else if (choice == 5) {
            if (indent > 0) --indent;
            out.append("}\n");
        } else if (choice <= 7) {
            out.append("return ");
            out.append(id_prefixes[rng.next_range(0, sizeof(id_prefixes)/sizeof(id_prefixes[0]) - 1)]);
            out.append(id_suffixes[rng.next_range(0, sizeof(id_suffixes)/sizeof(id_suffixes[0]) - 1)]);
            out.append(";\n");
        } else {
            out.append("// Synthetic comment token: ");
            out.append(std::to_string(rng.next_u64()));
            out.append("\n");
        }
    }
    return out;
}

std::string generate_logs_content(DeterministicRng& rng, std::size_t target_bytes) {
    static const char* levels[] = {"INFO", "WARN", "ERROR", "DEBUG", "FATAL"};
    static const char* modules[] = {"auth_service", "storage_engine", "network_io", "query_planner", "cache_lru"};
    static const char* messages[] = {
        "connection established with peer",
        "checkpoint persisted successfully to disk",
        "cache eviction triggered under memory pressure",
        "unexpected status code 503 from downstream service",
        "request timeout after 5000ms waiting for lock"
    };

    std::string out;
    out.reserve(target_bytes + 256);

    std::uint32_t sec = 10000;
    while (out.size() < target_bytes) {
        sec += static_cast<std::uint32_t>(rng.next_range(1, 10));
        out.append("2026-09-08T12:");
        out.append(sec / 60 < 10 ? "0" : "");
        out.append(std::to_string(sec / 60));
        out.append(":");
        out.append(sec % 60 < 10 ? "0" : "");
        out.append(std::to_string(sec % 60));
        out.append(".000Z [");
        out.append(levels[rng.next_range(0, sizeof(levels)/sizeof(levels[0]) - 1)]);
        out.append("] (");
        out.append(modules[rng.next_range(0, sizeof(modules)/sizeof(modules[0]) - 1)]);
        out.append(") ");
        out.append(messages[rng.next_range(0, sizeof(messages)/sizeof(messages[0]) - 1)]);
        out.append(" req_id=REQ_");
        out.append(std::to_string(rng.next_u32()));
        out.append("\n");
    }
    return out;
}

std::string generate_minified_content(DeterministicRng& rng, std::size_t target_bytes) {
    std::string out;
    out.reserve(target_bytes + 256);
    out.append("{\"meta\":{\"version\":\"1.0.0\",\"generated\":true},\"items\":[");

    bool first = true;
    while (out.size() < target_bytes) {
        if (!first) out.append(",");
        first = false;
        out.append("{\"k\":\"tok_");
        out.append(std::to_string(rng.next_range(1, 10000)));
        out.append("\",\"v\":");
        out.append(std::to_string(rng.next_u32()));
        out.append(",\"f\":");
        out.append(rng.next_range(0, 1) ? "true" : "false");
        out.append("}");
    }
    out.append("]}");
    return out;
}

std::string generate_unicode_content(DeterministicRng& rng, std::size_t target_bytes) {
    // UTF-8 multi-byte sample character sequences:
    // CJK, Cyrillic, Accents, Emoji
    static const char* unicode_tokens[] = {
        "\xE4\xB8\x96\xE7\x95\x8C",                 // 世界 (World)
        "\xE6\xA4\x9C\xE7\xB4\xA2",                 // 検索 (Search)
        "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E",     // 日本語 (Japanese)
        "\xE4\xBD\xA0\xE5\xA5\xBD",                 // 你好 (Hello)
        "\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82", // привет (Russian Hello)
        "\xC3\xA9\xC3\xA0\xC3\xAE\xC3\xB6\xC3\xBC", // éàîöü (Accents)
        "\xF0\x9F\x94\x8D",                         // 🔍 (Magnifying glass)
        "\xF0\x9F\x9A\x80",                         // 🚀 (Rocket)
        "regular_latin_identifier",
        "delimiter_separator_space "
    };

    std::string out;
    out.reserve(target_bytes + 256);

    while (out.size() < target_bytes) {
        out.append(unicode_tokens[rng.next_range(0, sizeof(unicode_tokens)/sizeof(unicode_tokens[0]) - 1)]);
        if (rng.next_range(0, 3) == 0) out.append("\n");
        else out.append(" ");
    }
    return out;
}

std::string generate_binary_content(DeterministicRng& rng, std::size_t target_bytes) {
    std::string out;
    out.reserve(target_bytes + 256);

    // Standard magic header
    out.append("PGPK\x01\x00\x00\x00", 8);

    while (out.size() < target_bytes) {
        int mode = static_cast<int>(rng.next_range(0, 3));
        if (mode == 0) {
            // Null-byte runs
            out.append(rng.next_range(4, 32), '\0');
        } else if (mode == 1) {
            // Raw binary bytes (including high values > 127)
            std::size_t len = rng.next_range(8, 64);
            for (std::size_t i = 0; i < len; ++i) {
                out.push_back(static_cast<char>(rng.next_u32() & 0xFF));
            }
        } else {
            // Embedded ASCII string tokens
            out.append("EMBEDDED_TEXT_SEGMENT_");
            out.append(std::to_string(rng.next_u32()));
            out.push_back('\0');
        }
    }
    return out;
}

std::size_t compute_file_size(DeterministicRng& rng, FileSizeDistribution dist, std::size_t avg_target) {
    switch (dist) {
        case FileSizeDistribution::Uniform:
            return avg_target;
        case FileSizeDistribution::SmallFiles:
            return rng.next_range(1024, 16384);
        case FileSizeDistribution::MediumFiles:
            return rng.next_range(16384, 131072);
        case FileSizeDistribution::Bimodal:
            return rng.next_range(0, 9) == 0 ? avg_target * 5 : avg_target / 4;
        case FileSizeDistribution::LongTail: {
            double u = rng.next_double();
            // Simple Pareto-like inverse transform sampling
            double scale = 1024.0;
            double alpha = 1.5;
            double sz = scale / std::pow(1.0 - u * 0.95, 1.0 / alpha);
            return std::clamp<std::size_t>(static_cast<std::size_t>(sz), 1024, avg_target * 4);
        }
    }
    return avg_target;
}

} // namespace

std::vector<Document> generate_profile_corpus(const CorpusProfileSpec& spec) {
    if (spec.file_count == 0) return {};

    DeterministicRng rng(spec.seed);
    const std::size_t avg_target = spec.total_target_bytes / spec.file_count;

    std::vector<Document> documents;
    documents.reserve(spec.file_count);

    std::vector<std::string> base_pool; // For duplication category

    for (std::size_t i = 0; i < spec.file_count; ++i) {
        std::size_t target_sz = compute_file_size(rng, spec.size_distribution, avg_target);
        std::string dir = spec.scope.directory_structure.empty() ? "" :
            spec.scope.directory_structure[rng.next_range(0, spec.scope.directory_structure.size() - 1)] + "/";
        std::string ext = spec.scope.file_extensions.empty() ? ".txt" :
            spec.scope.file_extensions[rng.next_range(0, spec.scope.file_extensions.size() - 1)];
        std::string path = spec.scope.base_directory + "/" + dir + "file_" + std::to_string(i) + ext;

        std::string content;
        switch (spec.category) {
            case CorpusCategory::Code:
                content = generate_code_content(rng, target_sz);
                break;
            case CorpusCategory::Logs:
                content = generate_logs_content(rng, target_sz);
                break;
            case CorpusCategory::GeneratedMinified:
                content = generate_minified_content(rng, target_sz);
                break;
            case CorpusCategory::UnicodeText:
                content = generate_unicode_content(rng, target_sz);
                break;
            case CorpusCategory::BinaryHeavy:
                content = generate_binary_content(rng, target_sz);
                break;
            case CorpusCategory::LargeFiles:
                // Large file expands target size significantly
                content = generate_code_content(rng, target_sz * 4);
                break;
            case CorpusCategory::HighDuplication:
                if (base_pool.size() < 3 || rng.next_range(0, 10) < 3) {
                    content = generate_code_content(rng, target_sz);
                    base_pool.push_back(content);
                } else {
                    // Duplicate or minor mutation of existing file
                    content = base_pool[rng.next_range(0, base_pool.size() - 1)];
                    if (rng.next_range(0, 1)) {
                        content.append("\n// Mutated duplicate suffix token: " + std::to_string(rng.next_u32()) + "\n");
                    }
                }
                break;
        }

        documents.push_back(Document{std::move(path), std::move(content)});
    }

    return documents;
}

CorpusProfileSpec canonical_code_profile(std::size_t scale) {
    CorpusProfileSpec spec;
    spec.profile_id = "pergrep-profile-v1-code";
    spec.version = "1.0.0";
    spec.category = CorpusCategory::Code;
    spec.file_count = 16 * scale;
    spec.total_target_bytes = 256 * 1024 * scale;
    spec.seed = 0xC0DE2026ULL;
    spec.size_distribution = FileSizeDistribution::MediumFiles;
    spec.scope = {"repo", {".cpp", ".hpp", ".h"}, {"src", "include", "tests"}, {"**/*.cpp", "**/*.hpp"}};
    spec.query_distribution = {
        {"rare_function", "compute_result", "literal", 0.05},
        {"common_keyword", "struct|class", "alternation", 0.80},
        {"bounded_regex", "handle_[a-z]+", "bounded_regex", 0.30}
    };
    return spec;
}

CorpusProfileSpec canonical_logs_profile(std::size_t scale) {
    CorpusProfileSpec spec;
    spec.profile_id = "pergrep-profile-v1-logs";
    spec.version = "1.0.0";
    spec.category = CorpusCategory::Logs;
    spec.file_count = 8 * scale;
    spec.total_target_bytes = 512 * 1024 * scale;
    spec.seed = 0x10652026ULL;
    spec.size_distribution = FileSizeDistribution::MediumFiles;
    spec.scope = {"var/log", {".log"}, {"server", "audit", "system"}, {"**/*.log"}};
    spec.query_distribution = {
        {"error_level", "\\[ERROR\\]", "literal", 0.20},
        {"status_code", "status=[0-9]{3}", "bounded_regex", 0.50},
        {"req_id", "req_id=REQ_[0-9]+", "prefix", 0.90}
    };
    return spec;
}

CorpusProfileSpec canonical_minified_profile(std::size_t scale) {
    CorpusProfileSpec spec;
    spec.profile_id = "pergrep-profile-v1-minified";
    spec.version = "1.0.0";
    spec.category = CorpusCategory::GeneratedMinified;
    spec.file_count = 6 * scale;
    spec.total_target_bytes = 128 * 1024 * scale;
    spec.seed = 0x31112026ULL;
    spec.size_distribution = FileSizeDistribution::Uniform;
    spec.scope = {"dist", {".min.json", ".min.js"}, {"bundle", "assets"}, {"**/*.min.*"}};
    spec.query_distribution = {
        {"token_match", "tok_[0-9]+", "bounded_regex", 0.95},
        {"exact_key", "\"meta\":", "literal", 1.0}
    };
    return spec;
}

CorpusProfileSpec canonical_unicode_profile(std::size_t scale) {
    CorpusProfileSpec spec;
    spec.profile_id = "pergrep-profile-v1-unicode";
    spec.version = "1.0.0";
    spec.category = CorpusCategory::UnicodeText;
    spec.file_count = 12 * scale;
    spec.total_target_bytes = 192 * 1024 * scale;
    spec.seed = 0x4E1C2026ULL;
    spec.size_distribution = FileSizeDistribution::SmallFiles;
    spec.scope = {"i18n", {".txt", ".md"}, {"locales", "docs"}, {"**/*.txt", "**/*.md"}};
    spec.query_distribution = {
        {"cjk_search", "\xE6\xA4\x9C\xE7\xB4\xA2", "unicode", 0.30}, // 検索
        {"emoji_rocket", "\xF0\x9F\x9A\x80", "literal", 0.15},       // 🚀
        {"cyrillic", "\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82", "unicode", 0.25} // привет
    };
    return spec;
}

CorpusProfileSpec canonical_binary_heavy_profile(std::size_t scale) {
    CorpusProfileSpec spec;
    spec.profile_id = "pergrep-profile-v1-binary";
    spec.version = "1.0.0";
    spec.category = CorpusCategory::BinaryHeavy;
    spec.file_count = 8 * scale;
    spec.total_target_bytes = 256 * 1024 * scale;
    spec.seed = 0xB1482026ULL;
    spec.size_distribution = FileSizeDistribution::MediumFiles;
    spec.scope = {"bin", {".dat", ".bin", ".pack"}, {"cache", "index"}, {"**/*.dat", "**/*.bin"}};
    spec.query_distribution = {
        {"embedded_string", "EMBEDDED_TEXT_SEGMENT_[0-9]+", "prefix", 0.80},
        {"header_magic", "PGPK", "literal", 1.0}
    };
    return spec;
}

CorpusProfileSpec canonical_large_file_profile(std::size_t scale) {
    CorpusProfileSpec spec;
    spec.profile_id = "pergrep-profile-v1-large";
    spec.version = "1.0.0";
    spec.category = CorpusCategory::LargeFiles;
    spec.file_count = 2 * scale;
    spec.total_target_bytes = 512 * 1024 * scale;
    spec.seed = 0x1A262026ULL;
    spec.size_distribution = FileSizeDistribution::Bimodal;
    spec.scope = {"data", {".big.txt"}, {"blobs"}, {"**/*.big.txt"}};
    spec.query_distribution = {
        {"needle_token", "parse_token", "literal", 1.0},
        {"pattern_rare", "compute_node", "literal", 0.50}
    };
    return spec;
}

CorpusProfileSpec canonical_duplication_profile(std::size_t scale) {
    CorpusProfileSpec spec;
    spec.profile_id = "pergrep-profile-v1-duplication";
    spec.version = "1.0.0";
    spec.category = CorpusCategory::HighDuplication;
    spec.file_count = 14 * scale;
    spec.total_target_bytes = 256 * 1024 * scale;
    spec.seed = 0xD0012026ULL;
    spec.size_distribution = FileSizeDistribution::MediumFiles;
    spec.scope = {"clones", {".cpp"}, {"branch_a", "branch_b"}, {"**/*.cpp"}};
    spec.query_distribution = {
        {"clone_target", "struct|class", "alternation", 0.90},
        {"unique_mutant", "Mutated duplicate suffix", "literal", 0.40}
    };
    return spec;
}

std::vector<CorpusProfileSpec> all_canonical_profiles(std::size_t scale) {
    return {
        canonical_code_profile(scale),
        canonical_logs_profile(scale),
        canonical_minified_profile(scale),
        canonical_unicode_profile(scale),
        canonical_binary_heavy_profile(scale),
        canonical_large_file_profile(scale),
        canonical_duplication_profile(scale)
    };
}

} // namespace pergrep::profile
