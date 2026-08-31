#include "internal.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <unicode/uchar.h>
#include <unicode/utf8.h>
namespace pergrep {
using detail::QueryDesc;
namespace {
// Recursive pure-literal regex conversion calls find again with a Fixed
// Pattern. Keep the M1.7 guard exclusive to direct Fixed searches.
thread_local bool suppress_guarded_fixed_dispatch = false;
struct FixedDispatchSuppression {
    bool previous;
    FixedDispatchSuppression() noexcept : previous(suppress_guarded_fixed_dispatch) {
        suppress_guarded_fixed_dispatch = true;
    }
    ~FixedDispatchSuppression() { suppress_guarded_fixed_dispatch = previous; }
};
enum class ProbeKind : std::uint8_t { Chunk, Positional };
struct StatsRecorder {
    SearchStats* stats = nullptr;
    const detail::IndexData& index;
    std::vector<std::vector<std::pair<std::uint64_t, std::uint64_t>>> touched;
    std::unordered_set<std::uint32_t> candidate_file_ids;
    std::vector<std::uint8_t> eligible;

    StatsRecorder(SearchStats* s, const detail::IndexData& i,
                  std::span<const std::uint32_t> eligible_file_ids = {})
        : stats(s), index(i) {
        if (stats) touched.resize(index.infos.size());
        if (!eligible_file_ids.empty()) {
            eligible.assign(index.infos.size(), 0);
            for (auto fid : eligible_file_ids) {
                if (fid < eligible.size()) eligible[fid] = 1;
            }
        }
    }

    bool allows(std::uint32_t file_id) const noexcept {
        return eligible.empty() || (file_id < eligible.size() && eligible[file_id]);
    }

    void note_candidates(const std::vector<std::uint32_t>& chunks) {
        if (!stats) return;
        for (auto ci : chunks) {
            if (ci >= index.chunks.size() || !allows(index.chunks[ci].file_id)) continue;
            ++stats->candidate_chunks;
            candidate_file_ids.insert(index.chunks[ci].file_id);
        }
    }

    void note_all_chunks() {
        if (!stats) return;
        for (const auto& chunk : index.chunks) {
            if (!allows(chunk.file_id)) continue;
            ++stats->candidate_chunks;
            candidate_file_ids.insert(chunk.file_id);
        }
    }

    void note_probe(std::size_t bytes, ProbeKind kind) {
        if (!stats) return;
        stats->index_probe_bytes += bytes;
        ++stats->index_probe_operations;
        if (kind == ProbeKind::Chunk) {
            stats->chunk_probe_bytes += bytes;
            ++stats->chunk_probe_operations;
        } else {
            stats->positional_probe_bytes += bytes;
            ++stats->positional_probe_operations;
        }
    }

    void note_selection(std::size_t count) {
        if (!stats) return;
        stats->effective_k = static_cast<std::uint64_t>(count);
        stats->selected_qgram_count = static_cast<std::uint64_t>(count);
        stats->selected_qgram_rows = static_cast<std::uint64_t>(count);
    }

    void touch(std::uint32_t file_id, std::uint64_t begin, std::uint64_t end) {
        if (!stats || !allows(file_id) || begin >= end || file_id >= touched.size()) return;
        const auto bytes = end - begin;
        stats->physically_touched_bytes += bytes;
        stats->verified_bytes += bytes; // legacy alias for physical work
        touched[file_id].push_back({begin, end});
    }

    void finish() {
        if (!stats) return;
        for (auto& ranges : touched) {
            if (ranges.empty()) continue;
            std::sort(ranges.begin(), ranges.end());
            std::uint64_t begin = ranges.front().first;
            std::uint64_t end = ranges.front().second;
            for (std::size_t i = 1; i < ranges.size(); ++i) {
                if (ranges[i].first > end) {
                    stats->logical_unique_bytes += end - begin;
                    begin = ranges[i].first;
                    end = ranges[i].second;
                } else if (ranges[i].second > end) {
                    end = ranges[i].second;
                }
            }
            stats->logical_unique_bytes += end - begin;
        }
        stats->candidate_files = candidate_file_ids.size();
    }
};
struct BoundedRegexRegion {
    std::uint64_t candidate_begin = 0;
    std::uint64_t candidate_end = 0;
    std::uint64_t region_begin = 0;
    std::uint64_t region_end = 0;
};

std::vector<std::shared_ptr<detail::RegexNode>> top_level_branches(
    const std::shared_ptr<detail::RegexNode>& root) {
    if (!root) return {};
    if (root->kind == detail::RegexNode::Kind::Group && !root->children.empty())
        return top_level_branches(root->children.front());
    if (root->kind == detail::RegexNode::Kind::Alt) return root->children;
    return {root};
}
bool bounded_regex_eligible(const detail::RegexProgram& re, const PatternOptions& opt,
                             unsigned char separator, detail::RegexAnalysis* analysis) {
    // Region execution is safe for all regular boundary modes: boundary
    // assertions consult source/record metadata, while only rune consumption
    // is clipped to the execution region. Extended constructs stay on the
    // established full verifier because their context can be data-dependent.
    const bool multi_mandatory = re.query_ir.mandatory.size() >= 2;
    auto branches = top_level_branches(re.ast);
    bool branch_multi_mandatory = !re.query_ir.branch_mandatory.empty() &&
        std::all_of(re.query_ir.branch_mandatory.begin(), re.query_ir.branch_mandatory.end(),
                    [](const auto& branch) { return branch.size() >= 2; });
    if (!branch_multi_mandatory && branches.size() > 1) {
        branch_multi_mandatory = std::all_of(branches.begin(), branches.end(), [](const auto& branch) {
            return detail::query_mandatory(branch).size() >= 2;
        });
    }
    if (re.extended || re.groups > 1 || (!multi_mandatory && !branch_multi_mandatory) ||
        opt.case_mode != CaseMode::Sensitive) return false;
    auto a = re.context_analysis(separator);
    if (!a.byte_upper.is_finite() || a.byte_upper.value == 0 || a.byte_upper.value > (1u << 20)) return false;
    if (!a.forward_lookahead_bytes.is_finite() || !a.backward_lookbehind_bytes.is_finite()) return false;
    if (a.has_backreference || a.has_lookahead || a.has_lookbehind || a.has_unbounded_repeat) return false;
    if (re.query_ir.mandatory.empty() && !branch_multi_mandatory) return false;
    for (const auto& literal : re.query_ir.mandatory)
        if (literal.empty() || literal.size() > a.byte_upper.value) return false;
    if (analysis) *analysis = std::move(a);
    return true;
}

std::vector<BoundedRegexRegion> bounded_regex_regions(std::string_view record, std::uint64_t record_begin,
                                                       std::uint64_t record_end, std::string_view literal,
                                                       const detail::RegexAnalysis& analysis) {
    std::vector<BoundedRegexRegion> regions;
    if (literal.empty() || record.empty()) return regions;
    const auto width = analysis.byte_upper.value;
    const auto literal_width = static_cast<std::uint64_t>(literal.size());
    if (literal_width > width) return regions;
    const auto start_slack = width - literal_width;
    for (std::size_t at = record.find(literal); at != std::string_view::npos;) {
        const auto absolute = record_begin + static_cast<std::uint64_t>(at);
        const auto begin = absolute > start_slack ? std::max(record_begin, absolute - start_slack) : record_begin;
        const auto end = std::min(record_end + 1, absolute + 1);
        const auto lookahead = analysis.forward_lookahead_bytes.value;
        const auto extension = width > std::numeric_limits<std::uint64_t>::max() - lookahead
            ? std::numeric_limits<std::uint64_t>::max() : width + lookahead;
        const auto region_end = record_end - end < extension ? record_end : end + extension;
        if (begin < end && begin < region_end) {
            BoundedRegexRegion next{begin, end, begin, region_end};
            if (!regions.empty() && next.candidate_begin <= regions.back().candidate_end) {
                auto& current = regions.back();
                current.candidate_end = std::max(current.candidate_end, next.candidate_end);
                current.region_begin = std::min(current.region_begin, next.region_begin);
                current.region_end = std::max(current.region_end, next.region_end);
            } else {
                regions.push_back(next);
            }
        }
        if (at > record.size() - literal.size()) break;
        at = record.find(literal, at + 1);
    }
    return regions;
}
// M2.6 interval-aware candidate joins.
struct LiteralOffsetConstraint {
    std::string literal;
    std::uint64_t min_start = 0;
    std::uint64_t max_start = 0;
};
struct LiteralOffsetSummary {
    std::uint64_t min_width = 0, max_width = 0;
    bool finite = true, mandatory_known = true;
    std::vector<LiteralOffsetConstraint> literals;
};
std::uint64_t sat_add(std::uint64_t a, std::uint64_t b) noexcept {
    return a > std::numeric_limits<std::uint64_t>::max() - b
        ? std::numeric_limits<std::uint64_t>::max() : a + b;
}
std::uint64_t sat_mul(std::uint64_t a, std::uint64_t b) noexcept {
    return a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a
        ? std::numeric_limits<std::uint64_t>::max() : a * b;
}
LiteralOffsetSummary literal_offsets(const std::shared_ptr<detail::RegexNode>& node) {
    using K = detail::RegexNode::Kind;
    LiteralOffsetSummary out;
    if (!node) return out;
    switch (node->kind) {
    case K::Empty: case K::Begin: case K::End: case K::AbsBegin: case K::AbsEnd:
    case K::EndNewline: case K::WordBoundary: case K::WordStartHalf: case K::WordEndHalf:
        return out;
    case K::Literal:
        out.min_width = out.max_width = node->literal.size();
        if (!node->literal.empty() && !node->icase)
            out.literals.push_back({node->literal, 0, 0});
        return out;
    case K::Dot: case K::Class:
        out.min_width = 1; out.max_width = 4; return out;
    case K::Group:
        return node->children.empty() ? out : literal_offsets(node->children.front());
    case K::Concat: {
        std::uint64_t min_prefix = 0, max_prefix = 0;
        for (const auto& child : node->children) {
            auto part = literal_offsets(child);
            out.finite = out.finite && part.finite;
            out.mandatory_known = out.mandatory_known && part.mandatory_known;
            for (auto literal : part.literals) {
                literal.min_start = sat_add(min_prefix, literal.min_start);
                literal.max_start = sat_add(max_prefix, literal.max_start);
                out.literals.push_back(std::move(literal));
            }
            min_prefix = sat_add(min_prefix, part.min_width);
            max_prefix = sat_add(max_prefix, part.max_width);
        }
        out.min_width = min_prefix; out.max_width = max_prefix; return out;
    }
    case K::Alt: {
        bool first = true;
        for (const auto& child : node->children) {
            auto part = literal_offsets(child);
            out.finite = out.finite && part.finite;
            if (first) { out.min_width = part.min_width; out.max_width = part.max_width; first = false; }
            else { out.min_width = std::min(out.min_width, part.min_width); out.max_width = std::max(out.max_width, part.max_width); }
        }
        out.mandatory_known = false; out.literals.clear(); return out;
    }
    case K::Repeat: {
        if (node->children.empty()) return out;
        auto part = literal_offsets(node->children.front());
        out.finite = part.finite && node->max != SIZE_MAX;
        out.mandatory_known = part.mandatory_known && node->min != 0;
        out.min_width = sat_mul(part.min_width, node->min);
        out.max_width = node->max == SIZE_MAX ? std::numeric_limits<std::uint64_t>::max()
                                              : sat_mul(part.max_width, node->max);
        if (node->min == 0 || node->max == SIZE_MAX) return out;
        for (auto literal : part.literals) {
            literal.min_start = 0;
            literal.max_start = out.max_width > literal.literal.size()
                ? out.max_width - literal.literal.size() : 0;
            out.literals.push_back(std::move(literal));
        }
        return out;
    }
    default:
        out.finite = false; out.mandatory_known = false; return out;
    }
}
std::vector<BoundedRegexRegion> bounded_regex_regions_joined(
    std::string_view record, std::uint64_t record_begin, std::uint64_t record_end,
    const std::vector<LiteralOffsetConstraint>& constraints,
    const detail::RegexAnalysis& analysis) {
    if (constraints.empty() || record.empty()) return {};
    using Interval = std::pair<std::uint64_t, std::uint64_t>;
    std::vector<Interval> joined;
    for (const auto& constraint : constraints) {
        if (constraint.literal.empty()) return {};
        std::vector<Interval> current;
        for (std::size_t at = record.find(constraint.literal);
             at != std::string_view::npos;) {
            const auto absolute = record_begin + static_cast<std::uint64_t>(at);
            if (absolute >= record_begin && absolute - record_begin >= constraint.min_start) {
                const auto low = absolute > constraint.max_start
                    ? std::max(record_begin, absolute - constraint.max_start) : record_begin;
                const auto start = absolute - constraint.min_start;
                const auto record_limit = record_end == std::numeric_limits<std::uint64_t>::max() ? record_end : record_end + 1;
                const auto high = std::min(record_limit, sat_add(start, 1));
                if (low < high) current.push_back({low, high});
            }
            if (at > record.size() - constraint.literal.size()) break;
            at = record.find(constraint.literal, at + 1);
        }
        if (current.empty()) return {};
        if (joined.empty()) { joined = std::move(current); continue; }
        std::vector<Interval> next;
        std::size_t i = 0, j = 0;
        while (i < joined.size() && j < current.size()) {
            const auto begin = std::max(joined[i].first, current[j].first);
            const auto end = std::min(joined[i].second, current[j].second);
            if (begin < end) next.push_back({begin, end});
            if (joined[i].second < current[j].second) ++i; else ++j;
        }
        if (next.empty()) return {};
        joined = std::move(next);
    }
    std::vector<BoundedRegexRegion> regions;
    const auto width = analysis.byte_upper.value;
    const auto lookahead = analysis.forward_lookahead_bytes.value;
    const auto back = analysis.backward_lookbehind_bytes.value;
    for (const auto [begin, end] : joined) {
        const auto region_begin = begin > back ? std::max(record_begin, begin - back) : record_begin;
        const auto region_end = std::min(record_end, sat_add(end, sat_add(width, lookahead)));
        if (begin < end && region_begin < region_end)
            regions.push_back({begin, end, region_begin, region_end});
    }
    std::vector<BoundedRegexRegion> merged;
    for (const auto& next : regions) {
        if (!merged.empty() && next.candidate_begin <= merged.back().candidate_end) {
            auto& current = merged.back();
            current.candidate_end = std::max(current.candidate_end, next.candidate_end);
            current.region_begin = std::min(current.region_begin, next.region_begin);
            current.region_end = std::max(current.region_end, next.region_end);
        } else merged.push_back(next);
    }
    return merged;
}


struct DecodedRune { UChar32 cp = U_SENTINEL; std::size_t next = 0; bool ok = false; };
DecodedRune decode_rune(std::string_view s, std::size_t pos) {
    if (pos >= s.size()) return {};
    int32_t i = static_cast<int32_t>(pos), n = static_cast<int32_t>(s.size()); UChar32 cp; U8_NEXT(s.data(), i, n, cp);
    if (cp < 0) return {static_cast<unsigned char>(s[pos]), pos + 1, true};
    return {cp, static_cast<std::size_t>(i), true};
}
DecodedRune decode_prev(std::string_view s, std::size_t pos) {
    if (!pos) return {};
    int32_t i = static_cast<int32_t>(pos); UChar32 cp; U8_PREV(s.data(), 0, i, cp);
    if (cp < 0) return {static_cast<unsigned char>(s[pos-1]), pos-1, true};
    return {cp, static_cast<std::size_t>(i), true};
}
bool unicode_icase_equal_at(std::string_view s, std::string_view q, std::size_t pos, std::size_t* end = nullptr) {
    std::size_t sp = pos, qp = 0;
    while (qp < q.size()) {
        auto a = decode_rune(s, sp), b = decode_rune(q, qp);
        if (!a.ok || !b.ok || u_foldCase(a.cp, U_FOLD_CASE_DEFAULT) != u_foldCase(b.cp, U_FOLD_CASE_DEFAULT))
            return false;
        sp = a.next;
        qp = b.next;
    }
    if (end) *end = sp;
    return true;
}
bool unicode_word_cp(UChar32 cp) {
    return u_isalnum(cp) ||
           u_charType(cp) == U_CONNECTOR_PUNCTUATION ||
           u_hasBinaryProperty(cp, UCHAR_JOIN_CONTROL) ||
           u_charType(cp) == U_NON_SPACING_MARK ||
           u_charType(cp) == U_COMBINING_SPACING_MARK ||
           u_charType(cp) == U_ENCLOSING_MARK;
}
void group_candidates(const detail::IndexData::Group& g, const QueryDesc& q,
                      std::vector<uint32_t>& out, StatsRecorder* rec = nullptr) {
    if (g.gids.empty()) return;
    auto const& qc = q.classes[g.lg - 9];
    if (qc.empty()) {
        for (auto ci : g.gids) {
            if (!rec || rec->allows(rec->index.chunks[ci].file_id)) out.push_back(ci);
        }
        return;
    }
    std::vector<uint64_t> c(g.words, ~0ull);
    if (g.gids.size() & 63) c.back() = (1ull << (g.gids.size() & 63)) - 1;
    for (auto [ww, mask64] : qc) {
        while (mask64) {
            unsigned bit = std::countr_zero(mask64);
            uint32_t row = uint32_t(ww) * 64 + bit;
            auto p = g.bits.data() + (size_t)row * g.words;
            if (rec) rec->note_probe(static_cast<std::size_t>(g.words) * sizeof(std::uint64_t), ProbeKind::Chunk);
            for (uint32_t j = 0; j < g.words; ++j) c[j] &= p[j];
            mask64 &= mask64 - 1;
        }
        bool any = false;
        for (auto v : c) any |= v != 0;
        if (!any) return;
    }
    for (uint32_t w = 0; w < g.words; ++w) {
        uint64_t z = c[w];
        while (z) {
            unsigned b = std::countr_zero(z);
            uint32_t li = w * 64 + b;
            if (li < g.gids.size()) {
                auto ci = g.gids[li];
                if (!rec || rec->allows(rec->index.chunks[ci].file_id)) out.push_back(ci);
            }
            z &= z - 1;
        }
    }
}

std::vector<uint32_t> planned_hashes(const detail::IndexData& I, std::string_view q);

std::vector<uint32_t> chunk_candidates(const detail::IndexData& I, std::string_view lit,
                                       StatsRecorder* rec = nullptr) {
    std::vector<uint32_t> out;
    if (lit.size() < 4 || lit.size() > I.opt.chunk_overlap) {
        out.reserve(I.chunks.size());
        for (uint32_t ci = 0; ci < I.chunks.size(); ++ci) {
            if (!rec || rec->allows(I.chunks[ci].file_id)) out.push_back(ci);
        }
        return out;
    }
    auto selected = planned_hashes(I, lit);
    if (rec) rec->note_selection(selected.size());
    auto q = detail::compile_qgram_query(lit, selected);
    for (auto const& g : I.groups) {
        group_candidates(g, q, out, rec);
    }
    std::sort(out.begin(), out.end(), [&](uint32_t a, uint32_t b) {
        if (I.chunks[a].file_id != I.chunks[b].file_id)
            return I.chunks[a].file_id < I.chunks[b].file_id;
        if (I.chunks[a].core_begin != I.chunks[b].core_begin)
            return I.chunks[a].core_begin < I.chunks[b].core_begin;
        return a < b;
    });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}


size_t choose_rare_byte(const detail::IndexData& I, std::string_view q) {
    if (q.empty()) return 0;
    size_t a = 0;
    uint64_t best = UINT64_MAX;
    for (size_t i = 0; i < q.size(); ++i) {
        auto x = I.byte_freq[(unsigned char)q[i]];
        if (x < best) {
            best = x;
            a = i;
        }
    }
    return a;
}

size_t anchor_find(std::string_view s, std::string_view q, size_t anchor, size_t start = 0, size_t max_start = SIZE_MAX, bool icase = false, size_t* match_end = nullptr) {
    if (q.empty()) {
        if (start <= s.size() && start < max_start) {
            if (match_end) *match_end = start;
            return start;
        }
        return std::string_view::npos;
    }
    if (!icase && s.size() < q.size()) return std::string_view::npos;
    if (icase) {
        size_t p = start;
        while (p < s.size() && p < max_start) {
            size_t e = 0;
            if (unicode_icase_equal_at(s, q, p, &e)) {
                if (match_end) *match_end = e;
                return p;
            }
            auto r = decode_rune(s, p);
            p = r.ok ? r.next : p + 1;
        }
        return std::string_view::npos;
    }
    max_start = std::min(max_start, s.size() - q.size() + 1);
    if (start >= max_start) return std::string_view::npos;
    const unsigned char* base = (const unsigned char*)s.data();
    unsigned char needle = (unsigned char)q[anchor];
    size_t lo = start + anchor, hi = max_start + anchor;
    while (lo < hi) {
        auto p = (const unsigned char*)std::memchr(base + lo, needle, hi - lo);
        if (!p) return std::string_view::npos;
        size_t apos = p - base, st = apos - anchor;
        if (std::memcmp(base + st, q.data(), q.size()) == 0) {
            if (match_end) *match_end = st + q.size();
            return st;
        }
        lo = apos + 1;
    }
    return std::string_view::npos;
}

// Deterministic q-gram budget contract shared by chunk and positional filters.
// `planned_qgrams == 0` means auto (all available distinct hash rows); positive
// values are a maximum and clamp only to the available distinct query rows.
// No cost-derived or fixed eight-row cap is applied. A selected subset remains
// conservative because every indexed q-gram row is a necessary condition.
size_t adaptive_k(const detail::IndexData& I, std::string_view q) {
    if (q.size() < 4) return 0;
    std::unordered_set<uint32_t> uniq;
    uniq.reserve(q.size());
    for (size_t i = 0; i + 4 <= q.size(); ++i)
        uniq.insert(detail::hash4(reinterpret_cast<const unsigned char*>(q.data()) + i));
    const size_t available = uniq.size();
    if (available == 0) return 0;
    return I.opt.planned_qgrams == 0
        ? available
        : std::min(I.opt.planned_qgrams, available);
}

// Rarity-aware q-gram planner. Planner ordering uses exact q-gram chunk
// frequency, widened by the corresponding legacy hash-bucket chunk frequency.
// The returned hashes remain the legacy hash values consumed by conservative
// Bloom filters; planner ordering and the explicitly configured probe budget
// determine the selected safe subset shared by chunk and positional filters.
std::vector<uint32_t> planned_hashes(const detail::IndexData& I, std::string_view q) {
    std::vector<std::pair<uint32_t, std::uint64_t>> ranked;
    if (q.size() < 4) return {};
    for (size_t i = 0; i + 4 <= q.size(); ++i) {
        const auto* p = reinterpret_cast<const unsigned char*>(q.data() + i);
        const auto hash = detail::hash4(p);
        const auto key = detail::qgram4_key(p);
        std::uint64_t exact_chunks = I.chunks.size();
        auto it = I.exact_qgrams.find(key);
        if (I.planner_stats_ready && it != I.exact_qgrams.end())
            exact_chunks = it->second.chunk_frequency;
        std::uint64_t bucket_chunks = I.chunks.size();
        if (I.planner_stats_ready)
            bucket_chunks = I.hash_chunk_freq[hash & 65535u];
        ranked.push_back({hash, std::max(exact_chunks, bucket_chunks)});
    }
    std::sort(ranked.begin(), ranked.end(), [](auto a, auto b) {
        if (a.second != b.second) return a.second < b.second;
        return a.first < b.first;
    });
    std::vector<std::pair<uint32_t, std::uint64_t>> unique_ranked;
    unique_ranked.reserve(ranked.size());
    for (const auto item : ranked) {
        bool seen = false;
        for (const auto prior : unique_ranked)
            if (prior.first == item.first) { seen = true; break; }
        if (!seen) unique_ranked.push_back(item);
    }
    ranked.swap(unique_ranked);
    std::vector<uint32_t> h;
    h.reserve(ranked.size());
    for (auto [hash, unused] : ranked) {
        (void)unused;
        h.push_back(hash);
    }
    size_t k = adaptive_k(I, q);
    if (k == 0) return {};
    // Never discard all rows based on a frequency heuristic: an empty result
    // would be an unsafe rejection rather than a conservative widening.
    if (h.size() > k) h.resize(k);
    return h;
}

// Positional filter compiler — query-aware block Bloom.
// Positional filter compiler — query-aware block Bloom. The planner chooses
// q-grams using exact chunk frequencies widened by legacy hash-bucket collision
// counts; this only changes which safe subset of hash rows is intersected.
// Conservative invariants:
// - Positional blocks are ONLY used for case-sensitive fixed literals without word/line flags.
// - A literal longer than chunk_overlap uses whole-file fallback.
// - Safe cross-chunk fallback: when literal length exceeds chunk_overlap, a match may straddle
//   two chunks and would be missed by chunk-level pruning. The code detects `q.size() > chunk_overlap`
//   and falls back to whole-file rare-byte scan over the union of candidate files (conservative
//   files union). This guarantees no false negatives for long literals crossing 32 KiB boundaries.
// - Block Bloom is conservative: each block's Bloom may have false positives but never false
//   negatives; intersection of q-gram rows can only over-approximate candidate blocks.
std::vector<std::pair<uint32_t, uint32_t>> fixed_candidate_blocks(
    const detail::IndexData& I, std::string_view q, StatsRecorder* rec) {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    if (q.empty()) {
        out.reserve(I.chunks.size());
        for (uint32_t ci = 0; ci < I.chunks.size(); ++ci) {
            if (!rec || rec->allows(I.chunks[ci].file_id)) out.push_back({ci, 0});
        }
        if (rec && rec->stats) {
            rec->note_all_chunks();
            rec->stats->candidate_blocks += out.size();
        }
        return out;
    }
    auto cv = chunk_candidates(I, q, rec);
    if (rec) rec->note_candidates(cv);
    auto hs = planned_hashes(I, q);
    if (rec) rec->note_selection(hs.size());
    std::vector<uint8_t> bm;
    for (auto ci : cv) {
        auto d = I.pos_desc[ci];
        if (d.blocks == 0 || d.mask_bytes == 0) continue;
        bm.assign(d.mask_bytes, 0xFF);
        if ((d.blocks & 7) != 0) {
            bm.back() = static_cast<uint8_t>((1u << (d.blocks & 7)) - 1);
        }
        std::vector<uint16_t> rows;
        for (auto h : hs) {
            rows.push_back(static_cast<uint16_t>(h & (d.m - 1)));
        }
        std::sort(rows.begin(), rows.end());
        rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
        for (auto r : rows) {
            const uint8_t* ptr = I.pos.data() + d.off + static_cast<size_t>(r) * d.mask_bytes;
            if (rec) rec->note_probe(d.mask_bytes, ProbeKind::Positional);
            bool any = false;
            for (uint32_t j = 0; j < d.mask_bytes; ++j) {
                bm[j] &= ptr[j];
                if (bm[j] != 0) any = true;
            }
            if (!any) break;
        }
        for (uint32_t byte_idx = 0; byte_idx < d.mask_bytes; ++byte_idx) {
            uint8_t val = bm[byte_idx];
            while (val) {
                unsigned b = std::countr_zero(val);
                uint32_t bi = (byte_idx << 3) + b;
                if (bi < d.blocks) {
                    out.push_back({ci, bi});
                }
                val &= val - 1;
            }
        }
    }
    if (rec && rec->stats) rec->stats->candidate_blocks += out.size();
    return out;
}

bool fixed_match_in_record(std::string_view rec, std::string_view q, bool icase, bool word_opt, bool line_opt, bool unicode_opt, const detail::IndexData& I) {
    size_t a = choose_rare_byte(I, q);
    size_t pos = 0;
    size_t max_pos = rec.size() + (q.empty() ? 1 : 0);
    while (pos < max_pos) {
        size_t local_end = 0;
        auto x = anchor_find(rec, q, a, pos, max_pos, icase, &local_end);
        if (x == std::string_view::npos) break;
        uint64_t abs = x, abs_end = local_end;
        bool ok = true;
        if (word_opt) {
            if (unicode_opt) {
                auto l = decode_prev(rec, abs), r = decode_rune(rec, abs_end);
                if (l.ok && unicode_word_cp(l.cp)) ok = false;
                if (r.ok && unicode_word_cp(r.cp)) ok = false;
            } else {
                auto word = [](unsigned char c) { return std::isalnum(c) || c == '_'; };
                if (abs > 0 && word((unsigned char)rec[abs - 1])) ok = false;
                if (abs_end < rec.size() && word((unsigned char)rec[abs_end])) ok = false;
            }
        }
        if (line_opt) {
            if (abs != 0 || abs_end != rec.size()) ok = false;
        }
        if (ok) return true;
        pos = x + std::max<size_t>(1, local_end - x);
    }
    return false;
}
// Planner selectivity is expressed in candidate chunks, not source-byte
// occurrence counts. Hash-bucket frequencies are conservative upper bounds;
// exact entries are keyed by raw q-gram bytes and remain collision-aware.
static bool scope_contains(std::span<const std::uint32_t> scope, std::uint32_t fid) {
    return scope.empty() || std::binary_search(scope.begin(), scope.end(), fid);
}
static std::uint64_t scoped_chunk_count(const detail::IndexData& I,
                                        std::span<const std::uint32_t> scope) {
    if (scope.empty()) return I.chunks.size();
    std::uint64_t n = 0;
    for (const auto& c : I.chunks) n += scope_contains(scope, c.file_id);
    return n;
}
static std::uint64_t scoped_hash_chunks(const detail::IndexData& I, std::uint32_t bucket,
                                        std::span<const std::uint32_t> scope) {
    if (!I.planner_stats_ready) return I.chunks.size();
    const auto total = scoped_chunk_count(I, scope);
    std::uint64_t result = 0;
    // A group with lg bits accepts every legacy 16-bit bucket sharing the
    // query's low-lg bits. Count those chunk IDs exactly; this widens for
    // collisions without collapsing rare grams into all chunks.
    std::vector<std::uint32_t> seen(I.chunks.size(), 0);
    for (std::uint8_t lg = 9; lg <= 16; ++lg) {
        const std::uint32_t mask = (1u << lg) - 1u;
        const std::uint32_t target = bucket & mask;
        const std::uint32_t generation = lg - 8;
        for (std::uint32_t b = 0; b < 65536; ++b) {
            if ((b & mask) != target) continue;
            for (auto ci : I.hash_chunk_ids[b]) {
                if (ci < I.chunks.size() && seen[ci] != generation &&
                    scope_contains(scope, I.chunks[ci].file_id)) {
                    const auto actual_lg = detail::lg_for(static_cast<std::size_t>(
                        I.chunks[ci].ext_end - I.chunks[ci].core_begin));
                    if (actual_lg == lg) {
                        seen[ci] = generation;
                        ++result;
                    }
                }
            }
        }
    }
    return std::min(result, total);
}
static std::uint64_t scoped_extended_bytes(
    const detail::IndexData& I, std::span<const std::uint32_t> scope) {
    std::uint64_t n = 0;
    for (const auto& c : I.chunks) {
        if (c.file_id >= I.loaded.size()) continue;
        const auto size = static_cast<std::uint64_t>(I.loaded[c.file_id].data.size());
        if (c.core_begin > size || c.ext_end < c.core_begin || c.ext_end > size) continue;
        if (scope.empty() || scope_contains(scope, c.file_id))
            n += c.ext_end - c.core_begin;
    }
    return n;
}
static double estimate_literal_selectivity_impl(
    std::string_view lit, const detail::IndexData& I,
    std::span<const std::uint32_t> scope = {}) {
    const auto total_chunks = scoped_chunk_count(I, scope);
    if (lit.size() < 4 || total_chunks == 0) return 1.0;
    if (!I.planner_stats_ready) return 1.0;
    double selectivity = 1.0;
    // Mirror the execution planner: estimates must describe the selected
    // budgeted rows, not every query window and not a hidden eight-row cap.
    const auto selected = planned_hashes(I, lit);
    for (const auto hash : selected) {
        const auto candidates = std::min<std::uint64_t>(
            total_chunks, scoped_hash_chunks(I, hash & 65535u, scope));
        selectivity = std::min(selectivity, double(candidates) / double(total_chunks));
    }
    return std::clamp(selectivity, 0.0, 1.0);
}
static double estimate_branch_selectivity_impl(
    const std::vector<std::vector<std::string>>& branches, const detail::IndexData& I,
    std::span<const std::uint32_t> scope = {}) {
    if (branches.empty()) return 1.0;
    if (scoped_chunk_count(I, scope) == 0) return 1.0;
    double sum = 0.0;
    for (const auto& br : branches) {
        if (br.empty()) return 1.0;
        double best = 1.0;
        for (const auto& lit : br)
            best = std::min(best, estimate_literal_selectivity_impl(lit, I, scope));
        sum += best;
        if (sum >= 1.0) return 1.0;
    }
    return std::min(sum, 1.0);
}

// M1.7 guarded fixed dispatch. The guard is deliberately narrower than the
// exact matchers: only the q-gram/positional contract shared by all three
// physical backends is admitted. Any uncertainty leaves the established
// length/option dispatch untouched.
static bool fixed_guard_eligible(const PlanKey& key, const detail::IndexData& I) {
    const auto& po = key.pattern_options;
    const auto& io = key.index_options;
    const auto qlen = key.pattern_expression.size();
    if (po.kind != PatternKind::Fixed || qlen < 4 || qlen > 64 || qlen > io.chunk_overlap) return false;
    if (po.case_mode != CaseMode::Sensitive || po.word || po.line || po.multiline || po.dotall || po.crlf ||
        !po.unicode || po.engine != Engine::Default) return false;
    if (key.overlapping || key.invert_match || key.files_with_matches || key.files_without_match ||
        key.max_matches != 0 || key.record_separator != '\n' || key.include_binary ||
        !key.eligible_file_ids.empty()) return false;
    // A PlanKey must describe this exact index. This prevents a stale or
    // synthetic key from selecting an operator whose layout differs.
    if (io.chunk_bytes != I.opt.chunk_bytes || io.chunk_overlap != I.opt.chunk_overlap ||
        io.positional_block_bytes != I.opt.positional_block_bytes ||
        io.positional_budget_ratio != I.opt.positional_budget_ratio || io.planned_qgrams != I.opt.planned_qgrams ||
        io.include_hidden != I.opt.include_hidden || io.follow_symlinks != I.opt.follow_symlinks ||
        io.persist_corpus != I.opt.persist_corpus) return false;
    if (!I.planner_stats_ready || I.pos_block == 0 ||
        I.pos_desc.size() != I.chunks.size() || I.pos.empty()) return false;
    // Excluding binary inputs makes the binary policy invariant for every
    // candidate backend. Callers that opt into binary or have binary files use
    // the existing dispatch instead.
    for (const auto& info : I.infos) if (info.binary) return false;
    return true;
}

struct FixedPlanCosts {
    double positional = 0.0;
    double chunk = 0.0;
    double whole_file = 0.0;
    std::uint64_t chunks = 0;
    std::uint64_t blocks = 0;
    std::uint64_t positional_bytes = 0;
    std::uint64_t chunk_bytes = 0;
    std::uint64_t whole_bytes = 0;
};

static FixedPlanCosts fixed_plan_costs(const PlanKey& key, const detail::IndexData& I) {
    FixedPlanCosts c;
    const std::span<const std::uint32_t> scope(key.eligible_file_ids.data(), key.eligible_file_ids.size());
    const auto total_chunks = scoped_chunk_count(I, scope);
    const auto total_blocks = [&] {
        std::uint64_t n = 0;
        for (std::size_t i = 0; i < I.pos_desc.size(); ++i) {
            if (i < I.chunks.size() && scope_contains(scope, I.chunks[i].file_id)) n += I.pos_desc[i].blocks;
        }
        return n;
    }();
    const auto sel = estimate_literal_selectivity_impl(key.pattern_expression, I, scope);
    c.chunks = total_chunks ? static_cast<std::uint64_t>(sel * total_chunks) : 0;
    if (c.chunks == 0 && sel < 1.0 && total_chunks > 0) c.chunks = 1;
    c.blocks = total_blocks ? static_cast<std::uint64_t>(sel * total_blocks) : 0;
    if (c.blocks == 0 && sel < 1.0 && total_blocks > 0) c.blocks = 1;
    c.positional_bytes = c.blocks * (I.pos_block + 64);
    c.chunk_bytes = c.chunks * I.opt.chunk_bytes;
    c.whole_bytes = scoped_extended_bytes(I, scope);
    // Coefficients are calibrated to M1.6 shadow units: index probes and
    // candidate enumeration are charged separately from source verification.
    const auto selected_qgrams = adaptive_k(I, key.pattern_expression);
    // Charge the rows actually selected by the shared q-gram budget. This is
    // deliberately independent of positional_budget_ratio: that ratio sizes
    // the index, while planned_qgrams controls query probes.
    c.positional = 0.5 * double(c.positional_bytes) + 50.0 * double(c.blocks) +
                   10.0 * double(c.chunks) +
                   double(selected_qgrams) * double(c.chunks);
    c.chunk = double(c.chunk_bytes) + 100.0 * double(c.chunks);
    c.whole_file = double(c.whole_bytes) + 100.0 * double(c.chunks);
    return c;
}
} // namespace
// M1.3 PlanKey implementation
namespace {
inline uint64_t fnv_mix(uint64_t h, uint64_t v) noexcept {
    h ^= v;
    h *= 1099511628211ULL;
    return h;
}
inline uint64_t fnv_mix_str(uint64_t h, std::string_view s) noexcept {
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    h ^= 0xFF; h *= 1099511628211ULL;
    return h;
}
inline uint64_t double_bits(double d) noexcept {
    uint64_t u = 0; std::memcpy(&u, &d, sizeof(u)); return u;
}
} // namespace
bool PlanKey::operator==(const PlanKey& o) const noexcept {
    if (objective != o.objective) return false;
    if (pattern_expression != o.pattern_expression) return false;
    if (pattern_options.kind != o.pattern_options.kind) return false;
    if (pattern_options.case_mode != o.pattern_options.case_mode) return false;
    if (pattern_options.engine != o.pattern_options.engine) return false;
    if (pattern_options.word != o.pattern_options.word) return false;
    if (pattern_options.line != o.pattern_options.line) return false;
    if (pattern_options.multiline != o.pattern_options.multiline) return false;
    if (pattern_options.dotall != o.pattern_options.dotall) return false;
    if (pattern_options.unicode != o.pattern_options.unicode) return false;
    if (pattern_options.crlf != o.pattern_options.crlf) return false;
    if (overlapping != o.overlapping) return false;
    if (invert_match != o.invert_match) return false;
    if (files_with_matches != o.files_with_matches) return false;
    if (files_without_match != o.files_without_match) return false;
    if (include_binary != o.include_binary) return false;
    if (max_matches != o.max_matches) return false;
    if (record_separator != o.record_separator) return false;
    if (eligible_file_ids != o.eligible_file_ids) return false;
    if (index_options.chunk_bytes != o.index_options.chunk_bytes) return false;
    if (index_options.chunk_overlap != o.index_options.chunk_overlap) return false;
    if (index_options.positional_block_bytes != o.index_options.positional_block_bytes) return false;
    if (double_bits(index_options.positional_budget_ratio) != double_bits(o.index_options.positional_budget_ratio)) return false;
    if (index_options.planned_qgrams != o.index_options.planned_qgrams) return false;
    if (index_options.include_hidden != o.index_options.include_hidden) return false;
    if (index_options.follow_symlinks != o.index_options.follow_symlinks) return false;
    if (index_options.persist_corpus != o.index_options.persist_corpus) return false;
    if (transformed_input_identity != o.transformed_input_identity) return false;
    return true;
}
std::uint64_t PlanKey::hash() const noexcept {
    uint64_t h = 14695981039346656037ULL;
    h = fnv_mix(h, static_cast<uint64_t>(objective));
    h = fnv_mix_str(h, pattern_expression);
    h = fnv_mix(h, static_cast<uint64_t>(pattern_options.kind));
    h = fnv_mix(h, static_cast<uint64_t>(pattern_options.case_mode));
    h = fnv_mix(h, static_cast<uint64_t>(pattern_options.engine));
    h = fnv_mix(h, pattern_options.word ? 1 : 0);
    h = fnv_mix(h, pattern_options.line ? 1 : 0);
    h = fnv_mix(h, pattern_options.multiline ? 1 : 0);
    h = fnv_mix(h, pattern_options.dotall ? 1 : 0);
    h = fnv_mix(h, pattern_options.unicode ? 1 : 0);
    h = fnv_mix(h, pattern_options.crlf ? 1 : 0);
    h = fnv_mix(h, overlapping ? 1 : 0);
    h = fnv_mix(h, invert_match ? 1 : 0);
    h = fnv_mix(h, files_with_matches ? 1 : 0);
    h = fnv_mix(h, files_without_match ? 1 : 0);
    h = fnv_mix(h, include_binary ? 1 : 0);
    h = fnv_mix(h, max_matches);
    h = fnv_mix(h, static_cast<uint64_t>(record_separator));
    h = fnv_mix(h, static_cast<uint64_t>(eligible_file_ids.size()));
    for (auto v : eligible_file_ids) h = fnv_mix(h, static_cast<uint64_t>(v));
    h = fnv_mix(h, static_cast<uint64_t>(index_options.chunk_bytes));
    h = fnv_mix(h, static_cast<uint64_t>(index_options.chunk_overlap));
    h = fnv_mix(h, static_cast<uint64_t>(index_options.positional_block_bytes));
    h = fnv_mix(h, double_bits(index_options.positional_budget_ratio));
    h = fnv_mix(h, static_cast<uint64_t>(index_options.planned_qgrams));
    h = fnv_mix(h, index_options.include_hidden ? 1 : 0);
    h = fnv_mix(h, index_options.follow_symlinks ? 1 : 0);
    h = fnv_mix(h, index_options.persist_corpus ? 1 : 0);
    h = fnv_mix(h, transformed_input_identity);
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL; h ^= h >> 33;
    return h;
}
std::string semantic_mode_key(const PlanKey& key) {
    // PlanKey::hash mixes the complete semantic and capability contract. Keep
    // the prefix explicit so serialized reports distinguish this key from
    // workload labels and never imply an observed execution.
    return std::string("plan-key:") + std::to_string(key.hash());
}
PlanKey make_plan_key(const Pattern& pattern, const SearchOptions& search_options, const Index& index, std::uint64_t transformed_input_identity) {
    return make_plan_key(pattern, search_options, index.options(), transformed_input_identity);
}
PlanKey make_plan_key(const Pattern& pattern, const SearchOptions& search_options, const IndexOptions& index_options, std::uint64_t transformed_input_identity) {
    PlanKey k;
    k.pattern_expression = pattern.expression();
    k.pattern_options = pattern.options();
    k.objective = search_options.objective;
    k.overlapping = search_options.overlapping;
    k.invert_match = search_options.invert_match;
    k.files_with_matches = search_options.files_with_matches;
    k.files_without_match = search_options.files_without_match;
    k.include_binary = search_options.include_binary;
    k.max_matches = search_options.max_matches;
    k.record_separator = search_options.record_separator;
    k.index_options = index_options;
    k.transformed_input_identity = transformed_input_identity;
    if (search_options.eligible_file_ids.data() && search_options.eligible_file_ids.size() > 0) {
        k.eligible_file_ids.assign(search_options.eligible_file_ids.begin(), search_options.eligible_file_ids.end());
        std::sort(k.eligible_file_ids.begin(), k.eligible_file_ids.end());
        k.eligible_file_ids.erase(std::unique(k.eligible_file_ids.begin(), k.eligible_file_ids.end()), k.eligible_file_ids.end());
    }
    return k;
}
// ---- QO-4: public cost-model wrappers (still inside namespace pergrep) ----
namespace detail {
double estimate_literal_selectivity(std::string_view lit, const IndexData& I) {
    return estimate_literal_selectivity_impl(lit, I);
}
double estimate_branch_selectivity(const std::vector<std::vector<std::string>>& branches, const IndexData& I) {
    return estimate_branch_selectivity_impl(branches, I);
}
} // namespace detail
std::string pick_rarest_branch_literal(const std::vector<std::vector<std::string>>& branches, const detail::IndexData& I) {
    if (branches.empty()) return {};
    std::string best;
    double best_sel = 2.0;
    for (const auto& br : branches) {
        for (const auto& lit : br) {
            double s = detail::estimate_literal_selectivity(lit, I);
            if (s < best_sel || (s == best_sel && lit.size() > best.size())) {
                best_sel = s;
                best = lit;
            }
        }
    }
    // If multiple branches, also consider per-branch rarest: pick the single
    // literal with minimal selectivity across all branches (global rarest).
    // This satisfies QO-4 test: rare literal vs common literal -> picks rare.
    return best;
}
detail::QueryCost estimateCost(const Pattern& p, const detail::IndexData& I, unsigned char record_separator) {
    detail::QueryCost qc;
    const auto total_chunks = I.chunks.size();
    const auto total_blocks = [&]{
        size_t n=0; for(auto& d:I.pos_desc) n+=d.blocks; return n;
    }();
    // Fixed patterns
    if (p.is_fixed()) {
        auto q = std::string_view(p.impl_->expr);
        bool icase = (p.impl_->opt.case_mode == CaseMode::Insensitive);
        double sel = estimate_literal_selectivity_impl(q, I);
        if (q.size() > I.opt.chunk_overlap || icase) sel = 1.0;
        qc.selectivity = sel;
        qc.estimated_candidate_chunks = total_chunks ? static_cast<uint64_t>(sel * total_chunks) : total_chunks;
        if (qc.estimated_candidate_chunks==0 && sel < 1.0 && total_chunks>0) qc.estimated_candidate_chunks = 1;
        qc.estimated_verified_bytes =
            (q.size() > I.opt.chunk_overlap || icase)
                ? scoped_extended_bytes(I, {})
                : qc.estimated_candidate_chunks * I.opt.chunk_bytes;
        if (p.is_fixed()) {
            PlanKey key;
            key.pattern_expression = p.impl_->expr;
            key.pattern_options = p.impl_->opt;
            key.record_separator = record_separator;
            key.index_options = I.opt;
            auto guarded = estimateCost(key, I);
            if (guarded.guarded_dispatch) return guarded;
        }
        // Branching similar to actual dispatch but with cost annotation
        // Cost model: FixedPositional cheaper when many blocks can be pruned (sel small)
        // and q small enough for positional (<=64) without word/line/icase.
        if (q.size() > I.opt.chunk_overlap) {
            qc.verifier = detail::VerifierKind::FixedRareByte;
            qc.estimated_candidate_blocks = 0; // whole-file scan
            qc.cost = double(qc.estimated_verified_bytes) + 100.0 * double(qc.estimated_candidate_chunks);
        } else if (icase || p.impl_->opt.word || p.impl_->opt.line) {
            qc.verifier = detail::VerifierKind::FixedRareByte;
            qc.estimated_candidate_blocks = 0;
            qc.cost = double(qc.estimated_verified_bytes) + 100.0 * double(qc.estimated_candidate_chunks);
        } else if (q.size() <= 64) {
            qc.verifier = detail::VerifierKind::FixedPositional;
            qc.estimated_candidate_blocks = static_cast<uint64_t>(sel * double(total_blocks));
            if (qc.estimated_candidate_blocks==0 && sel < 1.0 && total_blocks>0) qc.estimated_candidate_blocks = 1;
            qc.estimated_verified_bytes =
                qc.estimated_candidate_blocks * (I.pos_block + 64);
            qc.cost = double(qc.estimated_verified_bytes) * 0.5 + 50.0 * double(qc.estimated_candidate_blocks) + 10.0 * double(qc.estimated_candidate_chunks);
        } else {
            qc.verifier = detail::VerifierKind::FixedRareByte;
            qc.estimated_candidate_blocks = 0;
            qc.cost = double(qc.estimated_verified_bytes) + 100.0 * double(qc.estimated_candidate_chunks);
        }
        return qc;
    }
    // Regex patterns
    // is_pure_literal fast path: (multiline || !contains sep) matches actual find() dispatch to Fixed
    if (p.impl_->re.query_ir.is_pure_literal && p.impl_->re.groups == 0 && p.impl_->opt.case_mode != CaseMode::Insensitive && !p.impl_->re.extended) {
        bool sep_in_lit = p.impl_->re.query_ir.exact_literal.find(static_cast<char>(record_separator)) != std::string::npos;
        if (p.impl_->opt.multiline || !sep_in_lit) {
            PatternOptions fopt = p.impl_->opt;
            fopt.kind = PatternKind::Fixed;
            auto fixed_pat = Pattern::compile(p.impl_->re.query_ir.exact_literal, fopt);
            return estimateCost(fixed_pat, I, record_separator);
        }
    }
    // Regex with branch_mandatory or mandatory
    if (!p.impl_->re.query_ir.branch_mandatory.empty()) {
        double sel = estimate_branch_selectivity_impl(p.impl_->re.query_ir.branch_mandatory, I);
        qc.selectivity = sel;
        qc.verifier = detail::VerifierKind::RegexChunk;
        qc.estimated_candidate_chunks = total_chunks ? static_cast<uint64_t>(sel * total_chunks) : total_chunks;
        if (qc.estimated_candidate_chunks==0 && sel < 1.0) qc.estimated_candidate_chunks = 1;
        qc.estimated_verified_bytes = scoped_extended_bytes(I, {});
        qc.cost = double(qc.estimated_verified_bytes) + 200.0 * double(qc.estimated_candidate_chunks);
        return qc;
    }
    if (!p.impl_->re.query_ir.mandatory.empty()) {
        // Use rarest mandatory literal (longest is currently chosen for pruning, but cost uses rarest)
        double best = 1.0;
        for (auto& m : p.impl_->re.query_ir.mandatory) {
            double s = estimate_literal_selectivity_impl(m, I);
            if (s < best) best = s;
        }
        qc.selectivity = best;
        qc.verifier = detail::VerifierKind::RegexChunk;
        qc.estimated_candidate_chunks = total_chunks ? static_cast<uint64_t>(best * total_chunks) : total_chunks;
        if (qc.estimated_candidate_chunks==0 && best < 1.0) qc.estimated_candidate_chunks = 1;
        qc.estimated_verified_bytes = scoped_extended_bytes(I, {});
        qc.cost = double(qc.estimated_verified_bytes) + 200.0 * double(qc.estimated_candidate_chunks);
        return qc;
    }
    // No mandatory -> brute force (no pruning)
    qc.selectivity = 1.0;
    qc.verifier = detail::VerifierKind::RegexBruteForce;
    qc.estimated_candidate_chunks = I.chunks.size();
    qc.estimated_candidate_blocks = total_blocks;
    qc.estimated_verified_bytes = scoped_extended_bytes(I, {});
    qc.cost = double(qc.estimated_verified_bytes) + 200.0 * double(I.chunks.size());
    return qc;
}
detail::VerifierKind chooseVerifier(const Pattern& p, const detail::IndexData& I, unsigned char record_separator) {
    return estimateCost(p, I, record_separator).verifier;
}
// M1.3 PlanKey overloads: explicit contract, no default-assumption paths.
detail::QueryCost estimateCost(const PlanKey& key, const detail::IndexData& I) {
    detail::QueryCost qc;
    const auto& Kopt = key.index_options;
    const unsigned char sep = key.record_separator;
    const std::span<const std::uint32_t> scope(key.eligible_file_ids.data(), key.eligible_file_ids.size());
    bool is_fixed = (key.pattern_options.kind == PatternKind::Fixed);
    const auto total_chunks = scoped_chunk_count(I, scope);
    const auto total_blocks = [&]{
        size_t n = 0;
        for (std::size_t ci = 0; ci < I.pos_desc.size(); ++ci)
            if (scope.empty() || (ci < I.chunks.size() && scope_contains(scope, I.chunks[ci].file_id)))
                n += I.pos_desc[ci].blocks;
        return n;
    }();
    if (is_fixed) {
        auto q = std::string_view(key.pattern_expression);
        bool icase = (key.pattern_options.case_mode == CaseMode::Insensitive);
        double sel = estimate_literal_selectivity_impl(q, I, scope);
        if (q.size() > Kopt.chunk_overlap || icase) sel = 1.0;
        qc.selectivity = sel;
        qc.estimated_candidate_chunks = total_chunks ? static_cast<uint64_t>(sel * total_chunks) : total_chunks;
        if (qc.estimated_candidate_chunks == 0 && sel < 1.0 && total_chunks > 0) qc.estimated_candidate_chunks = 1;
        qc.estimated_verified_bytes =
            (q.size() > Kopt.chunk_overlap || icase)
                ? scoped_extended_bytes(I, scope)
                : qc.estimated_candidate_chunks * Kopt.chunk_bytes;

        if (fixed_guard_eligible(key, I)) {
            const auto costs = fixed_plan_costs(key, I);
            qc.guarded_dispatch = true;
            if (costs.whole_file < costs.chunk && costs.whole_file < costs.positional) {
                qc.verifier = detail::VerifierKind::FixedRareByte;
                qc.fixed_operator = detail::FixedPhysicalOperator::WholeFile;
                qc.estimated_candidate_blocks = 0;
                qc.estimated_verified_bytes = costs.whole_bytes;
                qc.cost = costs.whole_file;
            } else if (costs.chunk < costs.positional) {
                qc.verifier = detail::VerifierKind::FixedRareByte;
                qc.fixed_operator = detail::FixedPhysicalOperator::Chunk;
                qc.estimated_candidate_blocks = 0;
                qc.estimated_verified_bytes = costs.chunk_bytes;
                qc.cost = costs.chunk;
            } else {
                qc.verifier = detail::VerifierKind::FixedPositional;
                qc.fixed_operator = detail::FixedPhysicalOperator::PositionalBlock;
                qc.estimated_candidate_blocks = costs.blocks;
                qc.estimated_verified_bytes = costs.positional_bytes;
                qc.cost = costs.positional;
            }
            return qc;
        }

        // Outside the guarded M1.7 contract preserve the established estimate
        // labels, even when execution has to use a conservative fallback.
        if (q.size() > Kopt.chunk_overlap) {
            qc.verifier = detail::VerifierKind::FixedRareByte;
            qc.fixed_operator = detail::FixedPhysicalOperator::WholeFile;
            qc.estimated_candidate_blocks = 0;
            qc.cost = double(qc.estimated_verified_bytes) + 100.0 * double(qc.estimated_candidate_chunks);
        } else if (icase || key.pattern_options.word || key.pattern_options.line) {
            qc.verifier = detail::VerifierKind::FixedRareByte;
            qc.fixed_operator = detail::FixedPhysicalOperator::Chunk;
            qc.estimated_candidate_blocks = 0;
            qc.cost = double(qc.estimated_verified_bytes) + 100.0 * double(qc.estimated_candidate_chunks);
        } else if (q.size() <= 64) {
            qc.verifier = detail::VerifierKind::FixedPositional;
            qc.fixed_operator = detail::FixedPhysicalOperator::PositionalBlock;
            qc.estimated_candidate_blocks = static_cast<uint64_t>(sel * double(total_blocks));
            if (qc.estimated_candidate_blocks == 0 && sel < 1.0 && total_blocks > 0) qc.estimated_candidate_blocks = 1;
            qc.estimated_verified_bytes = qc.estimated_candidate_blocks * (I.pos_block + 64);
            qc.cost = double(qc.estimated_verified_bytes) * 0.5 + 50.0 * double(qc.estimated_candidate_blocks) + 10.0 * double(qc.estimated_candidate_chunks);
        } else {
            qc.verifier = detail::VerifierKind::FixedRareByte;
            qc.fixed_operator = detail::FixedPhysicalOperator::Chunk;
            qc.estimated_candidate_blocks = 0;
            qc.cost = double(qc.estimated_verified_bytes) + 100.0 * double(qc.estimated_candidate_chunks);
        }
        if (key.invert_match || key.files_with_matches || key.files_without_match) qc.cost += 1000.0;
        if (key.overlapping) qc.cost += 500.0;
        if (key.max_matches != 0) qc.cost = std::min(qc.cost, double(key.max_matches * 1024));
        if (!key.eligible_file_ids.empty()) {
            const double ratio = double(total_chunks) / double(std::max<std::size_t>(I.chunks.size(), 1));
            qc.cost *= (0.5 + 0.5 * ratio);
        }
        qc.cost += (key.include_binary ? 250.0 : 0) + double(key.transformed_input_identity & 0xFF);
        return qc;
    }
    Pattern tmp;
    try { tmp = Pattern::compile(key.pattern_expression, key.pattern_options); } catch (...) {
        qc.selectivity = 1.0; qc.verifier = detail::VerifierKind::RegexBruteForce;
        qc.estimated_candidate_chunks = total_chunks; qc.estimated_candidate_blocks = total_blocks;
        qc.estimated_verified_bytes = total_chunks * I.opt.chunk_bytes;
        qc.cost = double(qc.estimated_verified_bytes) + 200.0 * double(total_chunks);
        return qc;
    }
    if (tmp.impl_->re.query_ir.is_pure_literal && tmp.impl_->re.groups == 0 && key.pattern_options.case_mode != CaseMode::Insensitive && !tmp.impl_->re.extended) {
        bool sep_in_lit = tmp.impl_->re.query_ir.exact_literal.find(static_cast<char>(sep)) != std::string::npos;
        if (key.pattern_options.multiline || !sep_in_lit) {
            PatternOptions fopt = key.pattern_options; fopt.kind = PatternKind::Fixed;
            auto fixed_pat = Pattern::compile(tmp.impl_->re.query_ir.exact_literal, fopt);
            PlanKey fixed_key = key; fixed_key.pattern_expression = fixed_pat.expression(); fixed_key.pattern_options = fopt;
            return estimateCost(fixed_key, I);
        }
    }
    if (!tmp.impl_->re.query_ir.branch_mandatory.empty()) {
        double sel = estimate_branch_selectivity_impl(tmp.impl_->re.query_ir.branch_mandatory, I, scope);
        qc.selectivity = sel; qc.verifier = detail::VerifierKind::RegexChunk;
        qc.estimated_candidate_chunks = total_chunks ? static_cast<uint64_t>(sel * total_chunks) : total_chunks;
        if (qc.estimated_candidate_chunks == 0 && sel < 1.0 && total_chunks > 0) qc.estimated_candidate_chunks = 1;
        qc.estimated_verified_bytes = scoped_extended_bytes(I, scope);
        qc.cost = double(qc.estimated_verified_bytes) + 200.0 * double(qc.estimated_candidate_chunks);
    } else if (!tmp.impl_->re.query_ir.mandatory.empty()) {
        double best = 1.0;
        for (auto& m : tmp.impl_->re.query_ir.mandatory)
            best = std::min(best, estimate_literal_selectivity_impl(m, I, scope));
        qc.selectivity = best; qc.verifier = detail::VerifierKind::RegexChunk;
        qc.estimated_candidate_chunks = total_chunks ? static_cast<uint64_t>(best * total_chunks) : total_chunks;
        if (qc.estimated_candidate_chunks == 0 && best < 1.0 && total_chunks > 0) qc.estimated_candidate_chunks = 1;
        qc.estimated_verified_bytes = scoped_extended_bytes(I, scope);
        qc.cost = double(qc.estimated_verified_bytes) + 200.0 * double(qc.estimated_candidate_chunks);
    } else {
        qc.selectivity = 1.0; qc.verifier = detail::VerifierKind::RegexBruteForce;
        qc.estimated_candidate_chunks = total_chunks; qc.estimated_candidate_blocks = total_blocks;
        qc.estimated_verified_bytes = scoped_extended_bytes(I, scope);
        qc.cost = double(qc.estimated_verified_bytes) + 200.0 * double(total_chunks);
    }
    if (key.invert_match || key.files_with_matches || key.files_without_match) qc.cost += 1000.0;
    if (key.overlapping) qc.cost += 500.0;
    if (key.max_matches != 0) qc.cost = std::min(qc.cost, double(key.max_matches * 1024));
    if (!key.eligible_file_ids.empty()) {
        const double ratio = double(total_chunks) / double(std::max<std::size_t>(I.chunks.size(), 1));
        qc.cost *= (0.5 + 0.5 * ratio);
    }
    qc.cost += (key.include_binary ? 250.0 : 0) + double(key.transformed_input_identity & 0xFF);
    return qc;
}
detail::VerifierKind chooseVerifier(const PlanKey& key, const detail::IndexData& I) {
    return estimateCost(key, I).verifier;
}
namespace detail {
std::vector<PlanCandidateMetrics> estimate_all_candidate_plans(const Pattern& p, const detail::IndexData& I, unsigned char record_separator) {
    std::vector<PlanCandidateMetrics> candidates;
    const auto total_chunks = I.chunks.size();

    auto chosen_cost = estimateCost(p, I, record_separator);

    if (p.is_fixed()) {
        PlanKey key;
        key.pattern_expression = p.expression();
        key.pattern_options = p.options();
        key.record_separator = record_separator;
        key.index_options = I.opt;
        return estimate_all_candidate_plans(key, I);
    } else {
        // Regex patterns
        if (p.impl_ && p.impl_->re.query_ir.is_pure_literal && p.impl_->re.groups == 0 && p.impl_->opt.case_mode != CaseMode::Insensitive && !p.impl_->re.extended) {
            bool sep_in_lit = p.impl_->re.query_ir.exact_literal.find(static_cast<char>(record_separator)) != std::string::npos;
            if (p.impl_->opt.multiline || !sep_in_lit) {
                PatternOptions fopt = p.impl_->opt;
                fopt.kind = PatternKind::Fixed;
                auto fixed_pat = Pattern::compile(p.impl_->re.query_ir.exact_literal, fopt);
                return estimate_all_candidate_plans(fixed_pat, I, record_separator);
            }
        }

        if (p.impl_ && (!p.impl_->re.query_ir.branch_mandatory.empty() || !p.impl_->re.query_ir.mandatory.empty())) {
            double sel = chosen_cost.selectivity;
            uint64_t est_chunks = chosen_cost.estimated_candidate_chunks;
            uint64_t est_bytes = chosen_cost.estimated_verified_bytes;
            PlanCandidateMetrics c;
            c.name = "RegexChunk";
            c.verifier = pergrep::VerifierKind::RegexChunk;
            c.predicted_selectivity = sel;
            c.predicted_cost = double(est_bytes) + 200.0 * double(est_chunks);
            c.is_fallback = false;
            c.chosen = (chosen_cost.verifier == detail::VerifierKind::RegexChunk);
            c.actual_observed = false;
            candidates.push_back(c);
        }

        {
            PlanCandidateMetrics c;
            c.name = "RegexBruteForce";
            c.verifier = pergrep::VerifierKind::RegexBruteForce;
            c.predicted_selectivity = 1.0;
            c.predicted_cost = double(total_chunks * I.opt.chunk_bytes) + 200.0 * double(total_chunks);
            c.is_fallback = true;
            c.chosen = (chosen_cost.verifier == detail::VerifierKind::RegexBruteForce);
            c.actual_observed = false;
            candidates.push_back(c);
        }
    }
    return candidates;
}
} // namespace detail
namespace detail {
std::vector<PlanCandidateMetrics> estimate_all_candidate_plans(const PlanKey& key, const IndexData& I) {
    std::vector<PlanCandidateMetrics> candidates;
    const std::span<const std::uint32_t> scope(key.eligible_file_ids.data(), key.eligible_file_ids.size());
    const auto total_chunks = scoped_chunk_count(I, scope);
    const auto total_blocks = [&]{
        std::size_t n = 0;
        for (std::size_t i = 0; i < I.pos_desc.size(); ++i)
            if (i < I.chunks.size() && scope_contains(scope, I.chunks[i].file_id)) n += I.pos_desc[i].blocks;
        return n;
    }();
    auto chosen_cost = estimateCost(key, I);
    bool is_fixed = (key.pattern_options.kind == PatternKind::Fixed);
    if (is_fixed) {
        auto q = std::string_view(key.pattern_expression);
        const bool icase = (key.pattern_options.case_mode == CaseMode::Insensitive);
        const double sel = estimate_literal_selectivity_impl(q, I, scope);
        const bool guarded = fixed_guard_eligible(key, I);
        FixedPlanCosts costs;
        if (guarded) costs = fixed_plan_costs(key, I);
        std::uint64_t est_chunks = total_chunks ? static_cast<std::uint64_t>(sel * total_chunks) : total_chunks;
        if (est_chunks == 0 && sel < 1.0 && total_chunks > 0) est_chunks = 1;
        const double est_bytes = guarded ? double(std::min(costs.chunk_bytes, costs.whole_bytes)) : double(est_chunks) * I.opt.chunk_bytes;
        {
            PlanCandidateMetrics c; c.name = "FixedRareByte"; c.verifier = pergrep::VerifierKind::FixedRareByte;
            c.predicted_selectivity = sel;
            c.predicted_cost = guarded ? std::min(costs.chunk, costs.whole_file) : est_bytes + 100.0 * est_chunks;
            c.is_fallback = !guarded && (q.size() > key.index_options.chunk_overlap || icase || key.pattern_options.word || key.pattern_options.line);
            c.chosen = (chosen_cost.verifier == detail::VerifierKind::FixedRareByte);
            c.actual_observed = false; candidates.push_back(c);
        }
        if ((guarded || (q.size() <= 64 && !icase && !key.pattern_options.word && !key.pattern_options.line))) {
            PlanCandidateMetrics c; c.name = "FixedPositional"; c.verifier = pergrep::VerifierKind::FixedPositional;
            c.predicted_selectivity = sel;
            std::uint64_t est_blocks = static_cast<std::uint64_t>(sel * double(total_blocks));
            if (est_blocks == 0 && sel < 1.0 && total_blocks > 0) est_blocks = 1;
            const auto positional_bytes = guarded ? costs.positional_bytes : est_blocks * (I.pos_block + 64);
            c.predicted_cost = guarded ? costs.positional : double(positional_bytes) * 0.5 + 50.0 * double(est_blocks) + 10.0 * est_chunks;
            c.is_fallback = false;
            c.chosen = (chosen_cost.verifier == detail::VerifierKind::FixedPositional);
            c.actual_observed = false; candidates.push_back(c);
        }
        {
            PlanCandidateMetrics c; c.name = "RegexBruteForce"; c.verifier = pergrep::VerifierKind::RegexBruteForce;
            c.predicted_cost = double(total_chunks * I.opt.chunk_bytes) + 200.0 * double(total_chunks);
            c.is_fallback = true; c.chosen = (chosen_cost.verifier == detail::VerifierKind::RegexBruteForce);
            c.actual_observed = false; candidates.push_back(c);
        }
    } else {
        Pattern tmp; try { tmp = Pattern::compile(key.pattern_expression, key.pattern_options); } catch(...) {
            PlanCandidateMetrics c; c.name="RegexBruteForce"; c.verifier=pergrep::VerifierKind::RegexBruteForce;
            c.predicted_cost=double(total_chunks * I.opt.chunk_bytes)+200.0*double(total_chunks);
            c.is_fallback=true; c.chosen=true; c.actual_observed=false; candidates.push_back(c);
            return candidates;
        }
        if (tmp.impl_->re.query_ir.is_pure_literal && tmp.impl_->re.groups == 0 && key.pattern_options.case_mode != CaseMode::Insensitive && !tmp.impl_->re.extended) {
            bool sep_in_lit = tmp.impl_->re.query_ir.exact_literal.find(static_cast<char>(key.record_separator)) != std::string::npos;
            if (key.pattern_options.multiline || !sep_in_lit) {
                PatternOptions fopt = key.pattern_options; fopt.kind = PatternKind::Fixed;
                auto fixed_pat = Pattern::compile(tmp.impl_->re.query_ir.exact_literal, fopt);
                PlanKey fixed_key = key; fixed_key.pattern_expression = fixed_pat.expression(); fixed_key.pattern_options = fopt;
                return estimate_all_candidate_plans(fixed_key, I);
            }
        }
        if (tmp.impl_ && (!tmp.impl_->re.query_ir.branch_mandatory.empty() || !tmp.impl_->re.query_ir.mandatory.empty())) {
            double sel = chosen_cost.selectivity; uint64_t est_chunks = chosen_cost.estimated_candidate_chunks; uint64_t est_bytes = chosen_cost.estimated_verified_bytes;
            PlanCandidateMetrics c; c.name="RegexChunk"; c.verifier=pergrep::VerifierKind::RegexChunk;
            c.predicted_selectivity=sel; c.predicted_cost=double(est_bytes)+200.0*double(est_chunks);
            c.is_fallback=false; c.chosen=(chosen_cost.verifier==detail::VerifierKind::RegexChunk); c.actual_observed=false; candidates.push_back(c);
        }
        {
            PlanCandidateMetrics c; c.name="RegexBruteForce"; c.verifier=pergrep::VerifierKind::RegexBruteForce;
            c.predicted_cost=double(total_chunks * I.opt.chunk_bytes)+200.0*double(total_chunks);
            c.is_fallback=true; c.chosen=(chosen_cost.verifier==detail::VerifierKind::RegexBruteForce); c.actual_observed=false; candidates.push_back(c);
        }
    }
    for (auto& c : candidates) {
        if (key.invert_match || key.files_with_matches || key.files_without_match) c.predicted_cost += 1000.0;
        if (key.overlapping) c.predicted_cost += 500.0;
        if (key.max_matches != 0) c.predicted_cost = std::min(c.predicted_cost, double(key.max_matches * 1024));
        if (!key.eligible_file_ids.empty()) {
            double ratio = double(key.eligible_file_ids.size())/double(std::max<size_t>(I.infos.size(),1));
            c.predicted_cost *= (0.5 + 0.5 * ratio);
        }
        c.predicted_cost += (key.include_binary ? 250.0 : 0) + double(key.transformed_input_identity & 0xFF);
    }
    return candidates;
}
} // namespace detail

std::vector<PlanCandidateMetrics> estimate_candidate_plans(const PlanKey& key, const Index& index) {
    if (!index.debug_index_data()) return {};
    const auto& I = *static_cast<const detail::IndexData*>(index.debug_index_data());
    return detail::estimate_all_candidate_plans(key, I);
}

std::vector<PlanCandidateMetrics> estimate_candidate_plans(const Pattern& pattern, const Index& index, unsigned char record_separator) {
    if (!index.debug_index_data()) return {};
    const auto& I = *static_cast<const detail::IndexData*>(index.debug_index_data());
    return detail::estimate_all_candidate_plans(pattern, I, record_separator);
}

PlanRegret compute_plan_regret(const PlanCandidateMetrics& chosen,
                               const std::vector<PlanCandidateMetrics>& candidates,
                               std::string query_name) {
    PlanRegret regret;
    regret.query_name = std::move(query_name);
    regret.chosen_plan = chosen.name;
    regret.chosen_verifier = chosen.verifier;
    regret.predicted_cost = chosen.predicted_cost;
    regret.actual_cost = chosen.actual_cost;
    regret.is_fallback = chosen.is_fallback;
    regret.candidates = candidates;
    // Canonical ordering makes the serialized report independent of caller order.
    std::sort(regret.candidates.begin(), regret.candidates.end(), [](const auto& a, const auto& b) {
        if (a.verifier != b.verifier) return static_cast<unsigned>(a.verifier) < static_cast<unsigned>(b.verifier);
        if (a.name != b.name) return a.name < b.name;
        if (a.predicted_cost != b.predicted_cost) return a.predicted_cost < b.predicted_cost;
        return a.chosen > b.chosen;
    });
    regret.candidate_count = regret.candidates.size();

    // The chosen argument is the single observed chosen plan by definition
    std::vector<const PlanCandidateMetrics*> observed;
    observed.push_back(&chosen);

    // Collect only other candidates that are observed, excluding the same plan (name + verifier)
    for (const auto& cand : candidates) {
        if (cand.name == chosen.name && cand.verifier == chosen.verifier) {
            continue;
        }
        const bool is_obs = cand.actual_observed ||
                            cand.observation == PlanCandidateMetrics::ObservationStatus::Observed;
        if (is_obs) {
            observed.push_back(&cand);
        }
    }

    regret.observed_candidate_count = observed.size();
    // If fewer than two observed candidates, report zero regret and no rank inversion
    constexpr double kEpsilon = 1e-9;
    if (observed.size() < 2) {
        regret.optimal_plan = chosen.name;
        regret.optimal_verifier = chosen.verifier;
        regret.optimal_actual_cost = chosen.actual_cost;
        regret.absolute_regret = 0.0;
        regret.relative_regret = 0.0;
        regret.prediction_error = std::abs(chosen.predicted_cost - chosen.actual_cost) /
                                  std::max(chosen.actual_cost, kEpsilon);
        regret.is_suboptimal = false;
        regret.rank_inversions = 0;
        return regret;
    }

    const PlanCandidateMetrics* best = observed[0];
    for (const auto* cand : observed) {
        if (cand->actual_cost < best->actual_cost) {
            best = cand;
        }
    }

    regret.optimal_plan = best->name;
    regret.optimal_verifier = best->verifier;
    regret.optimal_actual_cost = best->actual_cost;
    regret.absolute_regret = std::max(0.0, chosen.actual_cost - best->actual_cost);
    regret.relative_regret = (chosen.actual_cost - best->actual_cost) /
                             std::max(best->actual_cost, kEpsilon);
    regret.prediction_error = std::abs(chosen.predicted_cost - chosen.actual_cost) /
                              std::max(chosen.actual_cost, kEpsilon);
    regret.is_suboptimal = (regret.absolute_regret > 1e-6);
    regret.observed_fallback_loss = regret.is_suboptimal && !chosen.is_fallback && best->is_fallback;

    std::size_t inversions = 0;
    for (std::size_t i = 0; i < observed.size(); ++i) {
        for (std::size_t j = i + 1; j < observed.size(); ++j) {
            const auto* a = observed[i];
            const auto* b = observed[j];
            bool pred_a_better = a->predicted_cost < b->predicted_cost;
            bool act_a_better = a->actual_cost < b->actual_cost;
            if (pred_a_better != act_a_better &&
                std::abs(a->predicted_cost - b->predicted_cost) > 1e-6 &&
                std::abs(a->actual_cost - b->actual_cost) > 1e-6) {
                ++inversions;
            }
        }
    }
    regret.rank_inversions = inversions;
    return regret;
}

static double internal_percentile(std::vector<double> samples, double p) {
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

ShadowPlanReport evaluate_shadow_plans(const std::vector<PlanRegret>& query_regrets) {
    ShadowPlanReport report;
    report.total_queries = query_regrets.size();
    report.query_regrets = query_regrets;
    std::sort(report.query_regrets.begin(), report.query_regrets.end(), [](const auto& a, const auto& b) {
        if (a.workload_key != b.workload_key) return a.workload_key < b.workload_key;
        if (a.semantic_mode != b.semantic_mode) return a.semantic_mode < b.semantic_mode;
        if (a.query_name != b.query_name) return a.query_name < b.query_name;
        if (a.plan_key_hash != b.plan_key_hash) return a.plan_key_hash < b.plan_key_hash;
        return a.chosen_plan < b.chosen_plan;
    });
    if (query_regrets.empty()) return report;

    std::vector<double> regrets;
    regrets.reserve(query_regrets.size());
    std::vector<double> errors;
    errors.reserve(query_regrets.size());

    double sum_regret = 0.0;
    double sum_error = 0.0;
    double max_r = 0.0;

    for (const auto& r : report.query_regrets) {
        if (r.is_suboptimal) ++report.suboptimal_plan_count;
        if (r.is_fallback) ++report.fallback_count;
        if (r.observed_candidate_count > 0) ++report.observed_query_count;
        if (r.observed_fallback_loss) ++report.measured_fallback_loss_count;
        sum_regret += r.relative_regret;
        sum_error += r.prediction_error;
        report.total_excess_cost += r.absolute_regret;
        regrets.push_back(r.relative_regret);
        errors.push_back(r.prediction_error);
        if (r.relative_regret > max_r) max_r = r.relative_regret;

        const bool new_group = report.groups.empty() ||
            report.groups.back().workload_key != r.workload_key ||
            report.groups.back().semantic_mode != r.semantic_mode;
        if (new_group) {
            report.groups.push_back({r.workload_key, r.semantic_mode, 0, 0, 0, 0.0});
        }
        auto& group = report.groups.back();
        ++group.query_count;
        if (r.observed_candidate_count > 0) ++group.observed_query_count;
        if (r.is_suboptimal) ++group.suboptimal_plan_count;
        group.mean_regret += r.relative_regret;
    }
    for (auto& group : report.groups) {
        if (group.query_count) group.mean_regret /= static_cast<double>(group.query_count);
    }

    report.fallback_rate = static_cast<double>(report.fallback_count) / static_cast<double>(report.total_queries);
    report.mean_regret = sum_regret / static_cast<double>(report.total_queries);
    report.mean_prediction_error = sum_error / static_cast<double>(report.total_queries);
    report.max_regret = max_r;

    report.p50_regret = internal_percentile(regrets, 0.50);
    report.p95_regret = internal_percentile(regrets, 0.95);
    report.p95_prediction_error = internal_percentile(errors, 0.95);

    return report;
}

GateEvaluation evaluate_performance_gate(
    const std::vector<ScenarioGateVerdict>& scenario_verdicts,
    const PerformanceGateThresholds& thresholds,
    const ShadowPlanReport& shadow_report) {
    GateEvaluation eval;
    eval.scenario_verdicts = scenario_verdicts;
    eval.shadow_report = shadow_report;

    bool has_fail = false;
    bool has_warn = false;
    bool has_rollback = false;

    for (const auto& sc : scenario_verdicts) {
        if (sc.classification == WorkloadClassification::Win) ++eval.wins_count;
        else if (sc.classification == WorkloadClassification::Neutral) ++eval.neutral_count;
        else if (sc.classification == WorkloadClassification::Regression) ++eval.regressions_count;

        if (!sc.correctness_pass && thresholds.require_correctness_pass) {
            has_rollback = true;
            eval.rollback_reasons.push_back(sc.scenario_name + ": Correctness check failed (require_correctness_pass violated)");
        }

        if (sc.status == GateStatus::Rollback) {
            has_rollback = true;
            for (const auto& v : sc.violations) eval.rollback_reasons.push_back(sc.scenario_name + ": " + v);
        } else if (sc.status == GateStatus::Fail) {
            has_fail = true;
            for (const auto& v : sc.violations) eval.failure_reasons.push_back(sc.scenario_name + ": " + v);
        } else if (sc.status == GateStatus::Warn) {
            has_warn = true;
            for (const auto& w : sc.warnings) eval.warning_reasons.push_back(sc.scenario_name + ": " + w);
        }
    }

    // Shadow planner aggregate checks
    if (shadow_report.total_queries > 0) {
        const double subopt_ratio = static_cast<double>(shadow_report.suboptimal_plan_count) / static_cast<double>(shadow_report.total_queries);
        if (subopt_ratio > thresholds.max_suboptimal_plan_ratio) {
            has_fail = true;
            eval.failure_reasons.push_back("Shadow evaluation suboptimal plan ratio (" +
                std::to_string(subopt_ratio * 100.0) + "%) exceeds threshold (" +
                std::to_string(thresholds.max_suboptimal_plan_ratio * 100.0) + "%)");
        }

        if (shadow_report.fallback_rate > thresholds.rollback_fallback_rate) {
            has_rollback = true;
            eval.rollback_reasons.push_back("Shadow evaluation fallback rate (" +
                std::to_string(shadow_report.fallback_rate * 100.0) + "%) exceeds rollback limit (" +
                std::to_string(thresholds.rollback_fallback_rate * 100.0) + "%)");
        } else if (shadow_report.fallback_rate > thresholds.max_fallback_rate) {
            has_fail = true;
            eval.failure_reasons.push_back("Shadow evaluation fallback rate (" +
                std::to_string(shadow_report.fallback_rate * 100.0) + "%) exceeds threshold (" +
                std::to_string(thresholds.max_fallback_rate * 100.0) + "%)");
        }

        if (shadow_report.mean_regret > thresholds.rollback_mean_regret_ratio) {
            has_rollback = true;
            eval.rollback_reasons.push_back("Shadow evaluation mean regret (" +
                std::to_string(shadow_report.mean_regret * 100.0) + "%) exceeds rollback limit (" +
                std::to_string(thresholds.rollback_mean_regret_ratio * 100.0) + "%)");
        } else if (shadow_report.mean_regret > thresholds.max_mean_regret_ratio) {
            has_fail = true;
            eval.failure_reasons.push_back("Shadow evaluation mean regret (" +
                std::to_string(shadow_report.mean_regret * 100.0) + "%) exceeds threshold (" +
                std::to_string(thresholds.max_mean_regret_ratio * 100.0) + "%)");
        }

        if (shadow_report.p95_regret > thresholds.max_p95_regret_ratio) {
            has_fail = true;
            eval.failure_reasons.push_back("Shadow evaluation p95 regret (" +
                std::to_string(shadow_report.p95_regret * 100.0) + "%) exceeds threshold (" +
                std::to_string(thresholds.max_p95_regret_ratio * 100.0) + "%)");
        }
    }
    if (has_rollback) {
        eval.overall_status = GateStatus::Rollback;
        eval.passed = false;
        eval.rollback_triggered = true;
    } else if (has_fail) {
        eval.overall_status = GateStatus::Fail;
        eval.passed = false;
        eval.rollback_triggered = false;
    } else if (has_warn) {
        eval.overall_status = GateStatus::Warn;
        eval.passed = true;
        eval.rollback_triggered = false;
    } else {
        eval.overall_status = GateStatus::Pass;
        eval.passed = true;
        eval.rollback_triggered = false;
    }

    return eval;
}

std::string GateEvaluation::format_release_report() const {
    std::string out;
    out += "# Performance & Plan Regret Release Gate Report\n\n";
    out += "**Overall Status**: " + std::string(to_string(overall_status)) + "\n";
    out += "**Gate Passed**: " + std::string(passed ? "YES" : "NO") + "\n";
    out += "**Rollback Triggered**: " + std::string(rollback_triggered ? "YES" : "NO") + "\n\n";

    out += "## Workload Overview\n";
    out += "- **Total Scenarios**: " + std::to_string(scenario_verdicts.size()) + "\n";
    out += "- **Wins**: " + std::to_string(wins_count) + "\n";
    out += "- **Neutral**: " + std::to_string(neutral_count) + "\n";
    out += "- **Regressions**: " + std::to_string(regressions_count) + "\n\n";

    out += "## Scenario Breakdown\n\n";
    out += "| Scenario | Class | Status | Classif | p50 (ms) | p50 Base | Delta p50 | p95 (ms) | p95 Base | Delta p95 | Fallback % | Regret % |\n";
    out += "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |\n";
    for (const auto& sc : scenario_verdicts) {
        char buf[512];
        double p50_diff = (sc.p50_ratio - 1.0) * 100.0;
        double p95_diff = (sc.p95_ratio - 1.0) * 100.0;
        snprintf(buf, sizeof(buf), "| `%s` | %s | **%s** | %s | %.3f | %.3f | %+.1f%% | %.3f | %.3f | %+.1f%% | %.1f%% | %.1f%% |\n",
            sc.scenario_name.c_str(),
            sc.workload_class.c_str(),
            to_string(sc.status),
            to_string(sc.classification),
            sc.p50_ms,
            sc.baseline_p50_ms,
            p50_diff,
            sc.p95_ms,
            sc.baseline_p95_ms,
            p95_diff,
            sc.fallback_rate * 100.0,
            sc.mean_regret * 100.0
        );
        out += buf;
    }
    out += "\n";

    if (shadow_report.total_queries > 0) {
        out += "## Shadow Planner & Plan Regret Summary\n";
        out += "- **Total Queries Evaluated**: " + std::to_string(shadow_report.total_queries) + "\n";
        out += "- **Suboptimal Plan Selections**: " + std::to_string(shadow_report.suboptimal_plan_count) + "\n";
        out += "- **Fallback Invocations**: " + std::to_string(shadow_report.fallback_count) + "\n";
        char sbuf[256];
        snprintf(sbuf, sizeof(sbuf), "- **Fallback Rate**: %.2f%%\n- **Mean Relative Regret**: %.2f%%\n- **P50 Relative Regret**: %.2f%%\n- **P95 Relative Regret**: %.2f%%\n- **Max Relative Regret**: %.2f%%\n- **P95 Prediction Error**: %.2f%%\n- **Total Excess Cost**: %.1f\n\n",
            shadow_report.fallback_rate * 100.0,
            shadow_report.mean_regret * 100.0,
            shadow_report.p50_regret * 100.0,
            shadow_report.p95_regret * 100.0,
            shadow_report.max_regret * 100.0,
            shadow_report.p95_prediction_error * 100.0,
            shadow_report.total_excess_cost
        );
        out += sbuf;
    }

    if (!rollback_reasons.empty()) {
        out += "## Rollback Triggers Fired\n";
        for (const auto& r : rollback_reasons) {
            out += "- **ROLLBACK**: " + r + "\n";
        }
        out += "\n";
    }

    if (!failure_reasons.empty()) {
        out += "## Gate Failures\n";
        for (const auto& f : failure_reasons) {
            out += "- **FAIL**: " + f + "\n";
        }
        out += "\n";
    }

    if (!warning_reasons.empty()) {
        out += "## Warnings & Advisories\n";
        for (const auto& w : warning_reasons) {
            out += "- **WARN**: " + w + "\n";
        }
        out += "\n";
    }

    return out;
}

Searcher::Searcher(std::shared_ptr<const Index> i) : owned_(std::move(i)), index_(owned_.get()) {}
Searcher::Searcher(const Index& i) : index_(&i) {}

std::vector<Match> Searcher::find(const Pattern& p, SearchOptions opt, SearchStats* stats) const {
    if (!index_ || !index_->impl_) throw std::runtime_error("pergrep: empty index");
    if (stats) *stats = {};
    auto& I = *index_->impl_;
    std::vector<std::uint32_t> normalized_scope;
    if (!opt.eligible_file_ids.empty()) {
        normalized_scope.assign(opt.eligible_file_ids.begin(), opt.eligible_file_ids.end());
        std::sort(normalized_scope.begin(), normalized_scope.end());
        normalized_scope.erase(std::unique(normalized_scope.begin(), normalized_scope.end()), normalized_scope.end());
        opt.eligible_file_ids = normalized_scope;
    }
    StatsRecorder accounting(stats, I, opt.eligible_file_ids);
    if (stats) {
        const auto effective_objective =
            opt.objective == SearchObjective::Exhaustive && opt.max_matches != 0
                ? SearchObjective::OrderedPrefix : opt.objective;
        stats->objective = to_string(effective_objective);
        stats->candidate_order = "file-id,offset";
        stats->candidate_order_preserved = true;
        stats->configured_planned_qgrams = static_cast<std::uint64_t>(I.opt.planned_qgrams);
        stats->qgram_fallback_reason = "none";
    }
    std::vector<Match> out;
    const auto search_start = std::chrono::steady_clock::now();
    const auto total_blocks = [&] {
        std::size_t n = 0;
        for (std::size_t ci = 0; ci < I.pos_desc.size(); ++ci)
            if (opt.eligible_file_ids.empty() ||
                (ci < I.chunks.size() && scope_contains(opt.eligible_file_ids, I.chunks[ci].file_id)))
                n += I.pos_desc[ci].blocks;
        return n;
    }();
    const std::span<const std::uint32_t> scope(opt.eligible_file_ids.data(), opt.eligible_file_ids.size());
    // Planner statistics estimate candidate work in chunk/block/byte units;
    // conservative hash-bucket bounds are never used to reject candidates.
    PlanKey plan_key = make_plan_key(p, opt, I.opt, 0);
    auto qc = estimateCost(plan_key, I);
    if (stats && p.is_fixed()) {
        const auto q = std::string_view(p.impl_->expr);
        const auto selected = planned_hashes(I, q);
        stats->effective_k = static_cast<std::uint64_t>(adaptive_k(I, q));
        stats->selected_qgram_count = static_cast<std::uint64_t>(selected.size());
        stats->selected_qgram_rows = static_cast<std::uint64_t>(selected.size());
        if (q.size() < 4) stats->qgram_fallback_reason = "query-fewer-than-4-bytes";
    } else if (stats) {
        stats->qgram_fallback_reason = "not-fixed-literal";
    }
    const bool guarded_fixed_dispatch = !suppress_guarded_fixed_dispatch && p.is_fixed() &&
                                        qc.guarded_dispatch && fixed_guard_eligible(plan_key, I);
    if (stats) {
        stats->verifier = std::string(detail::to_string(qc.verifier));
        stats->guarded_dispatch_used = guarded_fixed_dispatch;
        if (!p.is_fixed()) {
            stats->physical_operator = stats->verifier;
        } else if (opt.invert_match) {
            stats->physical_operator = "FixedRecordWrapper";
        } else if (qc.fixed_operator == detail::FixedPhysicalOperator::PositionalBlock) {
            stats->physical_operator = "FixedPositional";
        } else if (qc.fixed_operator == detail::FixedPhysicalOperator::Chunk) {
            stats->physical_operator = "FixedChunk";
        } else {
            stats->physical_operator = "FixedRareByteWholeFile";
        }
        stats->plan_key_hash = plan_key.hash();
        stats->semantic_mode = semantic_mode_key(plan_key);
        stats->estimated_selectivity = qc.selectivity;
        stats->estimated_cost = qc.cost;
        stats->predicted_candidate_chunks = qc.estimated_candidate_chunks;
        stats->predicted_candidate_blocks = qc.estimated_candidate_blocks;
        stats->predicted_verified_bytes = qc.estimated_verified_bytes;
        const auto scoped_chunks = scoped_chunk_count(I, scope);
        stats->prediction_error_bound_chunks =
            scoped_chunks >= qc.estimated_candidate_chunks
                ? scoped_chunks - qc.estimated_candidate_chunks : 0;
        stats->prediction_error_bound_blocks =
            total_blocks >= qc.estimated_candidate_blocks
                ? total_blocks - qc.estimated_candidate_blocks : 0;
        const auto scoped_bytes = scoped_extended_bytes(I, scope);
        stats->prediction_error_bound_bytes =
            scoped_bytes >= qc.estimated_verified_bytes
                ? scoped_bytes - qc.estimated_verified_bytes : 0;
    }
    bool cancellation_requested = false;
    const auto verifier_start = stats ? std::clock() : std::clock_t(0);
    const bool first_hit_objective = opt.objective == SearchObjective::FirstHit;
    const auto record_first_hit = [&]() {
        if (!stats || stats->first_hit_observed || out.empty()) return;
        stats->first_hit_observed = true;
        stats->time_to_first_hit_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - search_start).count());
    };
    const auto stop_requested = [&]() {
        if (opt.should_cancel && opt.should_cancel()) {
            cancellation_requested = true;
            if (stats) {
                stats->cancellation_requested = true;
                stats->early_stopped = true;
                stats->early_stop_reason = "cancellation";
            }
            return true;
        }
        return false;
    };
    const auto result_bound_reached = [&]() {
        return first_hit_objective || (opt.max_matches != 0 && out.size() >= opt.max_matches);
    };
    if (stop_requested()) goto done;
    if (opt.invert_match) {
        if (stats) stats->qgram_fallback_reason = "invert-match-record-scan";
        if (stats) {
            stats->effective_k = 0;
            stats->selected_qgram_count = 0;
            stats->selected_qgram_rows = 0;
        }
        accounting.note_all_chunks();
        for (uint32_t fid = 0; fid < I.loaded.size(); ++fid) {
            if (stop_requested()) goto done;
            if (!accounting.allows(fid)) continue;
            if (!opt.include_binary && I.infos[fid].binary) continue;
            const auto& data = I.loaded[fid].data;
            accounting.touch(fid, 0, data.size());
            std::size_t b = 0;
            while (b < data.size() || (b == 0 && data.empty())) {
                if (stop_requested()) goto done;
                auto e = data.find(static_cast<char>(opt.record_separator), b);
                bool term = (e != std::string::npos);
                if (!term) e = data.size();
                std::size_t logical_e = e;
                if (p.impl_->opt.crlf && opt.record_separator == '\n' && logical_e > b && data[logical_e - 1] == '\r')
                    --logical_e;
                auto rec = std::string_view(data).substr(b, logical_e - b);
                bool matched = false;
                if (p.is_fixed()) {
                    auto q = std::string_view(p.impl_->expr);
                    bool icase = (p.impl_->opt.case_mode == CaseMode::Insensitive);
                    matched = fixed_match_in_record(rec, q, icase, p.impl_->opt.word, p.impl_->opt.line, p.impl_->opt.unicode, I);
                } else {
                    Match m;
                    detail::VerifierContext context{data, 0, static_cast<std::uint64_t>(data.size()),
                        static_cast<std::uint64_t>(b), static_cast<std::uint64_t>(logical_e),
                        static_cast<std::uint64_t>(b), static_cast<std::uint64_t>(logical_e + 1),
                        false, false, opt.record_separator, p.impl_->opt.crlf};
                    if (detail::regex_search(p.impl_->re, context, p.impl_->opt, &m, fid)) {
                        matched = true;
                    }
                }
                if (!matched) {
                    out.push_back(Match{fid, b, logical_e, {}});
                    record_first_hit();
                    if (result_bound_reached()) {
                        if (stats) {
                            stats->early_stopped = true;
                            stats->early_stop_reason = first_hit_objective ? "first-hit" : "max-matches";
                        }
                        goto done;
                    }
                }
                if (!term) break;
                b = e + 1;
            }
        }
        goto done;
    }

    if (p.is_fixed()) {
        auto q = std::string_view(p.impl_->expr);
        bool icase = (p.impl_->opt.case_mode == CaseMode::Insensitive);

        // Safe cross-chunk fallback: literals longer than chunk_overlap may straddle two chunks.
        // Whole-file rare-byte verification is safe for the guarded contract too;
        // it is selected only when its calibrated estimate beats both indexed paths.
        if (q.size() > I.opt.chunk_overlap ||
            (guarded_fixed_dispatch && qc.fixed_operator == detail::FixedPhysicalOperator::WholeFile)) {
            if (stats) stats->qgram_fallback_reason =
                q.size() > I.opt.chunk_overlap ? "literal-exceeds-chunk-overlap" : "planner-selected-whole-file";
            if (stats && q.size() > I.opt.chunk_overlap) {
                stats->effective_k = 0;
                stats->selected_qgram_count = 0;
                stats->selected_qgram_rows = 0;
            }
            auto cv = chunk_candidates(I, q, &accounting);
            accounting.note_candidates(cv);
            std::vector<uint32_t> files;
            for (auto ci : cv) {
                if (files.empty() || files.back() != I.chunks[ci].file_id) {
                    files.push_back(I.chunks[ci].file_id);
                }
            }
            size_t a = choose_rare_byte(I, q);
            for (auto fid : files) {
                if (stop_requested()) goto done;
                if (!opt.include_binary && I.infos[fid].binary) continue;
                const auto& data = I.loaded[fid].data;
                accounting.touch(fid, 0, data.size());
                size_t pos = 0;
                size_t max_pos = data.size() + (q.empty() ? 1 : 0);
                while (pos < max_pos) {
                    size_t local_end = 0;
                    auto x = anchor_find(data, q, a, pos, max_pos, icase, &local_end);
                    if (x == std::string_view::npos) break;
                    uint64_t abs = x, abs_end = local_end;
                    bool ok = true;
                    if (p.impl_->opt.word) {
                        if (p.impl_->opt.unicode) {
                            auto l = decode_prev(data, abs), r = decode_rune(data, abs_end);
                            if (l.ok && unicode_word_cp(l.cp)) ok = false;
                            if (r.ok && unicode_word_cp(r.cp)) ok = false;
                        } else {
                            auto word = [](unsigned char c) { return std::isalnum(c) || c == '_'; };
                            if (abs > 0 && word((unsigned char)data[abs - 1])) ok = false;
                            if (abs_end < data.size() && word((unsigned char)data[abs_end])) ok = false;
                        }
                    }
                    if (p.impl_->opt.line) {
                        unsigned char sep = opt.record_separator;
                        if (abs > 0 && static_cast<unsigned char>(data[abs - 1]) != sep) ok = false;
                        if (abs_end < data.size() && static_cast<unsigned char>(data[abs_end]) != sep &&
                            !(p.impl_->opt.crlf && sep == '\n' && abs_end + 1 < data.size() && data[abs_end] == '\r' && data[abs_end + 1] == '\n'))
                            ok = false;
                    }
                    if (ok) {
                        out.push_back(Match{fid, abs, abs_end, {}});
                        record_first_hit();
                    if (result_bound_reached()) {
                        if (stats) {
                            stats->early_stopped = true;
                            stats->early_stop_reason = first_hit_objective ? "first-hit" : "max-matches";
                        }
                        goto done;
                    }
                    }
                    pos = x + (opt.overlapping ? 1 : std::max<size_t>(1, local_end - x));
                }
            }
        // Positional blocks are NOT used for word/line/icase queries due to boundary unsafety.
        // - icase: positional Bloom is case-sensitive (hash4 over raw bytes). Case-insensitive
        //   matching requires case folding, which the Bloom does not encode. Using it would miss
        //   matches with different casing (false negatives), so we fall back to chunk-level.
        // - word/line: word boundaries (\\b, \\w) and line anchors (^, $) depend on context outside
        //   the block (adjacent characters, record separators). A block Bloom only knows that the
        //   literal's q-grams appear inside the block, not whether the surrounding characters satisfy
        //   the word/line predicate. Block boundaries do not align with record boundaries, so
        //   pruning at block granularity would be unsafe. Chunk-level pruning is conservative and
        } else if (icase || p.impl_->opt.word || p.impl_->opt.line ||
                   (guarded_fixed_dispatch && qc.fixed_operator == detail::FixedPhysicalOperator::Chunk)) {
            if (stats) {
                if (icase) stats->qgram_fallback_reason = "case-insensitive";
                else if (p.impl_->opt.word) stats->qgram_fallback_reason = "word-boundary";
                else if (p.impl_->opt.line) stats->qgram_fallback_reason = "line-boundary";
                else stats->qgram_fallback_reason = "planner-selected-chunk";
            }
            if (stats && icase) {
                stats->effective_k = 0;
                stats->selected_qgram_count = 0;
                stats->selected_qgram_rows = 0;
            }
            auto cv = chunk_candidates(I, icase ? std::string_view{} : q, &accounting);
            accounting.note_candidates(cv);
            std::unordered_set<uint32_t> done_chunks;
            size_t a = choose_rare_byte(I, q);
            std::unordered_map<uint32_t, uint64_t> next;
            for (auto ci : cv) {
                auto z = I.chunks[ci];
                if (!opt.include_binary && I.infos[z.file_id].binary) continue;
                if (done_chunks.insert(ci).second) {
                    auto v = std::string_view(I.loaded[z.file_id].data).substr(z.core_begin, z.ext_end - z.core_begin);
                    accounting.touch(z.file_id, z.core_begin, z.ext_end);
                    size_t pos = 0;
                    const auto core = z.core_end - z.core_begin;
                    const auto max_start = core + (q.empty() ? 1 : 0);
                    while (pos < max_start) {
                        size_t local_end = 0;
                        auto x = anchor_find(v, q, a, pos, max_start, icase, &local_end);
                        if (x == std::string_view::npos) break;
                        uint64_t abs = z.core_begin + x, abs_end = z.core_begin + local_end;
                        bool ok = true;
                        if (p.impl_->opt.word) {
                            auto& s = I.loaded[z.file_id].data;
                            if (p.impl_->opt.unicode) {
                                auto l = decode_prev(s, abs), r = decode_rune(s, abs_end);
                                if (l.ok && unicode_word_cp(l.cp)) ok = false;
                                if (r.ok && unicode_word_cp(r.cp)) ok = false;
                            } else {
                                auto word = [](unsigned char c) { return std::isalnum(c) || c == '_'; };
                                if (abs > 0 && word((unsigned char)s[abs - 1])) ok = false;
                                if (abs_end < s.size() && word((unsigned char)s[abs_end])) ok = false;
                            }
                        }
                        if (p.impl_->opt.line) {
                            auto& s = I.loaded[z.file_id].data;
                            unsigned char sep = opt.record_separator;
                            if (abs > 0 && static_cast<unsigned char>(s[abs - 1]) != sep) ok = false;
                            if (abs_end < s.size() && static_cast<unsigned char>(s[abs_end]) != sep &&
                                !(p.impl_->opt.crlf && sep == '\n' && abs_end + 1 < s.size() && s[abs_end] == '\r' && s[abs_end + 1] == '\n'))
                                ok = false;
                        }
                        if (ok) {
                            if (!opt.overlapping) {
                                auto& n = next[z.file_id];
                                if (abs < n) {
                                    pos = x + 1;
                                    continue;
                                }
                                n = abs + std::max<size_t>(1, local_end - x);
                            }
                            out.push_back(Match{z.file_id, abs, abs_end, {}});
                            record_first_hit();
                    if (result_bound_reached()) {
                        if (stats) {
                            stats->early_stopped = true;
                            stats->early_stop_reason = first_hit_objective ? "first-hit" : "max-matches";
                        }
                        goto done;
                    }
                        }
                        pos = x + (opt.overlapping ? 1 : std::max<size_t>(1, local_end - x));
                    }
                }
            }
        // Positional block filtering for short case-sensitive fixed literals.
        // For q.size() <= 64 we use the positional Bloom to prune at block granularity:
        // - planned_hashes selects the rarest q-grams under the shared planned_qgrams
        //   budget (0 means auto), minimizing false positives without a hidden cap.
        // - fixed_candidate_blocks intersects the corresponding Bloom rows per chunk, producing a small
        //   set of (chunk, block) candidates. Each candidate is verified with an exact rare-byte scan
        //   limited to the block's core range (+64 lookahead for q-gram overlap). This is conservative
        //   (no false negatives) and typically reduces verified bytes by orders of magnitude for rare literals.
        } else if (q.size() <= 64) {
            if (stats && q.size() >= 4) stats->qgram_fallback_reason = "none";
            size_t a = choose_rare_byte(I, q);
            auto blocks = fixed_candidate_blocks(I, q, &accounting);
            std::unordered_map<uint32_t, uint64_t> next;
            for (auto [ci, bi] : blocks) {
                if (stop_requested()) goto done;
                auto z = I.chunks[ci];
                if (!opt.include_binary && I.infos[z.file_id].binary) continue;
                uint32_t rb = bi * I.pos_block;
                if (rb >= z.core_end - z.core_begin && !(q.empty() && rb == 0 && z.core_end == z.core_begin))
                    continue;
                uint32_t core = std::min<uint32_t>(I.pos_block, (z.core_end - z.core_begin) - rb);
                uint32_t re = std::min<uint32_t>(z.ext_end - z.core_begin, rb + I.pos_block + 64);
                auto v = std::string_view(I.loaded[z.file_id].data).substr(z.core_begin + rb, re - rb);
                size_t pos = 0;
                const auto max_start = static_cast<size_t>(core) + (q.empty() ? 1 : 0);
                accounting.touch(z.file_id, z.core_begin + rb, z.core_begin + re);
                while (pos < max_start) {
                    size_t local_end = 0;
                    auto x = anchor_find(v, q, a, pos, max_start, false, &local_end);
                    if (x == std::string_view::npos) break;
                    uint64_t abs = z.core_begin + rb + x;
                    uint64_t abs_end = z.core_begin + rb + local_end;
                    if (!opt.overlapping) {
                        auto& n = next[z.file_id];
                        if (abs < n) {
                            pos = x + 1;
                            continue;
                        }
                        n = abs + std::max<size_t>(1, local_end - x);
                    }
                    out.push_back(Match{z.file_id, abs, abs_end, {}});
                    record_first_hit();
                    if (result_bound_reached()) {
                        if (stats) {
                            stats->early_stopped = true;
                            stats->early_stop_reason = first_hit_objective ? "first-hit" : "max-matches";
                        }
                        goto done;
                    }
                    pos = x + (opt.overlapping ? 1 : std::max<size_t>(1, local_end - x));
                }
            }
        // Chunk-level fallback for longer literals (64 < q.size() <= chunk_overlap).
        // For longer queries the number of distinct q-grams grows and the positional Bloom's
        // selectivity diminishes (more rows to intersect, higher false-positive cost). The
        // chunk-level q-gram index (Groups) already provides strong pruning for long literals,
        // and verifying at chunk granularity avoids the per-block Bloom overhead. This path
        // remains conservative (chunk candidate set is a superset of true matches).
        } else {
            if (stats) stats->qgram_fallback_reason = "literal-longer-than-positional-range";
            auto cv = chunk_candidates(I, q, &accounting);
            accounting.note_candidates(cv);
            size_t a = choose_rare_byte(I, q);
            std::unordered_map<uint32_t, uint64_t> next;
            for (auto ci : cv) {
                auto z = I.chunks[ci];
                if (!opt.include_binary && I.infos[z.file_id].binary) continue;
                auto v = std::string_view(I.loaded[z.file_id].data).substr(z.core_begin, z.ext_end - z.core_begin);
                accounting.touch(z.file_id, z.core_begin, z.ext_end);
                size_t pos = 0;
                while (pos < z.core_end - z.core_begin) {
                    if (stop_requested()) goto done;
                    size_t local_end = 0;
                    auto x = anchor_find(v, q, a, pos, z.core_end - z.core_begin, false, &local_end);
                    if (x == std::string_view::npos) break;
                    uint64_t abs = z.core_begin + x;
                    uint64_t abs_end = z.core_begin + local_end;
                    if (!opt.overlapping) {
                        auto& n = next[z.file_id];
                        if (abs < n) {
                            pos = x + 1;
                            continue;
                        }
                        n = abs + std::max<size_t>(1, local_end - x);
                    }
                    out.push_back(Match{z.file_id, abs, abs_end, {}});
                    record_first_hit();
                    if (result_bound_reached()) {
                        if (stats) {
                            stats->early_stopped = true;
                            stats->early_stop_reason = first_hit_objective ? "first-hit" : "max-matches";
                        }
                        goto done;
                    }
                    pos = x + (opt.overlapping ? 1 : std::max<size_t>(1, local_end - x));
                }
            }
        }
    } else {
        if (p.impl_->re.query_ir.is_pure_literal && p.impl_->re.groups == 0 && p.impl_->opt.case_mode != CaseMode::Insensitive && !p.impl_->re.extended &&
            (p.impl_->opt.multiline || p.impl_->re.query_ir.exact_literal.find(static_cast<char>(opt.record_separator)) == std::string::npos)) {
            PatternOptions fopt = p.impl_->opt;
            fopt.kind = PatternKind::Fixed;
            auto fixed_pat = Pattern::compile(p.impl_->re.query_ir.exact_literal, fopt);
            FixedDispatchSuppression suppress;
            return find(fixed_pat, opt, stats);
        }
        std::string lit;
        std::vector<uint32_t> cv;
        if (p.impl_->opt.case_mode != CaseMode::Insensitive && !p.impl_->re.query_ir.branch_mandatory.empty()) {
            for (const auto& branch : p.impl_->re.query_ir.branch_mandatory) {
                std::string best_lit;
                for (const auto& m : branch) {
                    if (m.size() > best_lit.size()) best_lit = m;
                }
                if (!best_lit.empty()) {
                    auto bcv = chunk_candidates(I, best_lit, &accounting);
                    cv.insert(cv.end(), bcv.begin(), bcv.end());
                }
            }
            std::sort(cv.begin(), cv.end());
            cv.erase(std::unique(cv.begin(), cv.end()), cv.end());
        } else {
            if (p.impl_->opt.case_mode != CaseMode::Insensitive) {
                for (auto& m : p.impl_->re.query_ir.mandatory) {
                    if (m.size() > lit.size()) lit = m;
                }
            }
            cv = chunk_candidates(I, lit, &accounting);
        }
        struct BoundedBranch {
            std::string anchor;
            std::vector<LiteralOffsetConstraint> constraints;
            bool joined = false;
        };
        std::vector<BoundedBranch> bounded_branches;
        const auto branches = top_level_branches(p.impl_->re.ast);
        const auto& configured_branch_lists = p.impl_->re.query_ir.branch_mandatory;
        std::vector<std::vector<std::string>> derived_branch_lists;
        if (configured_branch_lists.empty() && branches.size() > 1) {
            derived_branch_lists.reserve(branches.size());
            for (const auto& branch : branches) derived_branch_lists.push_back(detail::query_mandatory(branch));
        }
        const auto& branch_lists = configured_branch_lists.empty() ? derived_branch_lists : configured_branch_lists;
        const bool split_branches = !branch_lists.empty() && branch_lists.size() == branches.size();
        const std::size_t branch_count = split_branches ? branches.size() : std::size_t{1};
        for (std::size_t branch_index = 0; branch_index < branch_count; ++branch_index) {
            const auto& required = split_branches ? branch_lists[branch_index] : p.impl_->re.query_ir.mandatory;
            BoundedBranch candidate;
            for (const auto& literal : required) {
                if (literal.size() > candidate.anchor.size()) candidate.anchor = literal;
            }
            if (candidate.anchor.empty()) continue;
            const auto summary = literal_offsets(branches.empty() ? p.impl_->re.ast : branches[split_branches ? branch_index : 0]);
            if (summary.finite && required.size() >= 2) {
                std::vector<bool> used(summary.literals.size(), false);
                bool complete = true;
                for (const auto& literal : required) {
                    bool found = false;
                    for (std::size_t i = 0; i < summary.literals.size(); ++i) {
                        if (!used[i] && summary.literals[i].literal == literal) {
                            used[i] = true;
                            candidate.constraints.push_back(summary.literals[i]);
                            found = true;
                            break;
                        }
                    }
                    if (!found) { complete = false; break; }
                }
                candidate.joined = complete && candidate.constraints.size() == required.size();
                if (!candidate.joined) candidate.constraints.clear();
            }
            bounded_branches.push_back(std::move(candidate));
        }
        detail::RegexAnalysis bounded_analysis;
        if (bounded_regex_eligible(p.impl_->re, p.impl_->opt, opt.record_separator, &bounded_analysis) &&
            !bounded_branches.empty()) {
            if (stats) stats->physical_operator = "RegexBoundedRegion";
            const auto& bounded_cv = cv;
            accounting.note_candidates(bounded_cv);
            std::vector<uint32_t> bounded_files;
            for (auto ci : bounded_cv) {
                if (bounded_files.empty() || bounded_files.back() != I.chunks[ci].file_id)
                    bounded_files.push_back(I.chunks[ci].file_id);
            }
            for (auto fid : bounded_files) {
                if (stop_requested()) goto done;
                if (!opt.include_binary && I.infos[fid].binary) continue;
                const auto& data = I.loaded[fid].data;
                const auto remain = [&]() {
                    return first_hit_objective ? std::size_t{1} :
                        (opt.max_matches ? opt.max_matches - out.size() : 0);
                };
                const auto verify_bounded_record = [&](std::uint64_t record_begin, std::uint64_t record_end) {
                    const auto record = std::string_view(data).substr(
                        static_cast<std::size_t>(record_begin),
                        static_cast<std::size_t>(record_end - record_begin));
                    std::vector<BoundedRegexRegion> regions;
                    for (const auto& branch : bounded_branches) {
                        std::vector<BoundedRegexRegion> branch_regions;
                        if (branch.joined) {
                            branch_regions = bounded_regex_regions_joined(record, record_begin, record_end,
                                                                          branch.constraints, bounded_analysis);
                        } else {
                            branch_regions = bounded_regex_regions(record, record_begin, record_end,
                                                                   branch.anchor, bounded_analysis);
                        }
                        regions.insert(regions.end(), branch_regions.begin(), branch_regions.end());
                    }
                    std::sort(regions.begin(), regions.end(), [](const auto& a, const auto& b) {
                        if (a.candidate_begin != b.candidate_begin) return a.candidate_begin < b.candidate_begin;
                        return a.candidate_end < b.candidate_end;
                    });
                    std::vector<BoundedRegexRegion> merged_regions;
                    for (const auto& next : regions) {
                        if (!merged_regions.empty() && next.candidate_begin <= merged_regions.back().candidate_end) {
                            auto& current = merged_regions.back();
                            current.candidate_end = std::max(current.candidate_end, next.candidate_end);
                            current.region_begin = std::min(current.region_begin, next.region_begin);
                            current.region_end = std::max(current.region_end, next.region_end);
                        } else merged_regions.push_back(next);
                    }
                    regions.swap(merged_regions);
                    for (const auto& bounded : regions) {
                        if (stop_requested()) return false;
                        detail::VerifierContext context{data, 0, static_cast<std::uint64_t>(data.size()),
                            record_begin, record_end, bounded.candidate_begin, bounded.candidate_end,
                            false, false, opt.record_separator, p.impl_->opt.crlf};
                        context.region_begin = bounded.region_begin;
                        context.region_end = bounded.region_end;
                        context.bounded_region = true;
                        accounting.touch(fid, bounded.region_begin, bounded.region_end);
                        auto ms = detail::regex_find_all(p.impl_->re, context, p.impl_->opt,
                                                          opt.overlapping, fid, remain());
                        out.insert(out.end(), ms.begin(), ms.end());
                        record_first_hit();
                        if (result_bound_reached()) {
                            if (stats) {
                                stats->early_stopped = true;
                                stats->early_stop_reason = first_hit_objective ? "first-hit" : "max-matches";
                            }
                            return false;
                        }
                    }
                    return true;
                };
                if (p.impl_->opt.multiline) {
                    if (!verify_bounded_record(0, static_cast<std::uint64_t>(data.size()))) goto done;
                } else {
                    std::size_t b = 0;
                    while (b < data.size() || (b == 0 && data.empty())) {
                        if (stop_requested()) goto done;
                        auto e = data.find(static_cast<char>(opt.record_separator), b);
                        bool term = (e != std::string::npos);
                        if (!term) e = data.size();
                        std::size_t logical_e = e;
                        if (p.impl_->opt.crlf && opt.record_separator == '\n' && logical_e > b && data[logical_e - 1] == '\r')
                            logical_e -= 1;
                        if (!verify_bounded_record(static_cast<std::uint64_t>(b),
                                                   static_cast<std::uint64_t>(logical_e))) goto done;
                        if (!term) break;
                        b = e + 1;
                    }
                }
            }
            goto done;
        }
        accounting.note_candidates(cv);
        std::vector<uint32_t> files;
        for (auto ci : cv) {
            if (files.empty() || files.back() != I.chunks[ci].file_id) {
                files.push_back(I.chunks[ci].file_id);
            }
        }
        for (auto fid : files) {
            if (stop_requested()) goto done;
            if (!opt.include_binary && I.infos[fid].binary) continue;
            auto const& data = I.loaded[fid].data;
            if (!lit.empty() && lit.size() < 4 && p.impl_->opt.case_mode == CaseMode::Sensitive &&
                data.find(lit) == std::string::npos) continue;
            accounting.touch(fid, 0, data.size());
            const auto remain = [&]() { return first_hit_objective ? std::size_t{1} : (opt.max_matches ? opt.max_matches - out.size() : 0); };
            if (p.impl_->opt.multiline) {
                detail::VerifierContext context{data, 0, static_cast<std::uint64_t>(data.size()),
                    0, static_cast<std::uint64_t>(data.size()), 0, static_cast<std::uint64_t>(data.size() + 1),
                    false, false, opt.record_separator, p.impl_->opt.crlf};
                auto ms = detail::regex_find_all(p.impl_->re, context, p.impl_->opt, opt.overlapping, fid, remain());
                out.insert(out.end(), ms.begin(), ms.end());
                record_first_hit();
                if (result_bound_reached()) {
                    if (stats) {
                        stats->early_stopped = true;
                        stats->early_stop_reason = first_hit_objective ? "first-hit" : "max-matches";
                    }
                    goto done;
                }
            } else {
                std::size_t b = 0;
                while (b < data.size() || (b == 0 && data.empty())) {
                    if (stop_requested()) goto done;
                    auto e = data.find(static_cast<char>(opt.record_separator), b);
                    bool term = (e != std::string::npos);
                    if (!term) e = data.size();
                    std::size_t logical_e = e;
                    if (p.impl_->opt.crlf && opt.record_separator == '\n' && logical_e > b && data[logical_e - 1] == '\r')
                        --logical_e;
                    detail::VerifierContext context{data, 0, static_cast<std::uint64_t>(data.size()),
                        static_cast<std::uint64_t>(b), static_cast<std::uint64_t>(logical_e),
                        static_cast<std::uint64_t>(b), static_cast<std::uint64_t>(logical_e + 1),
                        false, false, opt.record_separator, p.impl_->opt.crlf};
                    auto ms = detail::regex_find_all(p.impl_->re, context, p.impl_->opt, opt.overlapping, fid, remain());
                    out.insert(out.end(), ms.begin(), ms.end());
                record_first_hit();
                if (result_bound_reached()) {
                    if (stats) {
                        stats->early_stopped = true;
                        stats->early_stop_reason = first_hit_objective ? "first-hit" : "max-matches";
                    }
                    goto done;
                }
                    if (opt.max_matches && out.size() >= opt.max_matches) break;
                    if (!term) break;
                    b = e + 1;
                }
            }
            if (opt.max_matches && out.size() >= opt.max_matches) break;
        }
    }

done:
    if (cancellation_requested) out.clear();
    if (opt.max_matches && out.size() > opt.max_matches) {
        out.resize(opt.max_matches);
    }
    if (stats) {
        accounting.finish();
        stats->matches = out.size();
        const auto elapsed = std::clock() - verifier_start;
        if (elapsed > 0) {
            stats->verifier_cpu_ns =
                static_cast<std::uint64_t>((static_cast<long double>(elapsed) * 1000000000.0L) /
                                            static_cast<long double>(CLOCKS_PER_SEC));
        }
        const bool is_fallback_path = opt.invert_match || (qc.verifier == detail::VerifierKind::RegexBruteForce) ||
                                      (p.is_fixed() && !guarded_fixed_dispatch);
        stats->verifier_fallback = is_fallback_path;
        auto candidates = detail::estimate_all_candidate_plans(plan_key, I);
        PlanCandidateMetrics chosen;
        chosen.name = stats->verifier;
        chosen.verifier = static_cast<VerifierKind>(qc.verifier);
        chosen.predicted_cost = qc.cost;
        chosen.predicted_selectivity = qc.selectivity;
        chosen.actual_cost = double(stats->physically_touched_bytes) + 100.0 * double(stats->candidate_chunks);
        stats->measured_cost = chosen.actual_cost;
        chosen.actual_time_ms = double(stats->verifier_cpu_ns) / 1000000.0;
        chosen.actual_verified_bytes = stats->physically_touched_bytes;
        chosen.actual_candidate_chunks = stats->candidate_chunks;
        chosen.actual_candidate_blocks = stats->candidate_blocks;
        chosen.is_fallback = is_fallback_path;
        chosen.chosen = true;
        chosen.actual_observed = true;
        chosen.observation = PlanCandidateMetrics::ObservationStatus::Observed;
        chosen.actual_index_probe_bytes = stats->index_probe_bytes;
        chosen.actual_index_probe_operations = stats->index_probe_operations;
        chosen.actual_verification_bytes = stats->physically_touched_bytes;
        chosen.actual_verifier_cpu_ns = stats->verifier_cpu_ns;
        chosen.actual_allocation_count = stats->allocation_count;
        chosen.actual_allocation_bytes = stats->allocation_bytes;
        chosen.actual_page_faults = stats->page_faults;
        chosen.allocation_metrics_available = stats->allocation_metrics_available;
        chosen.page_fault_metrics_available = stats->page_fault_metrics_available;

        for (auto& cand : candidates) {
            if (cand.verifier == chosen.verifier) {
                cand.actual_cost = chosen.actual_cost;
                cand.actual_time_ms = chosen.actual_time_ms;
                cand.actual_verified_bytes = chosen.actual_verified_bytes;
                cand.actual_candidate_chunks = chosen.actual_candidate_chunks;
                cand.actual_candidate_blocks = chosen.actual_candidate_blocks;
                cand.chosen = true;
                cand.actual_observed = true;
                cand.observation = PlanCandidateMetrics::ObservationStatus::Observed;
                cand.actual_index_probe_bytes = chosen.actual_index_probe_bytes;
                cand.actual_index_probe_operations = chosen.actual_index_probe_operations;
                cand.actual_verification_bytes = chosen.actual_verification_bytes;
                cand.actual_verifier_cpu_ns = chosen.actual_verifier_cpu_ns;
                cand.actual_allocation_count = chosen.actual_allocation_count;
                cand.actual_allocation_bytes = chosen.actual_allocation_bytes;
                cand.actual_page_faults = chosen.actual_page_faults;
                cand.allocation_metrics_available = chosen.allocation_metrics_available;
                cand.page_fault_metrics_available = chosen.page_fault_metrics_available;
            }
        }
        auto reg = compute_plan_regret(chosen, candidates, p.expression());
        reg.plan_key_hash = plan_key.hash();
        reg.semantic_mode = semantic_mode_key(plan_key);
        stats->plan_regret = reg.relative_regret;
    }
    return out;
}

std::vector<uint32_t> Searcher::files(const Pattern& p, SearchOptions opt, SearchStats* stats) const {
    if (!index_ || !index_->impl_) throw std::runtime_error("pergrep: empty index");
    auto& I = *index_->impl_;
    std::vector<std::uint32_t> normalized_file_scope;
    if (!opt.eligible_file_ids.empty()) {
        normalized_file_scope.assign(opt.eligible_file_ids.begin(), opt.eligible_file_ids.end());
        std::sort(normalized_file_scope.begin(), normalized_file_scope.end());
        normalized_file_scope.erase(std::unique(normalized_file_scope.begin(), normalized_file_scope.end()), normalized_file_scope.end());
        opt.eligible_file_ids = normalized_file_scope;
    }
    if (stats) {
        *stats = {};
        stats->objective = opt.objective == SearchObjective::FirstHit ? "first-hit" :
                           opt.objective == SearchObjective::OrderedPrefix ? "ordered-prefix" : "exhaustive";
        stats->candidate_order = "file-id";
        stats->candidate_order_preserved = true;
        stats->configured_planned_qgrams = static_cast<std::uint64_t>(I.opt.planned_qgrams);
        if (p.is_fixed()) {
            const auto q = std::string_view(p.impl_->expr);
            const auto selected = planned_hashes(I, q);
            stats->effective_k = static_cast<std::uint64_t>(adaptive_k(I, q));
            stats->selected_qgram_count = static_cast<std::uint64_t>(selected.size());
            stats->selected_qgram_rows = static_cast<std::uint64_t>(selected.size());
            if (q.size() < 4) stats->qgram_fallback_reason = "query-fewer-than-4-bytes";
        } else {
            stats->qgram_fallback_reason = "not-fixed-literal";
        }
    }
    // A first-hit file query must retain file order. Probe each eligible file
    // with a one-file scope; this is intentionally conservative and never
    // reorders candidates based on hit probability.
    if (opt.objective == SearchObjective::FirstHit) {
        for (uint32_t fid = 0; fid < I.infos.size(); ++fid) {
            if (opt.should_cancel && opt.should_cancel()) {
                if (stats) { stats->cancellation_requested = true; stats->early_stopped = true; stats->early_stop_reason = "cancellation"; }
                return {};
            }
            if (!opt.eligible_file_ids.empty() && !scope_contains(opt.eligible_file_ids, fid)) continue;
            if (!opt.include_binary && I.infos[fid].binary) continue;
            const std::uint32_t only_id = fid;
            auto one = opt;
            one.eligible_file_ids = std::span<const std::uint32_t>(&only_id, 1);
            one.files_with_matches = false;
            one.files_without_match = false;
            one.max_matches = 0;
            SearchStats one_stats;
            auto ms = find(p, one, &one_stats);
            if (one_stats.cancellation_requested) {
                if (stats) *stats = one_stats;
                return {};
            }
            const bool selected = opt.files_without_match ? ms.empty() : !ms.empty();
            if (selected) {
                if (stats) {
                    *stats = one_stats;
                    stats->objective = "first-hit";
                    stats->candidate_order = "file-id";
                    stats->candidate_order_preserved = true;
                    stats->early_stopped = true;
                    stats->early_stop_reason = "first-file-result";
                }
                return {fid};
            }
        }
        return {};
    }
    auto find_opt = opt;
    // File selection needs the complete per-file hit map; max_matches bounds
    // the returned file list below, not the discovery scan.
    find_opt.objective = SearchObjective::Exhaustive;
    find_opt.max_matches = 0;
    SearchStats discovery_stats;
    SearchStats* discovery_stats_out = stats ? stats : &discovery_stats;
    auto ms = find(p, find_opt, discovery_stats_out);
    if (discovery_stats_out->cancellation_requested) {
        if (stats && discovery_stats_out != stats) *stats = *discovery_stats_out;
        return {};
    }
    if (stats) {
        stats->objective = to_string(opt.objective);
        stats->candidate_order = "file-id";
        stats->candidate_order_preserved = true;
    }
    std::vector<uint8_t> eligible(I.infos.size(), 1);
    if (!opt.eligible_file_ids.empty()) {
        std::fill(eligible.begin(), eligible.end(), 0);
        for (auto fid : opt.eligible_file_ids) {
            if (fid < eligible.size()) eligible[fid] = 1;
        }
    }
    std::vector<uint8_t> hit(I.infos.size(), 0);
    for (const auto& m : ms) {
        if (m.file_id < hit.size()) hit[m.file_id] = 1;
    }
    std::vector<uint32_t> out;
    if (opt.files_without_match) {
        for (uint32_t i = 0; i < hit.size(); ++i) {
            if (!eligible[i]) continue;
            if (!opt.include_binary && I.infos[i].binary) continue;
            if (!hit[i]) {
                out.push_back(i);
                if (opt.max_matches && out.size() >= opt.max_matches) break;
            }
        }
    } else {
        for (uint32_t i = 0; i < hit.size(); ++i) {
            if (!eligible[i]) continue;
            if (!opt.include_binary && I.infos[i].binary) continue;
            if (hit[i]) {
                out.push_back(i);
                if (opt.max_matches && out.size() >= opt.max_matches) break;
            }
        }
    }
    return out;
}

} // namespace pergrep
