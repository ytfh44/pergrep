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
namespace detail { struct IndexData; struct QueryCost; enum class VerifierKind : std::uint8_t; }

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
    friend detail::QueryCost estimateCost(const Pattern&, const detail::IndexData&);
    friend detail::VerifierKind chooseVerifier(const Pattern&, const detail::IndexData&);
    friend std::string pick_rarest_branch_literal(const std::vector<std::vector<std::string>>&, const detail::IndexData&);
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
