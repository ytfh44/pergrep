#include "internal.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
bool unicode_word_cp(UChar32 cp) {
    return u_isalnum(cp) ||
           u_charType(cp) == U_CONNECTOR_PUNCTUATION ||
           u_hasBinaryProperty(cp, UCHAR_JOIN_CONTROL) ||
           u_charType(cp) == U_NON_SPACING_MARK ||
           u_charType(cp) == U_COMBINING_SPACING_MARK ||
           u_charType(cp) == U_ENCLOSING_MARK;
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

void group_candidates(const detail::IndexData::Group& g, const QueryDesc& q, std::vector<uint32_t>& out) {
    if (g.gids.empty()) return;
    auto const& qc = q.classes[g.lg - 9];
    if (qc.empty()) {
        out.insert(out.end(), g.gids.begin(), g.gids.end());
        return;
    }
    std::vector<uint64_t> c(g.words, ~0ull);
    if (g.gids.size() & 63) c.back() = (1ull << (g.gids.size() & 63)) - 1;
    for (auto [ww, mask64] : qc) {
        while (mask64) {
            unsigned bit = std::countr_zero(mask64);
            uint32_t row = uint32_t(ww) * 64 + bit;
            auto p = g.bits.data() + (size_t)row * g.words;
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
            if (li < g.gids.size()) out.push_back(g.gids[li]);
            z &= z - 1;
        }
    }
}

std::vector<uint32_t> chunk_candidates(const detail::IndexData& I, std::string_view lit) {
    std::vector<uint32_t> out;
    if (lit.size() < 4 || lit.size() > I.opt.chunk_overlap) {
        out.resize(I.chunks.size());
        std::iota(out.begin(), out.end(), 0);
        return out;
    }
    auto q = detail::compile_qgram_query(lit);
    for (auto const& g : I.groups) {
        group_candidates(g, q, out);
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

// Adaptive k helper: chooses how many q-grams to use for positional filtering.
// - Short query (<=4 bytes, i.e. 0-1 distinct q-grams): k=1 (or 0 if <4, meaning no q-gram filter).
// - Otherwise k = min(planned_qgrams, num_unique_qgrams, budget_based_k) where
//   budget_based_k is derived from positional_budget_ratio and chunk_bytes.
// - For long literals (many distinct q-grams) budget is relaxed up to 8 for
//   better pruning; this still respects the 8-cap and never increases false
//   negatives because using more q-grams can only tighten the intersection,
//   while capping at 8 bounds positional row lookups per chunk.
// Conservative fallback: k==0 means no positional q-gram filter -> all chunks
// pass (zero false negatives).
size_t adaptive_k(const detail::IndexData& I, std::string_view q) {
    if (q.size() < 4) return 0;
    if (q.size() <= 4) return 1;
    // Count distinct 4-byte q-gram hashes in the query (per-query distinctness).
    std::unordered_set<uint32_t> uniq;
    uniq.reserve(q.size());
    for (size_t i = 0; i + 4 <= q.size(); ++i) {
        uniq.insert(detail::hash4((const unsigned char*)q.data() + i));
    }
    size_t num_unique = uniq.size();
    if (num_unique == 0) return 0;
    size_t planned = I.opt.planned_qgrams;
    // Cost model: each positional q-gram probe costs roughly one row (m * mask_bytes).
    // Derive a budget-based cap from index memory budget:
    //   budget_bytes ~= chunk_bytes * positional_budget_ratio
    //   budget_based_k ~= budget_bytes / 4096, clamped to [1,8].
    // This ties k to both knobs the user controls (chunk size and budget ratio)
    // and keeps the per-chunk row intersection cheap.
    double budget_bytes = double(I.opt.chunk_bytes) * I.opt.positional_budget_ratio;
    size_t budget_based_k = 1;
    if (budget_bytes > 0) {
        budget_based_k = static_cast<size_t>(budget_bytes / 4096.0);
        if (budget_based_k < 1) budget_based_k = 1;
        if (budget_based_k > 8) budget_based_k = 8;
    }
    // Long literals carry many distinct q-grams; relax budget slightly for
    // better pruning (still capped at 8). This is the "adaptive" part:
    // short queries stay at 1, long queries can use up to 8 rarest q-grams.
    if (num_unique >= 6 && q.size() >= 12) {
        budget_based_k = std::min<size_t>(8, budget_based_k + 2);
    }
    if (q.size() >= 20) {
        budget_based_k = std::min<size_t>(8, budget_based_k + 1);
    }
    size_t k = std::min({planned, num_unique, budget_based_k});
    // Ensure at least 1 for any query that actually has a q-gram.
    if (k == 0) k = 1;
    return k;
}

// Rarity-aware q-gram planner.
// - Collects all 4-byte q-gram hashes of the query.
// - Sorts by corpus frequency (I.qgram_freq on low 16 bits of hash) ascending:
//   rarest first, because rare q-grams prune more candidates per probe.
// - Deduplicates hashes to respect per-query distinctness (repeated q-grams
//   do not add filtering power).
// - Drops extremely common q-grams whose corpus frequency exceeds 10% of
//   corpus_bytes. This threshold identifies q-grams that appear in a large
//   fraction of chunks and would pollute the candidate set with low-selectivity
//   rows. Dropping is conservative: using fewer q-grams relaxes the positional
//   intersection (more blocks pass), so zero false negatives is preserved;
//   we only keep a subset of the rarest q-grams.
// - Selects the first k hashes after filtering, where k is from adaptive_k().
//   If filtering empties the set, returns empty -> caller falls back to no
//   q-gram filter (all chunks).
std::vector<uint32_t> planned_hashes(const detail::IndexData& I, std::string_view q) {
    std::vector<uint32_t> h;
    if (q.size() < 4) return h;
    for (size_t i = 0; i + 4 <= q.size(); ++i)
        h.push_back(detail::hash4((const unsigned char*)q.data() + i));
    // Rarity ordering: rarest first using corpus qgram frequency.
    std::sort(h.begin(), h.end(), [&](uint32_t a, uint32_t b) {
        auto fa = I.qgram_freq[a & 65535u], fb = I.qgram_freq[b & 65535u];
        if (fa != fb) return fa < fb;
        return a < b;
    });
    h.erase(std::unique(h.begin(), h.end()), h.end());
    // Adaptive k (budget + distinctness aware).
    size_t k = adaptive_k(I, q);
    if (k == 0) return {};
    // Drop extremely common q-grams: freq > 10% of corpus bytes.
    // For tiny corpora corpus_bytes may be 0-100; threshold then tiny, so
    // guard against over-dropping by only applying when corpus_bytes is large
    // enough for the ratio to be meaningful (>40). Otherwise keep all.
    if (I.corp_bytes > 40) {
        uint64_t common_thresh = I.corp_bytes / 10;
        std::vector<uint32_t> filtered;
        filtered.reserve(h.size());
        for (auto hh : h) {
            uint32_t f = I.qgram_freq[hh & 65535u];
            if (static_cast<uint64_t>(f) <= common_thresh) filtered.push_back(hh);
        }
        // Only use filtered set if it still contains at least one q-gram;
        // otherwise fall back to the rarest (even if common) to retain some
        // pruning rather than disabling the filter entirely. But spec says
        // dropping common even within budget is intended; if all are common,
        // returning empty (no filter) is the conservative fallback.
        // Here we prefer the conservative empty-return when everything is
        // common, to avoid a useless highly-common filter.
        if (!filtered.empty()) {
            h.swap(filtered);
        } else {
            // All q-grams are extremely common -> disable positional filter
            // rather than polluting with a low-selectivity gram.
            return {};
        }
    }
    if (h.size() > k) h.resize(k);
    return h;
}

// Positional filter compiler — query-aware block Bloom.
// Compiles a fixed literal query into positional constraints: chunk -> block Bloom row selection.
// Safety invariants (zero false negatives):
// - Positional blocks are ONLY used for case-sensitive fixed literals without word/line flags.
//   Word/line boundaries are record-based (e.g., \\b, ^, $) and block boundaries do not align
//   with record boundaries. Using block Bloom for word/line would be unsafe because a literal
//   could be present in a block but fail the word/line check due to context outside the block,
//   or vice versa. Therefore word/line/icase queries fall back to chunk-level candidate pruning
//   (see `else if (icase||word||line)` branch in Searcher::find) which is conservative.
// - Safe cross-chunk fallback: when literal length exceeds chunk_overlap, a match may straddle
//   two chunks and would be missed by chunk-level pruning. The code detects `q.size() > chunk_overlap`
//   and falls back to whole-file rare-byte scan over the union of candidate files (conservative
//   files union). This guarantees no false negatives for long literals crossing 32 KiB boundaries.
// - Block Bloom is conservative: each block's Bloom may have false positives but never false
//   negatives; intersection of q-gram rows can only over-approximate candidate blocks.
std::vector<std::pair<uint32_t, uint32_t>> fixed_candidate_blocks(const detail::IndexData& I, std::string_view q, SearchStats* st) {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    auto cv = chunk_candidates(I, q);
    if (st) st->candidate_chunks += cv.size();
    auto hs = planned_hashes(I, q);
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
    if (st) st->candidate_blocks += out.size();
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

} // namespace

Searcher::Searcher(std::shared_ptr<const Index> i) : owned_(std::move(i)), index_(owned_.get()) {}
Searcher::Searcher(const Index& i) : index_(&i) {}

std::vector<Match> Searcher::find(const Pattern& p, SearchOptions opt, SearchStats* stats) const {
    if (!index_ || !index_->impl_) throw std::runtime_error("pergrep: empty index");
    if (stats) *stats = {};
    auto& I = *index_->impl_;
    std::vector<Match> out;

    if (opt.invert_match) {
        if (stats) stats->candidate_chunks += I.chunks.size();
        for (uint32_t fid = 0; fid < I.loaded.size(); ++fid) {
            if (!opt.include_binary && I.infos[fid].binary) continue;
            const auto& data = I.loaded[fid].data;
            if (stats) stats->verified_bytes += data.size();
            std::size_t b = 0;
            while (b < data.size() || (b == 0 && data.empty())) {
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
                    if (detail::regex_search(p.impl_->re, rec, p.impl_->opt, 0, &m, fid, opt.record_separator)) {
                        matched = true;
                    }
                }
                if (!matched) {
                    out.push_back(Match{fid, b, logical_e, {}});
                    if (opt.max_matches && out.size() >= opt.max_matches) goto done;
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
        // Chunk-level pruning would miss such matches (no single chunk contains the full literal).
        // We therefore fall back to whole-file scanning over the union of candidate files
        // (conservative files union from chunk_candidates). The rare-byte anchor scan over the
        // entire file guarantees no false negatives, even when the literal crosses the 32 KiB boundary.
        if (q.size() > I.opt.chunk_overlap) {
            auto cv = chunk_candidates(I, q);
            if (stats) stats->candidate_chunks += cv.size();
            std::vector<uint32_t> files;
            for (auto ci : cv) {
                if (files.empty() || files.back() != I.chunks[ci].file_id) {
                    files.push_back(I.chunks[ci].file_id);
                }
            }
            size_t a = choose_rare_byte(I, q);
            for (auto fid : files) {
                if (!opt.include_binary && I.infos[fid].binary) continue;
                const auto& data = I.loaded[fid].data;
                if (stats) stats->verified_bytes += data.size();
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
                        if (opt.max_matches && out.size() >= opt.max_matches) goto done;
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
        //   defers word/line checks to exact verification.
        } else if (icase || p.impl_->opt.word || p.impl_->opt.line) {
            auto cv = chunk_candidates(I, icase ? std::string_view{} : q);
            if (stats) stats->candidate_chunks += cv.size();
            std::unordered_set<uint32_t> done_chunks;
            size_t a = choose_rare_byte(I, q);
            std::unordered_map<uint32_t, uint64_t> next;
            for (auto ci : cv) {
                auto z = I.chunks[ci];
                if (!opt.include_binary && I.infos[z.file_id].binary) continue;
                if (done_chunks.insert(ci).second) {
                    auto v = std::string_view(I.loaded[z.file_id].data).substr(z.core_begin, z.ext_end - z.core_begin);
                    if (stats) stats->verified_bytes += v.size();
                    size_t pos = 0;
                    while (pos < z.core_end - z.core_begin) {
                        size_t local_end = 0;
                        auto x = anchor_find(v, q, a, pos, z.core_end - z.core_begin, icase, &local_end);
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
                            if (opt.max_matches && out.size() >= opt.max_matches) goto done;
                        }
                        pos = x + (opt.overlapping ? 1 : std::max<size_t>(1, local_end - x));
                    }
                }
            }
        // Positional block filtering for short case-sensitive fixed literals.
        // For q.size() <= 64 we use the positional Bloom to prune at block granularity:
        // - planned_hashes selects the rarest q-grams of the query (adaptive k) to minimize false positives.
        // - fixed_candidate_blocks intersects the corresponding Bloom rows per chunk, producing a small
        //   set of (chunk, block) candidates. Each candidate is verified with an exact rare-byte scan
        //   limited to the block's core range (+64 lookahead for q-gram overlap). This is conservative
        //   (no false negatives) and typically reduces verified bytes by orders of magnitude for rare literals.
        } else if (q.size() <= 64) {
            auto blocks = fixed_candidate_blocks(I, q, stats);
            size_t a = choose_rare_byte(I, q);
            std::unordered_map<uint32_t, uint64_t> next;
            for (auto [ci, bi] : blocks) {
                auto z = I.chunks[ci];
                if (!opt.include_binary && I.infos[z.file_id].binary) continue;
                uint32_t rb = bi * I.pos_block;
                if (rb >= z.core_end - z.core_begin) continue;
                uint32_t core = std::min<uint32_t>(I.pos_block, (z.core_end - z.core_begin) - rb);
                uint32_t re = std::min<uint32_t>(z.ext_end - z.core_begin, rb + I.pos_block + 64);
                auto v = std::string_view(I.loaded[z.file_id].data).substr(z.core_begin + rb, re - rb);
                if (stats) stats->verified_bytes += v.size();
                size_t pos = 0;
                while (pos < core) {
                    size_t local_end = 0;
                    auto x = anchor_find(v, q, a, pos, core, false, &local_end);
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
                    if (opt.max_matches && out.size() >= opt.max_matches) goto done;
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
            auto cv = chunk_candidates(I, q);
            if (stats) stats->candidate_chunks += cv.size();
            size_t a = choose_rare_byte(I, q);
            std::unordered_map<uint32_t, uint64_t> next;
            for (auto ci : cv) {
                auto z = I.chunks[ci];
                if (!opt.include_binary && I.infos[z.file_id].binary) continue;
                auto v = std::string_view(I.loaded[z.file_id].data).substr(z.core_begin, z.ext_end - z.core_begin);
                if (stats) stats->verified_bytes += v.size();
                size_t pos = 0;
                while (pos < z.core_end - z.core_begin) {
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
                    if (opt.max_matches && out.size() >= opt.max_matches) goto done;
                    pos = x + (opt.overlapping ? 1 : std::max<size_t>(1, local_end - x));
                }
            }
        }
    } else {
        if (p.impl_->re.is_pure_literal && p.impl_->opt.case_mode != CaseMode::Insensitive && !p.impl_->re.extended &&
            (p.impl_->opt.multiline || p.impl_->re.exact_literal.find(static_cast<char>(opt.record_separator)) == std::string::npos)) {
            PatternOptions fopt = p.impl_->opt;
            fopt.kind = PatternKind::Fixed;
            auto fixed_pat = Pattern::compile(p.impl_->re.exact_literal, fopt);
            return find(fixed_pat, opt, stats);
        }
        std::string lit;
        std::vector<uint32_t> cv;
        if (p.impl_->opt.case_mode != CaseMode::Insensitive && !p.impl_->re.branch_mandatory.empty()) {
            for (const auto& branch : p.impl_->re.branch_mandatory) {
                std::string best_lit;
                for (const auto& m : branch) {
                    if (m.size() > best_lit.size()) best_lit = m;
                }
                if (!best_lit.empty()) {
                    auto bcv = chunk_candidates(I, best_lit);
                    cv.insert(cv.end(), bcv.begin(), bcv.end());
                }
            }
            std::sort(cv.begin(), cv.end());
            cv.erase(std::unique(cv.begin(), cv.end()), cv.end());
        } else {
            if (p.impl_->opt.case_mode != CaseMode::Insensitive) {
                for (auto& m : p.impl_->re.mandatory) {
                    if (m.size() > lit.size()) lit = m;
                }
            }
            cv = chunk_candidates(I, lit);
        }
        std::vector<uint32_t> files;
        for (auto ci : cv) {
            if (files.empty() || files.back() != I.chunks[ci].file_id) {
                files.push_back(I.chunks[ci].file_id);
            }
        }
        for (auto fid : files) {
            if (!opt.include_binary && I.infos[fid].binary) continue;
            auto const& data = I.loaded[fid].data;
            if (!lit.empty() && lit.size() < 4 && p.impl_->opt.case_mode == CaseMode::Sensitive &&
                data.find(lit) == std::string::npos) continue;
            if (stats) stats->verified_bytes += data.size();
            const auto remain = [&]() { return opt.max_matches ? opt.max_matches - out.size() : 0; };
            if (p.impl_->opt.multiline) {
                auto ms = detail::regex_find_all(p.impl_->re, data, p.impl_->opt, opt.overlapping, fid, 0, remain(), opt.record_separator);
                out.insert(out.end(), ms.begin(), ms.end());
            } else {
                std::size_t b = 0;
                while (b < data.size() || (b == 0 && data.empty())) {
                    auto e = data.find(static_cast<char>(opt.record_separator), b);
                    bool term = (e != std::string::npos);
                    if (!term) e = data.size();
                    std::size_t logical_e = e;
                    if (p.impl_->opt.crlf && opt.record_separator == '\n' && logical_e > b && data[logical_e - 1] == '\r')
                        --logical_e;
                    auto rec = std::string_view(data).substr(b, logical_e - b);
                    auto ms = detail::regex_find_all(p.impl_->re, rec, p.impl_->opt, opt.overlapping, fid, b, remain(), opt.record_separator);
                    out.insert(out.end(), ms.begin(), ms.end());
                    if (opt.max_matches && out.size() >= opt.max_matches) break;
                    if (!term) break;
                    b = e + 1;
                }
            }
            if (opt.max_matches && out.size() >= opt.max_matches) break;
        }
    }

done:
    if (opt.max_matches && out.size() > opt.max_matches) {
        out.resize(opt.max_matches);
    }
    if (stats) stats->matches = out.size();
    return out;
}

std::vector<uint32_t> Searcher::files(const Pattern& p, SearchOptions opt, SearchStats* stats) const {
    if (!index_ || !index_->impl_) throw std::runtime_error("pergrep: empty index");
    auto& I = *index_->impl_;
    auto find_opt = opt;
    find_opt.max_matches = 0;
    auto ms = find(p, find_opt, stats);
    std::vector<uint8_t> hit(I.infos.size(), 0);
    for (const auto& m : ms) {
        if (m.file_id < hit.size()) hit[m.file_id] = 1;
    }
    std::vector<uint32_t> out;
    if (opt.files_without_match) {
        for (uint32_t i = 0; i < hit.size(); ++i) {
            if (!opt.include_binary && I.infos[i].binary) continue;
            if (!hit[i]) {
                out.push_back(i);
                if (opt.max_matches && out.size() >= opt.max_matches) break;
            }
        }
    } else {
        for (uint32_t i = 0; i < hit.size(); ++i) {
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
