#include <pergrep/pergrep.hpp>
#include "../src/internal.hpp"
#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace pergrep;

namespace {
struct Case {
    std::vector<Document> docs;
    std::string expr;
    PatternOptions po{};
    SearchOptions so{};
    IndexOptions io{};
    std::uint32_t seed = 0;
    bool bounded = false;
};
struct Result {
    bool ok = true;
    std::string stage;
    SearchStats stats{};
};

std::string esc(std::string_view s) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (unsigned char c : s) {
        if (c >= 0x20 && c < 0x7f && c != '\\') out << char(c);
        else if (c == '\\') out << "\\\\";
        else out << "\\x" << std::setw(2) << unsigned(c);
    }
    return out.str();
}
std::string po_text(const PatternOptions& o) {
    std::ostringstream s;
    s << "kind=" << (o.kind == PatternKind::Fixed ? "fixed" : "regex")
      << ",case=" << int(o.case_mode) << ",engine=" << int(o.engine)
      << ",word=" << o.word << ",line=" << o.line << ",multiline=" << o.multiline
      << ",dotall=" << o.dotall << ",unicode=" << o.unicode << ",crlf=" << o.crlf;
    return s.str();
}
std::string so_text(const SearchOptions& o) {
    std::ostringstream s;
    s << "overlapping=" << o.overlapping << ",invert=" << o.invert_match
      << ",with_files=" << o.files_with_matches << ",without_files=" << o.files_without_match
      << ",include_binary=" << o.include_binary << ",max_matches=" << o.max_matches
      << ",separator=" << unsigned(o.record_separator);
    return s.str();
}
std::string stat_text(const SearchStats& s) {
    std::ostringstream out;
    out << "verifier=" << s.verifier << ",physical_operator=" << s.physical_operator
        << ",fallback=" << s.verifier_fallback << ",fallback_reason=" << s.qgram_fallback_reason
        << ",plan_key_hash=" << s.plan_key_hash << ",semantic_mode=" << s.semantic_mode
        << ",verified_bytes=" << s.verified_bytes;
    return out.str();
}
void add_capture(Match& m, std::size_t begin, std::size_t end);
void add_match(std::vector<Match>& out, std::uint32_t file_id, std::size_t begin, std::size_t end, bool include_full_capture = true) {
    Match m{};
    m.file_id = file_id;
    m.start = begin;
    m.end = end;
    if (include_full_capture) add_capture(m, begin, end);
    out.push_back(std::move(m));
}
void add_capture(Match& m, std::size_t begin, std::size_t end) {
    Capture capture{};
    capture.start = begin;
    capture.end = end;
    capture.matched = true;
    capture.name = "";
    m.captures.push_back(std::move(capture));
}
bool same(const Capture& a, const Capture& b) {
    return a.start == b.start && a.end == b.end && a.matched == b.matched && a.name == b.name;
}
bool same(const std::vector<Match>& a, const std::vector<Match>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].file_id != b[i].file_id || a[i].start != b[i].start || a[i].end != b[i].end ||
            a[i].captures.size() != b[i].captures.size()) return false;
        for (std::size_t g = 0; g < a[i].captures.size(); ++g)
            if (!same(a[i].captures[g], b[i].captures[g])) return false;
    }
    return true;
}

bool ascii_upper(unsigned char c) { return c >= 'A' && c <= 'Z'; }
bool ascii_word(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
bool digits(std::string_view s, std::size_t begin, std::size_t count) {
    if (begin + count > s.size()) return false;
    for (std::size_t i = 0; i < count; ++i)
        if (s[begin + i] < '0' || s[begin + i] > '9') return false;
    return true;
}

// Independent byte-level oracle for the deterministic generated language below.
// It deliberately does not call pergrep's parser, NFA, VM, or Searcher.
std::vector<Match> reference_matches(const Case& c) {
    const unsigned kind = c.seed % 14;
    std::vector<Match> out;
    for (std::uint32_t fid = 0; fid < c.docs.size(); ++fid) {
        const std::string_view s = c.docs[fid].content;
        auto add_records = [&](char sep, auto&& fn) {
            std::size_t begin = 0;
            while (begin <= s.size()) {
                std::size_t end = s.find(sep, begin);
                if (end == std::string_view::npos) end = s.size();
                std::size_t logical_end = end;
                if (c.po.crlf && sep == '\n' && logical_end > begin && s[logical_end - 1] == '\r') --logical_end;
                fn(begin, logical_end);
                if (end == s.size()) break;
                begin = end + 1;
            }
        };
        if (kind == 5) {
            add_records('\n', [&](std::size_t begin, std::size_t end) {
                if (end - begin < 7 || s.substr(begin, 3) != "foo") return;
                for (std::size_t n = 1; n <= 3; ++n)
                    if (begin + 3 + n + 3 == end && digits(s, begin + 3, n) && s.substr(begin + 3 + n, 3) == "bar") {
                        add_match(out, fid, begin, end);
                        break;
                    }
            });
        } else if (kind == 11) {
            add_records('\0', [&](std::size_t begin, std::size_t end) {
                if (s.substr(begin, end - begin) == "needle") add_match(out, fid, begin, end);
            });
        } else if (kind == 13) {
            add_records('|', [&](std::size_t begin, std::size_t end) {
                if (end - begin < 7 || s.substr(begin, 3) != "foo") return;
                for (std::size_t n = 1; n <= 3; ++n)
                    if (begin + 3 + n + 3 == end && digits(s, begin + 3, n) && s.substr(begin + 3 + n, 3) == "bar") {
                        add_match(out, fid, begin, end);
                        break;
                    }
            });
        } else {
            for (std::size_t at = 0; at < s.size(); ++at) {
                std::size_t end = std::string_view::npos;
                if (kind == 0 && s.substr(at, 3) == "pre") {
                    for (const auto alt : {std::string_view("foo"), std::string_view("bar")}) {
                        if (s.substr(at + 3, alt.size()) != alt) continue;
                        for (std::size_t n = 3; n >= 1; --n)
                            if (digits(s, at + 3 + alt.size(), n) && s.substr(at + 3 + alt.size() + n, 4) == "post") {
                                Match m{};
                                m.file_id = fid; m.start = at; m.end = at + 3 + alt.size() + n + 4;
                                add_capture(m, at, m.end);
                                add_capture(m, at + 3, at + 3 + alt.size());
                                add_capture(m, at + 3 + alt.size(), at + 3 + alt.size() + n);
                                out.push_back(std::move(m));
                                end = std::string_view::npos;
                                break;
                            }
                        if (end != std::string_view::npos) break;
                    }
                } else if (kind == 1 && s.substr(at, 3) == "pre") {
                    for (std::size_t n = 1; n <= 3; ++n)
                        if (s.substr(at + 3, n) == std::string(n, 'a') && s[at + 3 + n] == 'b' &&
                            s.substr(at + 4 + n, 4) == "post") {
                            end = at + 4 + n + 4;
                            break;
                        }
                } else if (kind == 2 && s.substr(at, 3) == "foo") {
                    for (std::size_t gap = 0; gap <= 12; ++gap)
                        if (s.substr(at + 3 + gap, 3) == "bar") { end = at + 6 + gap; break; }
                } else if (kind == 3 && (s.substr(at, 3) == "foo" || s.substr(at, 3) == "bar") &&
                           (at < 3 || (s.substr(at - 3, 3) != "foo" && s.substr(at - 3, 3) != "bar"))) {
                    std::size_t cursor = at;
                    for (std::size_t repeat = 0; repeat < 2; ++repeat) {
                        if (s.substr(cursor, 3) == "foo" || s.substr(cursor, 3) == "bar") cursor += 3;
                        else break;
                    }
                    if (cursor >= at + 3) {
                        Match m{}; m.file_id = fid; m.start = at; m.end = cursor;
                        add_capture(m, at, cursor); add_capture(m, cursor - 3, cursor);
                        out.push_back(std::move(m));
                        end = std::string_view::npos;
                    }
                } else if (kind == 4 && at + 4 <= s.size() && ascii_upper(static_cast<unsigned char>(s[at])) &&
                           (at == 0 || !ascii_upper(static_cast<unsigned char>(s[at - 1])))) {
                    for (std::size_t n = 3; n >= 1; --n)
                        if (at + n + 3 <= s.size() && std::all_of(s.begin() + at, s.begin() + at + n,
                                                                    [](char c) { return ascii_upper(static_cast<unsigned char>(c)); }) &&
                            s.substr(at + n, 3) == "foo") {
                            add_match(out, fid, at, at + n + 3);
                        }
                } else if (kind == 6 && s.substr(at, 3) == "foo" && at >= 3 && s.substr(at - 3, 3) == "pre" && s.substr(at + 3, 3) == "bar") {
                    end = at + 3;
                } else if (kind == 7 && s.substr(at, 6) == "needle") {
                    end = at + 6;
                } else if (kind == 8 && (s.substr(at, 2) == "\xc3\xa9" || s.substr(at, 6) == "\xe4\xb8\x96\xe7\x95\x8c")) {
                    const std::size_t prefix = s.substr(at, 2) == "\xc3\xa9" ? 2 : 6;
                    for (std::size_t n = 2; n >= 1; --n)
                        if (at + prefix + n <= s.size() && std::all_of(s.begin() + at + prefix, s.begin() + at + prefix + n,
                                                                        [](char c) { return ascii_upper(static_cast<unsigned char>(c)); })) {
                            Match m{}; m.file_id = fid; m.start = at; m.end = at + prefix + n;
                            add_capture(m, at, m.end); add_capture(m, at, at + prefix);
                            out.push_back(std::move(m));
                            end = std::string_view::npos;
                            break;
                        }
                } else if (kind == 9 && s.substr(at, 3) == "foo") {
                    for (std::size_t n = 3; n >= 1; --n)
                        if (digits(s, at + 3, n) && s.substr(at + 3 + n, 3) == "bar" &&
                            (at == 0 || !ascii_word(static_cast<unsigned char>(s[at - 1]))) &&
                            (at + 6 + n >= s.size() || !ascii_word(static_cast<unsigned char>(s[at + 6 + n]))) ) {
                            end = at + 6 + n; break;
                        }
                } else if (kind == 10 && s.substr(at, 3) == "aba") {
                    end = at + 3;
                } else if (kind == 12 && s.substr(at, 7) == std::string("foo\0bar", 7)) {
                    end = at + 7;
                }
                if (end != std::string_view::npos) add_match(out, fid, at, end, kind != 10 && kind != 12);
            }
        }
    }
    if (c.so.max_matches && out.size() > c.so.max_matches) out.resize(c.so.max_matches);
    return out;
}

struct Bounds { std::size_t begin = 0, end = 0; };
Bounds record_bounds(std::string_view s, std::size_t at, unsigned char sep, bool crlf) {
    const auto b = s.rfind(char(sep), at ? at - 1 : 0);
    const std::size_t begin = b == std::string_view::npos ? 0 : b + 1;
    auto e = s.find(char(sep), at);
    if (e == std::string_view::npos) e = s.size();
    if (crlf && sep == '\n' && e > begin && s[e - 1] == '\r') --e;
    return {begin, e};
}
std::vector<Match> bounded_region_reference(std::string_view source, const Pattern& p, const Case& c, const Match& m) {
    auto q = detail::parse_regex(p.expression(), p.options());
    const auto b = record_bounds(source, m.start, c.so.record_separator, p.options().crlf);
    detail::VerifierContext x{};
    x.source = source; x.source_end = source.size(); x.record_begin = b.begin; x.record_end = b.end;
    x.candidate_begin = m.start; x.candidate_end = std::min<std::size_t>(b.end + 1, m.start + 1);
    x.separator = c.so.record_separator; x.crlf = p.options().crlf;
    x.left_context_available = m.start > b.begin; x.right_context_available = m.end < b.end;
    const bool left = p.expression().find('^') != std::string::npos || p.expression().find("\\A") != std::string::npos;
    const bool right = p.expression().find('$') != std::string::npos || p.expression().find("\\z") != std::string::npos;
    x.region_begin = left ? b.begin : (m.start > b.begin + 24 ? m.start - 24 : b.begin);
    x.region_end = right ? b.end : std::min(b.end, std::max(m.end + 24, m.start + 1));
    if (x.region_end <= x.region_begin) x.region_end = std::min(b.end, x.region_begin + 1);
    x.bounded_region = true;
    return detail::regex_find_all(q, x, p.options(), c.so.overlapping, m.file_id, 0);
}

void report(const Case& c, const Result& r) {
    std::cerr << "region-verifier property failure stage=" << r.stage << " seed=" << c.seed << '\n'
              << "pattern=" << esc(c.expr) << '\n'
              << "pattern_options=" << po_text(c.po) << '\n'
              << "search_options=" << so_text(c.so) << '\n'
              << "chunk_bytes=" << c.io.chunk_bytes << " chunk_overlap=" << c.io.chunk_overlap
              << " positional_block_bytes=" << c.io.positional_block_bytes << '\n'
              << "stats=" << stat_text(r.stats) << '\n';
    for (const auto& d : c.docs) std::cerr << "document path=" << esc(d.path) << " content=" << esc(d.content) << '\n';
}
std::string noise(std::mt19937& r, std::size_t n) {
    static constexpr std::string_view alphabet = "xyz0123 _-";
    std::string s; s.reserve(n);
    for (std::size_t i = 0; i < n; ++i) s += alphabet[r() % alphabet.size()];
    return s;
}
Case make_case(std::uint32_t seed) {
    std::mt19937 r(seed);
    Case c; c.seed = seed; c.io.chunk_bytes = 64; c.io.chunk_overlap = 16; c.io.positional_block_bytes = 16;
    const unsigned kind = seed % 14; const unsigned variant = (seed / 14) % 4;
    const std::size_t at = 61 + (seed * 17u) % 180; std::string body = noise(r, at);
    switch (kind) {
    case 0: c.expr = R"(pre(foo|bar)([0-9]{1,3})post)"; body += "prefoo12post prefoo9999post prefoo12oops"; break;
    case 1: c.expr = R"(prea{1,3}?bpost)"; body += "preaaabpost preaaaabpost prebpost"; c.bounded = true; break;
    case 2: c.expr = R"(foo.{0,12}bar)"; body += "foo" + noise(r, seed % 8) + "bar foo" + noise(r, 13) + "bar"; c.bounded = true; break;
    case 3: c.expr = R"((foo|bar){1,2})"; body += "foobar fooxbar"; break;
    case 4: c.expr = R"([A-Z]{1,3}foo)"; body += "ABCfoo ABCDbar"; break;
    case 5: c.expr = R"(^foo[0-9]{1,3}bar$)"; c.po.multiline = true; c.po.line = true; c.po.crlf = variant % 2; body.push_back('\n'); body += "foo12bar"; body += c.po.crlf ? "\r\n" : "\n"; body += "foo1234bar"; c.bounded = true; break;
    case 6: c.expr = R"((?<=pre)foo(?=bar))"; c.po.engine = Engine::Pcre2Compat; body += "prefoobar prefooxbar"; break;
    case 7: c.expr = R"((?!bad)needle)"; c.po.engine = Engine::Pcre2Compat; body += "needle badneedle"; break;
    case 8: c.expr = R"((é|世界)[A-Z]{1,2})"; body += "éAB éaB 世界CD 世界cD"; break;
    case 9: c.expr = R"(\bfoo[0-9]{1,3}bar\b)"; c.po.word = true; body += " foo12bar xfoo12bar foo1234bar "; c.bounded = true; break;
    case 10: c.expr = R"(aba)"; c.so.overlapping = true; body += "ababa xabx"; break;
    case 11: c.expr = R"(^needle$)"; c.po.multiline = true; c.so.record_separator = 0; c.so.include_binary = true; body = noise(r, at); body += "\0needle\0tail\0badneedle\0"; break;
    case 12: c.expr = R"(foo\x00bar)"; c.so.include_binary = true; body += std::string("foo\0bar\n", 8) + "fooXbar"; break;
    default: c.expr = R"(^foo[0-9]{1,3}bar$)"; c.so.record_separator = '|'; body += "|foo12bar|foo1234bar|"; c.bounded = true; break;
    }
    c.docs = kind == 11 ? std::vector<Document>{{"binary.bin", body}}
                         : std::vector<Document>{{"a.txt", body}, {"b.txt", noise(r, 37) + body.substr(at / 2)}};
    return c;
}

Result check(const Case& c, bool direct) {
    Result result;
    try {
        const auto index = Index::from_documents(c.docs, c.io);
        const auto pattern = Pattern::compile(c.expr, c.po);
        const auto got = Searcher(index).find(pattern, c.so, &result.stats);
        const auto want = reference_matches(c);
        if (!same(got, want)) {
            result.ok = false; result.stage = "indexed-vs-independent-reference";
            return result;
        }
        if (direct && c.bounded) for (const auto& match : want) {
            const auto region = bounded_region_reference(c.docs[match.file_id].content, pattern, c, match);
            if (!same(region, {match})) { result.ok = false; result.stage = "region-vs-independent-reference"; return result; }
        }
        const unsigned kind = c.seed % 14;
        if (kind == 6 || kind == 7) {
            if (result.stats.physical_operator != "RegexBruteForce" || !result.stats.verifier_fallback || result.stats.qgram_fallback_reason != "lookaround") {
                result.ok = false; result.stage = "lookaround-fallback-not-observable"; return result;
            }
        }
    } catch (...) { result.ok = false; result.stage = "compile-or-execution-error"; }
    return result;
}

Case minimize_case(Case c) {
    const auto fails = [](const Case& candidate) {
        const auto result = check(candidate, true);
        return !result.ok || (candidate.bounded && result.stats.physical_operator != "RegexBoundedRegion");
    };
    for (std::size_t di = 0; di < c.docs.size(); ++di) {
        for (std::size_t width = c.docs[di].content.size() / 2; width; width /= 2) {
            bool changed = true;
            while (changed) {
                changed = false;
                for (std::size_t at = 0; at + width <= c.docs[di].content.size(); ++at) {
                    auto candidate = c;
                    candidate.docs[di].content.erase(at, width);
                    if (fails(candidate)) { c = std::move(candidate); changed = true; break; }
                }
            }
        }
    }
    return c;
}

} // namespace
int main() {
    std::size_t bounded = 0;
    constexpr std::uint32_t first_seed = 0x36;
    constexpr std::uint32_t case_count = 14 * 10;
    for (std::uint32_t seed = first_seed; seed < first_seed + case_count; ++seed) {
        auto c = make_case(seed);
        const auto result = check(c, true);
        if (!result.ok || (c.bounded && result.stats.physical_operator != "RegexBoundedRegion")) {
            auto minimized = minimize_case(c);
            auto minimized_result = check(minimized, true);
            if (minimized.bounded && minimized_result.stats.physical_operator != "RegexBoundedRegion") {
                minimized_result.ok = false;
                minimized_result.stage = "bounded-region-operator-not-selected";
            }
            report(minimized, minimized_result);
            return 1;
        }
        if (result.stats.physical_operator == "RegexBoundedRegion") ++bounded;
    }
    if (!bounded) { std::cerr << "region-verifier property failure stage=no-bounded-region-cases\n"; return 1; }
    std::cout << "region-verifier properties: " << case_count << " deterministic cases, " << bounded
              << " bounded-region executions\n";
    return 0;
}
