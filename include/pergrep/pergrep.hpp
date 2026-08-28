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
    bool fresh() const;
    std::string_view content(std::size_t file_id) const;
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
};

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

struct SearchStats {
    std::uint64_t candidate_chunks = 0;
    std::uint64_t candidate_blocks = 0;
    std::uint64_t verified_bytes = 0;
    std::uint64_t matches = 0;
};

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
