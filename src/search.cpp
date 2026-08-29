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
    return cp == '_' || u_isalnum(cp) || u_hasBinaryProperty(cp, UCHAR_JOIN_CONTROL) ||
           u_charType(cp) == U_NON_SPACING_MARK || u_charType(cp) == U_COMBINING_SPACING_MARK;
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

std::vector<uint32_t> planned_hashes(const detail::IndexData& I, std::string_view q) {
    std::vector<uint32_t> h;
    if (q.size() < 4) return h;
    for (size_t i = 0; i + 4 <= q.size(); ++i)
        h.push_back(detail::hash4((const unsigned char*)q.data() + i));
    std::sort(h.begin(), h.end(), [&](uint32_t a, uint32_t b) {
        auto fa = I.qgram_freq[a & 65535u], fb = I.qgram_freq[b & 65535u];
        if (fa != fb) return fa < fb;
        return a < b;
    });
    h.erase(std::unique(h.begin(), h.end()), h.end());
    if (h.size() > I.opt.planned_qgrams) h.resize(I.opt.planned_qgrams);
    return h;
}

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
        // Regex: use the longest conservative mandatory literal for chunk pruning. Positional block pruning is only used when the whole regex is literal-equivalent; otherwise arbitrary prefix/suffix width makes block pruning unsafe.
        std::string lit;
        if (p.impl_->opt.case_mode != CaseMode::Insensitive) {
            for (auto& m : p.impl_->re.mandatory) {
                if (m.size() > lit.size()) lit = m;
            }
        }
        auto cv = chunk_candidates(I, lit);
        if (stats) stats->candidate_chunks += cv.size();
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
