#pragma once
#include "pergrep/pergrep.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace pergrep::detail {

inline std::uint32_t hash4(const unsigned char* p) noexcept {
    std::uint32_t x = std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
                      (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
inline unsigned char fold_ascii(unsigned char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}
inline std::uint32_t qgram4_key(const unsigned char* p) noexcept {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

struct QueryDesc {
    std::vector<std::uint32_t> hashes;
    std::array<std::vector<std::pair<std::uint16_t,std::uint64_t>>,8> classes;
};
std::uint8_t lg_for(std::size_t n);
QueryDesc compile_qgram_query(std::string_view q);
QueryDesc compile_qgram_query(std::string_view q, std::span<const std::uint32_t> selected_hashes);
struct PosDesc {
    std::uint64_t off = 0;
    std::uint16_t m = 0;
    std::uint32_t mask_bytes = 0;
    std::uint32_t blocks = 0;
};
struct Chunk {
    std::uint32_t file_id = 0;
    std::uint64_t core_begin = 0;
    std::uint64_t core_end = 0;
    std::uint64_t ext_end = 0;
};
struct LoadedFile { FileInfo info; std::string data; };

struct UnicodeProperty {
    enum class Kind : std::uint8_t { GeneralCategory, GeneralGroup, Script, Binary, Alphabetic, WhiteSpace, Word, DecimalDigit, AsciiDigit, AsciiWord, AsciiSpace };
    Kind kind = Kind::Binary;
    std::int32_t value = 0;
    bool negated = false;
};
struct CharClassSpec {
    std::vector<std::pair<std::uint32_t,std::uint32_t>> ranges;
    std::vector<UnicodeProperty> properties;
    bool negated = false;
};
struct RegexNode {
    enum class Kind { Empty, Literal, Dot, Class, Begin, End, AbsBegin, AbsEnd, EndNewline, WordBoundary, WordStartHalf, WordEndHalf, Concat, Alt, Repeat, Group, BackRef, LookAhead, LookBehind };
    Kind kind = Kind::Empty;
    std::string literal;
    CharClassSpec char_class;
    std::array<std::uint64_t,4> cls{}; // fast ASCII class path
    bool cls_neg = false;
    std::string group_name;
    bool negative = false;
    bool greedy = true;
    bool icase = false;
    bool dotall = false;
    bool multiline = false;
    bool unicode = true;
    bool crlf = false;
    std::size_t min = 0, max = 0;
    int group = 0;
    std::vector<std::shared_ptr<RegexNode>> children;
};
struct NfaInst {
    enum class Op : std::uint8_t { Rune, Any, Class, Split, Jmp, SaveStart, SaveEnd, AssertBegin, AssertEnd, AssertAbsBegin, AssertAbsEnd, AssertEndNewline, AssertWord, AssertWordStartHalf, AssertWordEndHalf, Match };
    Op op = Op::Match;
    std::uint32_t rune = 0;
    std::shared_ptr<const CharClassSpec> char_class;
    std::int32_t x = -1, y = -1;
    std::int32_t group = 0;
    bool negative = false;
    bool icase = false;
    bool dotall = false;
    bool multiline = false;
    bool unicode = true;
    bool crlf = false;
};
// FilterExpr — a filter-only Boolean algebra. It is deliberately separate from
// RegexNode: these expressions are only necessary conditions for candidate
// pruning and must never be used to produce matches or mutate regex execution.
//
// Atom(literal) means the candidate contains the exact, non-empty, sensitive
// literal bytes. And requires every child; Or requires at least one child;
// True imposes no restriction. A filter may therefore retain false positives,
// but must never reject text containing an exact regex match.
struct FilterExpr {
    struct True {};
    struct Atom { std::string literal; };
    struct And { std::vector<FilterExpr> terms; };
    struct Or { std::vector<FilterExpr> terms; };

    std::variant<True, Atom, And, Or> value = True{};

    FilterExpr() = default;
    explicit FilterExpr(True x) : value(std::move(x)) {}
    explicit FilterExpr(Atom x) : value(std::move(x)) {}
    explicit FilterExpr(And x) : value(std::move(x)) {}
    explicit FilterExpr(Or x) : value(std::move(x)) {}

    static FilterExpr atom(std::string literal);
    static FilterExpr and_(std::vector<FilterExpr> terms);
    static FilterExpr or_(std::vector<FilterExpr> terms);
    static FilterExpr true_() { return FilterExpr(True{}); }

    // Evaluate only the necessary-condition filter, never the regex itself.
    bool matches(std::string_view candidate) const;
    // Apply Boolean identities, flattening, duplicate elimination, and safe
    // absorption (for example Atom("a") OR (Atom("a") AND Atom("b"))).
    FilterExpr simplified() const;
};

FilterExpr query_filter(const std::shared_ptr<RegexNode>& n);

// QueryIR — optimizer-facing literal/branch view derived from the regex AST.
// Formalizes the "query plan" extracted for candidate pruning / NFA fast-path.
// Fields are conservative (zero false negatives): pruning may keep extra chunks
// but must never discard a true match.
struct QueryIR {
    // Filter-only algebra exposed for conservative candidate pruning. The
    // current planner may continue using derived views below; exact regex
    // execution remains owned by RegexNode/NFA/VM and never reads this tree.
    FilterExpr filter;
    // mandatory: intersection of mandatory literals across all Alt branches.
    // Used as the global chunk filter when branch_mandatory is not available.
    // Example: `foo|foobar` -> intersection {foo}. Sorted longest-first.
    std::vector<std::string> mandatory;
    // branch_mandatory: per-branch mandatory lists for union pruning.
    // Each entry is the mandatory() set for one Alt branch. Empty overall
    // means conservative fallback (at least one branch has no mandatory
    // literal, so union pruning is disabled and search falls back to
    // the global `mandatory` intersection).
    // Example: `foo|bar` -> [[foo],[bar]]; `foo|.*` -> [] (conservative).
    // Only top-level Alt (after unwrapping outer Group nodes) is
    // decomposed; Concat containing Alt falls back to global mandatory.
    // Documented limitation: deeper nesting is not split.
    std::vector<std::vector<std::string>> branch_mandatory;
    // prefixes: per-branch literal prefixes for NFA jump optimization.
    // Used by nfa_search to skip to the next possible match offset.
    // Only case-sensitive literals contribute; icase literals yield empty.
    // Unwraps outer Group and Repeat with min>0 to reach the real prefix.
    std::vector<std::string> prefixes;
    // is_pure_literal + exact_literal: word/line-agnostic literal equivalence.
    // True when the regex is equivalent to a single fixed string (Concat of
    // case-sensitive Literals, optionally wrapped in a single Group).
    // The search layer may dispatch such patterns to the fixed-string path
    // provided `extended==false`, `case_mode` is sensitive, and word/line
    // flags do not affect the literal check. `exact_literal` is valid only
    // when `is_pure_literal` is true.
    bool is_pure_literal = false;
    std::string exact_literal;
};

// Internal contract for one exact verifier invocation. All coordinates are
// source bytes (not Unicode code points) and all intervals are half-open.
// `source` is a non-owning view whose byte zero is `source_begin`; callers
// own/keep the backing storage alive for the duration of verification.
// `record_begin/end` delimit the logical record (the separator and a CRLF
// terminator are outside it). `candidate_begin/end` is the planned, half-open
// range of start offsets to attempt; its end may be record_end + 1 so the
// empty match at the record end remains a valid candidate. M2.3 can additionally
// bound visible bytes to a proven execution region; otherwise bytes visible to an
// attempted match are the full source. Source/record bounds, context-availability flags, separator
// and CRLF policy are authoritative; derived views are convenience helpers.
struct VerifierContext {
    std::string_view source;
    std::uint64_t source_begin = 0;
    std::uint64_t source_end = 0;
    std::uint64_t record_begin = 0;
    std::uint64_t record_end = 0;
    std::uint64_t candidate_begin = 0;
    std::uint64_t candidate_end = 0;
    bool left_context_available = false;
    bool right_context_available = false;
    unsigned char separator = '\n';
    bool crlf = false;
    // Optional execution region. A zero/zero pair keeps the historical full-source visibility;
    // a non-empty pair bounds bytes visible to the verifier while all returned coordinates remain absolute.
    std::uint64_t region_begin = 0;
    std::uint64_t region_end = 0;
    bool bounded_region = false;

    bool validate() const noexcept {
        if (source_end < source_begin || source_end - source_begin != source.size()) return false;
        if (record_begin < source_begin || record_end < record_begin || record_end > source_end) return false;
        if (candidate_begin < record_begin || candidate_end < candidate_begin) return false;
        if (record_end == std::numeric_limits<std::uint64_t>::max()) return false;
        if (candidate_end > record_end + 1) return false;
        if (bounded_region && (region_begin < source_begin || region_end < region_begin || region_end > source_end)) return false;
        return true;
    }
    std::string_view record_view() const noexcept {
        if (!validate()) return {};
        return source.substr(static_cast<std::size_t>(record_begin - source_begin),
                             static_cast<std::size_t>(record_end - record_begin));
    }
    bool region_contains(std::uint64_t absolute) const noexcept {
        if (!bounded_region) return contains(absolute);
        return absolute >= region_begin && absolute < region_end;
    }
    bool contains(std::uint64_t absolute) const noexcept {
        return absolute >= source_begin && absolute < source_end;
    }
};

// Pure extraction helpers (conservative, no false negatives). Exposed for
// testing and for analyze_query(). Each operates on the AST subtree only.
std::vector<std::string> query_mandatory(const std::shared_ptr<RegexNode>& n);
std::vector<std::string> query_prefixes(const std::shared_ptr<RegexNode>& n);
std::vector<std::vector<std::string>> query_branch_mandatory(const std::shared_ptr<RegexNode>& n);
bool query_is_pure_literal(const std::shared_ptr<RegexNode>& n, std::string& out);
QueryIR analyze_query(const std::shared_ptr<RegexNode>& ast, bool extended);
// M2.2 conservative AST metadata. Bounds are source units: byte widths are
// UTF-8/source-byte offsets and rune widths are decoded code points. The
// analyzer is metadata only; RegexNode/NFA/VM remain the semantic authority.
struct RegexBound {
    enum class State : std::uint8_t { Finite, Unknown, Unbounded };
    State state = State::Finite;
    std::uint64_t value = 0;
    static RegexBound finite(std::uint64_t n) noexcept { return {State::Finite, n}; }
    static RegexBound unknown() noexcept { return {State::Unknown, 0}; }
    static RegexBound unbounded() noexcept { return {State::Unbounded, 0}; }
    bool is_finite() const noexcept { return state == State::Finite; }
    bool is_unknown() const noexcept { return state == State::Unknown; }
    bool is_unbounded() const noexcept { return state == State::Unbounded; }
};
struct RegexAnalysis {
    RegexBound byte_lower = RegexBound::finite(0), byte_upper = RegexBound::finite(0);
    RegexBound rune_lower = RegexBound::finite(0), rune_upper = RegexBound::finite(0);
    RegexBound forward_lookahead_bytes = RegexBound::finite(0);
    RegexBound forward_lookahead_runes = RegexBound::finite(0);
    RegexBound backward_lookbehind_bytes = RegexBound::finite(0);
    RegexBound backward_lookbehind_runes = RegexBound::finite(0);
    bool nullable = false, nullable_known = true;
    bool requires_record_boundary = false;
    bool requires_absolute_begin = false, requires_absolute_end = false;
    bool requires_line_begin = false, requires_line_end = false;
    bool requires_word_boundary = false, requires_word_start = false, requires_word_end = false;
    bool icase = false, unicode = true, dotall = false, multiline = false, crlf = false;
    bool contains_nul = false, custom_separator = false, separator_is_nul = false;
    unsigned char record_separator = '\n';
    bool has_backreference = false, has_lookahead = false, has_lookbehind = false;
    bool has_unbounded_repeat = false, repeat_limit_applied = false;
    bool lookbehind_limit_applied = false, vm_state_limit_relevant = false;
    std::uint64_t repeat_limit = 10000, lookbehind_limit = 8192, vm_state_limit = 50000;
    std::vector<std::string> notes;
};
// Canonical deterministic M2.2 analyzer. This is metadata only; exact matching
// remains owned by RegexNode/NFA/VM. The separator is a search input.
RegexAnalysis analyze_regex(const std::shared_ptr<RegexNode>& ast,
                           unsigned char record_separator = '\n');

struct RegexProgram {
    std::shared_ptr<RegexNode> ast;
    int groups = 0;
    std::vector<std::string> group_names;
    bool extended = false;

    // M2.2 canonical context/width metadata, computed after parse wrappers are attached.
    // This is advisory only: exact matching remains owned by the AST/NFA/VM.
    RegexAnalysis context_analysis(unsigned char record_separator = '\n') const { return analyze_regex(ast, record_separator); }
    // Canonical optimizer representation. Parsing is the only construction
    // path that installs this value; planners and verifiers must read it.
    QueryIR query_ir;
    const QueryIR& ir() const noexcept { return query_ir; }
    void install_query_ir(QueryIR value) {
        query_ir = std::move(value);
        // Legacy direct members remain source-compatible derived views.
        // They are synchronized here and must not be mutated independently.
        mandatory = query_ir.mandatory;
        prefixes = query_ir.prefixes;
        branch_mandatory = query_ir.branch_mandatory;
        is_pure_literal = query_ir.is_pure_literal;
        exact_literal = query_ir.exact_literal;
    }

    // Compatibility views for existing internal callers/tests. The
    // authoritative values are query_ir.*; these are derived at construction.
    std::vector<std::string> mandatory; // == query_ir.mandatory
    std::vector<std::string> prefixes;  // == query_ir.prefixes
    std::vector<std::vector<std::string>> branch_mandatory; // == query_ir.branch_mandatory
    bool is_pure_literal = false; // == query_ir.is_pure_literal
    std::string exact_literal;    // == query_ir.exact_literal
    std::vector<NfaInst> nfa;
    std::int32_t nfa_start = -1;
};
RegexProgram parse_regex(std::string_view pattern, const PatternOptions& opt);
bool regex_search(const RegexProgram&, const VerifierContext&, const PatternOptions&, Match*, std::uint32_t);
std::vector<Match> regex_find_all(const RegexProgram&, const VerifierContext&, const PatternOptions&, bool, std::uint32_t, std::uint64_t);
// Compatibility adapters for existing internal tests/callers. New verifier
// code should pass VerifierContext directly.
bool regex_search(const RegexProgram&, std::string_view, const PatternOptions&, std::size_t, Match*, std::uint32_t, unsigned char);
std::vector<Match> regex_find_all(const RegexProgram&, std::string_view, const PatternOptions&, bool, std::uint32_t, std::uint64_t, std::uint64_t, unsigned char);
// Test hook: directly exercises the eval recursion-depth guard without deep C++ recursion.
void test_eval_depth_guard(int depth);

struct IndexData {
    struct Group {
        std::uint8_t lg = 9;
        std::uint32_t m = 512;
        std::uint32_t words = 0;
        std::vector<std::uint32_t> gids;
        std::vector<std::uint64_t> bits;
    };
using PosDesc = detail::PosDesc;

    // Planner statistics are deliberately separate from qgram_freq. qgram_freq
    // is the legacy 16-bit hash-bucket occurrence counter used only by the
    // conservative filters. Planner entries are keyed by the raw four bytes,
    // so hash collisions cannot merge their exact frequencies.
    struct QgramStats {
        std::uint64_t occurrence_frequency = 0; // exact 4-byte windows
        std::uint64_t chunk_frequency = 0;      // distinct chunks containing it
        std::uint64_t document_frequency = 0;   // distinct documents containing it
        std::vector<std::uint32_t> chunk_ids;   // sorted deterministic scope index
        std::vector<std::uint32_t> document_ids;
    };
    std::map<std::uint32_t, QgramStats> exact_qgrams;
    // Distinct chunks per legacy hash bucket. This is an upper bound for an
    // exact q-gram's filter candidates and widens estimates under collisions.
    std::array<std::vector<std::uint32_t>,65536> hash_chunk_ids;
    std::array<std::uint64_t,65536> hash_chunk_freq{};
    bool planner_stats_ready = false;

    std::filesystem::path root;
    IndexOptions opt;
    std::vector<FileInfo> infos;
    std::vector<LoadedFile> loaded;
    std::vector<Chunk> chunks;
    std::array<Group,8> groups;
    std::vector<PosDesc> pos_desc;
    std::vector<std::uint8_t> pos;
    std::array<std::uint64_t,256> byte_freq{};
    std::array<std::uint32_t,65536> qgram_freq{};
    std::uint64_t corp_bytes = 0;
    std::int64_t root_mtime_ns = 0;
    std::uint32_t pos_block = 256;
    bool ephemeral = false;

    std::uint64_t bytes() const noexcept {
        std::uint64_t n = pos.size() + pos_desc.size()*sizeof(PosDesc) + chunks.size()*sizeof(Chunk);
        for (const auto& g : groups) n += g.bits.size()*sizeof(std::uint64_t) + g.gids.size()*sizeof(std::uint32_t);
        n += infos.size()*sizeof(FileInfo) + byte_freq.size()*sizeof(std::uint64_t) + qgram_freq.size()*sizeof(std::uint32_t);
        n += hash_chunk_freq.size() * sizeof(std::uint64_t);
        for (const auto& [key, q] : exact_qgrams)
            n += sizeof(key) + sizeof(QgramStats) + q.chunk_ids.size()*sizeof(std::uint32_t) +
                 q.document_ids.size()*sizeof(std::uint32_t);
        for (const auto& ids : hash_chunk_ids)
            n += ids.size() * sizeof(std::uint32_t);
        return n;
    }
};

// QO-4/M1.5 cost model & scheduler: estimates selectivity via exact q-gram
// chunk/document statistics, widened by legacy hash-bucket chunk counts for
// collision uncertainty. The model is conservative (no false negatives) — it
// only influences verifier annotation/planning and never rejects candidates.
// Candidate units are chunks, positional blocks, and verifier bytes; source
// q-gram occurrence bytes are not candidate-work proxies.
enum class VerifierKind : std::uint8_t { FixedRareByte = 0, FixedPositional = 1, RegexChunk = 2, RegexBruteForce = 3 };
// Physical backend selected for guarded fixed-string dispatch. FixedRareByte
// remains the public verifier label for both chunk and whole-file anchor scans.
enum class FixedPhysicalOperator : std::uint8_t { WholeFile = 0, Chunk = 1, PositionalBlock = 2 };
inline const char* to_string(VerifierKind k) noexcept {
    switch (k) {
        case VerifierKind::FixedRareByte: return "FixedRareByte";
        case VerifierKind::FixedPositional: return "FixedPositional";
        case VerifierKind::RegexChunk: return "RegexChunk";
        case VerifierKind::RegexBruteForce: return "RegexBruteForce";
    }
    return "Unknown";
}
struct QueryCost {
    double selectivity = 1.0; // estimated fraction of chunks that survive pruning (0..1)
    std::uint64_t estimated_candidate_chunks = 0;
    std::uint64_t estimated_candidate_blocks = 0;
    std::uint64_t estimated_verified_bytes = 0;
    VerifierKind verifier = VerifierKind::FixedRareByte;
    FixedPhysicalOperator fixed_operator = FixedPhysicalOperator::WholeFile;
    bool guarded_dispatch = false;
    double cost = 0.0; // calibrated candidate-work estimate
};
// Forward-declare pergrep::Pattern for cost estimation (defined in pergrep.hpp).


} // namespace pergrep::detail

namespace pergrep {
// M1.5 cost model: exact q-gram chunk/document statistics, widened by
// collision-prone hash-bucket chunk counts; no candidate rejection.
detail::QueryCost estimateCost(const Pattern& p, const detail::IndexData& I, unsigned char record_separator = '\n');
detail::VerifierKind chooseVerifier(const Pattern& p, const detail::IndexData& I, unsigned char record_separator = '\n');
// Internal helper: pick rarest q-gram branch (for tests).
std::string pick_rarest_branch_literal(const std::vector<std::vector<std::string>>& branches, const detail::IndexData& I);
namespace detail {
// M0.7: Candidate plan generator for shadow execution and plan regret analysis.
std::vector<PlanCandidateMetrics> estimate_all_candidate_plans(const Pattern& p, const IndexData& I, unsigned char record_separator);
double estimate_literal_selectivity(std::string_view lit, const IndexData& I);
double estimate_branch_selectivity(const std::vector<std::vector<std::string>>& branches, const IndexData& I);
} // namespace detail

struct Pattern::Impl { std::string expr; PatternOptions opt; detail::RegexProgram re; };
struct Index::Impl : detail::IndexData {};
} // namespace pergrep
