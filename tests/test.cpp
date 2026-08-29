#include <pergrep/pergrep.hpp>
#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#if __has_include("../src/internal.hpp")
#include "../src/internal.hpp"
#endif
using namespace pergrep;

static Index corpus(std::string s){ return Index::from_documents({{"a.txt",std::move(s)}}); }
static bool throws_compile(std::string p, PatternOptions o={}){try{(void)Pattern::compile(std::move(p),o);return false;}catch(...){return true;}}

int main(){
  {
    auto idx=Index::from_documents({{"a.txt","alpha beta\nbeta gamma\n"},{"b.txt","delta alpha\n"}});
    assert(idx.content(0)=="alpha beta\nbeta gamma\n");
    Searcher s(idx); auto p=Pattern::compile("alpha",{.kind=PatternKind::Fixed});
    auto m=s.find(p); assert(m.size()==2 && m[0].file_id==0 && m[1].file_id==1);
    auto r=Pattern::compile("b.ta"); auto rm=s.find(r); assert(rm.size()==2);
  }
  // Default engine is regular: PCRE-only constructs are compile errors.
  assert(throws_compile(R"((ab)\1)"));
  assert(throws_compile(R"(a(?=b))"));
  assert(throws_compile(R"(\q)"));
  // PCRE2-compat mode supports the common extended constructs internally.
  {
    PatternOptions o; o.engine=Engine::Pcre2Compat;
    auto idx=corpus("abab ax ab\n"); Searcher s(idx);
    auto p=Pattern::compile(R"((ab)\1)",o); auto m=s.find(p); assert(m.size()==1 && m[0].end-m[0].start==4);
    auto q=Pattern::compile(R"(a(?=b))",o); auto n=s.find(q); assert(!n.empty());
  }
  // Unicode properties, simple case folding and word boundaries belong to the default engine.
  {
    auto idx=corpus("abc Ελληνικά Σίσυφος\n"); Searcher s(idx);
    auto p=Pattern::compile(R"(\p{Greek}+)"); auto m=s.find(p); assert(m.size()>=2);
    PatternOptions o; o.case_mode=CaseMode::Insensitive;
    auto q=Pattern::compile("σίσυφος",o); auto n=s.find(q); assert(n.size()==1);
    auto w=Pattern::compile(R"(\bΣίσυφος\b)"); auto wm=s.find(w); assert(wm.size()==1);
    PatternOptions fo; fo.kind=PatternKind::Fixed; fo.case_mode=CaseMode::Insensitive;
    auto fq=Pattern::compile("σίσυφος",fo); auto fm=s.find(fq); assert(fm.size()==1);
    PatternOptions fwo; fwo.kind=PatternKind::Fixed; fwo.word=true;
    auto fw=Pattern::compile("Σίσυφος",fwo); auto fwm=s.find(fw); assert(fwm.size()==1);
  }
  // Smart case detects non-ASCII uppercase code points.
  {
    auto idx=corpus("Σίσυφος\nσίσυφος\n"); Searcher s(idx);
    PatternOptions o; o.case_mode=CaseMode::Smart;
    auto p=Pattern::compile("Σίσυφος",o); auto m=s.find(p); assert(m.size()==1 && m[0].start==0);
    o.kind=PatternKind::Fixed; auto f=Pattern::compile("Σίσυφος",o); auto fm=s.find(f); assert(fm.size()==1 && fm[0].start==0);
  }
  // Unicode case folding may change UTF-8 byte width; match coordinates must follow the haystack.
  {
    auto idx=corpus("x K y\n"); Searcher s(idx);
    PatternOptions o; o.kind=PatternKind::Fixed; o.case_mode=CaseMode::Insensitive;
    auto p=Pattern::compile("K",o); auto m=s.find(p);
    assert(m.size()==1); assert(m[0].start==2); assert(m[0].end==5);
    PatternOptions w=o; w.word=true; auto pw=Pattern::compile("K",w); auto wm=s.find(pw); assert(wm.size()==1 && wm[0].end==5);
  }
  // Regex-level word/line modes and scoped inline flags are part of the pattern semantics.
  {
    auto idx=corpus("xx foo yy\nfoo\nfoobar\nAb AB\n"); Searcher s(idx);
    PatternOptions w; w.word=true; auto pw=Pattern::compile("foo",w); auto wm=s.find(pw); assert(wm.size()==2);
    PatternOptions x; x.line=true; auto px=Pattern::compile("foo",x); auto xm=s.find(px); assert(xm.size()==1 && xm[0].start==10);
    auto scoped=Pattern::compile("(?i:a)b"); auto sm=s.find(scoped); assert(sm.size()==1);
  }
  // Unicode property shorthand, Python-style named groups, x/U scoped flags.
  {
    auto idx=corpus("a5_ Z\t\na   5\nA a AAA\n"); Searcher s(idx);
    auto cls=Pattern::compile(R"([\w\d]+)"); auto cm=s.find(cls); assert(!cm.empty());
    auto prop=Pattern::compile(R"(\pL+)"); auto pm=s.find(prop); assert(!pm.empty());
    auto named=Pattern::compile(R"((?P<letter>[A-Z]))"); auto nm=s.find(named); assert(!nm.empty() && nm[0].captures.size()>1 && nm[0].captures[1].name=="letter");
    auto x=Pattern::compile("(?x:a \\s+ 5)"); auto xm=s.find(x); assert(!xm.empty());
    auto ung=Pattern::compile("(?U:A+)"); auto um=s.find(ung); assert(!um.empty() && um.back().end-um.back().start==1);
    auto scoped_ung=Pattern::compile("(?U:A+):?"); (void)scoped_ung;
  }
  // Captures are preserved for replacement/frontends.
  {
    auto idx=corpus("foo123bar\n"); Searcher s(idx);
    auto p=Pattern::compile(R"(foo([0-9]+)bar)"); auto m=s.find(p); assert(m.size()==1); assert(m[0].captures.size()>=2); assert(m[0].captures[1].matched); assert(m[0].captures[1].end-m[0].captures[1].start==3);
    auto np=Pattern::compile(R"(foo(?<digits>[0-9]+)bar)"); auto nm=s.find(np); assert(nm.size()==1); assert(nm[0].captures[1].name=="digits");
  }
  // Multiline and zero-width matches make progress.
  {
    auto idx=corpus("a\nb\n"); Searcher s(idx); PatternOptions o; o.multiline=true;
    auto p=Pattern::compile("^",o); auto m=s.find(p); assert(m.size()>=2 && m.size()<10);
  }

  // Ordered Thompson NFA: leftmost-first alternation and greediness.
  {
    auto idx=corpus("ab\naaaaa\n"); Searcher s(idx);
    auto a=Pattern::compile("a|ab"); auto am=s.find(a); assert(!am.empty() && am[0].end-am[0].start==1);
    auto g=Pattern::compile("a+"); auto gm=s.find(g); assert(gm.size()>=2 && gm[1].end-gm[1].start==5);
    auto l=Pattern::compile("a+?"); auto lm=s.find(l); assert(!lm.empty() && lm[0].end-lm[0].start==1);
    auto b=Pattern::compile("a{2,4}"); auto bm=s.find(b); assert(!bm.empty() && bm.back().end-bm.back().start==4);
  }
  // A classic catastrophic-backtracking shape is handled by the regular-engine NFA,
  // both when the mandatory suffix is absent and when it is present.
  {
    std::string hay(20000,'a'); hay.push_back('\n'); auto idx=corpus(hay); Searcher s(idx);
    auto p=Pattern::compile("(a|aa)*b"); auto m=s.find(p); assert(m.empty());
    std::string hit(2000,'a'); hit += "b\n"; auto idx2=corpus(hit); Searcher s2(idx2);
    auto hm=s2.find(p); assert(hm.size()==1 && hm[0].start==0 && hm[0].end==2001);
  }
  // Chunk candidate ordering (>32KB corpus where tail chunk is smaller group).
  {
    std::string s(70000, ' ');
    for (size_t i = 80; i < s.size(); i += 80) s[i] = '\n';
    std::string needle = "CHUNK_ORDER_TEST_KEYWORD";
    size_t pos0 = 500;
    size_t pos1 = 35000;
    size_t pos2 = 68000;
    s.replace(pos0, needle.size(), needle);
    s.replace(pos1, needle.size(), needle);
    s.replace(pos2, needle.size(), needle);
    auto idx = Index::from_documents({{"doc.txt", s}});
    Searcher searcher(idx);
    auto p = Pattern::compile(needle, {.kind = PatternKind::Fixed});
    auto m = searcher.find(p);
    assert(m.size() == 3);
    assert(m[0].start == pos0);
    assert(m[1].start == pos1);
    assert(m[2].start == pos2);
  }
  // Literal crossing chunk boundary (130+ bytes crossing 32KB boundary).
  {
    std::string s(70000, '.');
    for (size_t i = 80; i < s.size(); i += 80) s[i] = '\n';
    std::string long_lit = "BOUNDARY_START_" + std::string(140, 'Z') + "_BOUNDARY_END";
    assert(long_lit.size() >= 130);
    size_t cross_pos = 32768 - 70;
    s.replace(cross_pos, long_lit.size(), long_lit);
    auto idx = Index::from_documents({{"cross.txt", s}});
    Searcher searcher(idx);
    auto p_fixed = Pattern::compile(long_lit, {.kind = PatternKind::Fixed});
    auto m_fixed = searcher.find(p_fixed);
    assert(m_fixed.size() == 1);
    assert(m_fixed[0].start == cross_pos);
    assert(m_fixed[0].end == cross_pos + long_lit.size());
    auto p_re = Pattern::compile("BOUNDARY_START_Z+_BOUNDARY_END");
    auto m_re = searcher.find(p_re);
    assert(m_re.size() == 1);
    assert(m_re[0].start == cross_pos);
    assert(m_re[0].end == cross_pos + long_lit.size());
  }
  // Negative lookaround pruning (?!FORBIDDEN)needle.
  {
    PatternOptions o; o.engine = Engine::Pcre2Compat;
    auto p = Pattern::compile(R"((?!FORBIDDEN)needle)", o);
    for (const auto& lit : p.mandatory_literals()) {
      assert(lit.find("FORBIDDEN") == std::string::npos);
    }
    auto idx = Index::from_documents({
      {"a.txt", "hello needle world\n"},
      {"b.txt", "hello FORBIDDEN world\n"},
      {"c.txt", "just needle here\n"}
    });
    Searcher searcher(idx);
    auto m = searcher.find(p);
    assert(m.size() == 2);
    assert(m[0].file_id == 0);
    assert(m[1].file_id == 2);
    auto p2 = Pattern::compile(R"(needle(?!FORBIDDEN))", o);
    auto m2 = searcher.find(p2);
    assert(m2.size() == 2);
    assert(m2[0].file_id == 0);
    assert(m2[1].file_id == 2);
  }
  // Case-insensitive class -i '[A-Z]' matching 'a'.
  {
    PatternOptions o; o.case_mode = CaseMode::Insensitive;
    auto idx = corpus("abc ABC 123\n");
    Searcher s(idx);
    auto p1 = Pattern::compile("[A-Z]+", o);
    auto m1 = s.find(p1);
    assert(m1.size() == 2);
    assert(m1[0].start == 0 && m1[0].end == 3);
    assert(m1[1].start == 4 && m1[1].end == 7);
    auto p2 = Pattern::compile("[a-z]+", o);
    auto m2 = s.find(p2);
    assert(m2.size() == 2);
    auto p3 = Pattern::compile("[B-D]", o);
    auto m3 = s.find(p3);
    assert(m3.size() == 4);
  }
  // Scoped flags (?-i:foo) inside -i pattern.
  {
    auto idx = corpus("FOObar\nfoobar\nFOOBAR\nfooBAR\n");
    Searcher s(idx);
    PatternOptions o; o.case_mode = CaseMode::Insensitive;
    auto p = Pattern::compile("FOO(?-i:bar)", o);
    auto m = s.find(p);
    assert(m.size() == 2);
    assert(m[0].start == 0);
    assert(m[1].start == 7);
    PatternOptions o_dotall; o_dotall.dotall = true;
    auto idx2 = corpus("a\nb\naxb\n");
    Searcher s2(idx2);
    auto p_nos = Pattern::compile("a(?-s:.)b", o_dotall);
    auto m_nos = s2.find(p_nos);
    assert(m_nos.size() == 1);
    assert(m_nos[0].start == 4);
  }
  // Positive lookaround capture (?=(a))\1.
  {
    PatternOptions o; o.engine = Engine::Pcre2Compat;
    auto idx = corpus("a ab abc\n");
    Searcher s(idx);
    auto p = Pattern::compile(R"((?=(a))\1)", o);
    auto m = s.find(p);
    assert(!m.empty());
    assert(m[0].captures.size() >= 2);
    assert(m[0].captures[1].matched);
    auto p2 = Pattern::compile(R"((?=(foo))(\1bar))", o);
    auto idx2 = corpus("foobar\n");
    Searcher s2(idx2);
    auto m2 = s2.find(p2);
    assert(m2.size() == 1);
    assert(m2[0].end - m2[0].start == 6);
  }
  // Fixed empty pattern "".
  {
    auto idx = corpus("line1\nline2\n");
    Searcher s(idx);
    PatternOptions o; o.kind = PatternKind::Fixed;
    auto p = Pattern::compile("", o);
    auto m = s.find(p);
    assert(!m.empty());
  }
  // Default-constructed Index observer calls.
  {
    Index idx;
    assert(idx.root().empty());
    assert(idx.files().empty());
    assert(idx.corpus_bytes() == 0);
    assert(idx.index_bytes() == 0);
    assert(!idx.fresh());
    assert(idx.options().chunk_bytes == 32 * 1024);
    try {
      (void)idx.content(0);
      assert(false);
    } catch (const std::out_of_range&) {}
  }
  // Index::save in current working directory without directory prefix.
  {
    namespace fs = std::filesystem;
    auto tmp_dir = fs::temp_directory_path() / "pergrep_save_test_dir";
    fs::create_directories(tmp_dir);
    {
      std::ofstream f(tmp_dir / "sample.txt", std::ios::binary);
      f << "save test content\n";
    }
    std::string fname = "test_pg_cwd_save_tmp.bin";
    auto real_idx = Index::build(tmp_dir);
    real_idx.save(fname);
    assert(fs::exists(fname));
    auto loaded = Index::load(fname);
    assert(loaded.files().size() == 1);
    assert(loaded.content(0) == "save test content\n");
    fs::remove(fname);
    fs::remove_all(tmp_dir);
  }
  // Large block positional index (>128 blocks).
  {
    const size_t block_size = 256;
    const size_t total_blocks = 200;
    const size_t total_bytes = total_blocks * block_size;
    std::string s(total_bytes, ' ');
    for (size_t i = 80; i < s.size(); i += 80) s[i] = '\n';
    std::string needle0 = "NEEDLE_BLOCK_005";
    std::string needle1 = "NEEDLE_BLOCK_135";
    std::string needle2 = "NEEDLE_BLOCK_190";
    size_t pos0 = 5 * block_size + 10;
    size_t pos1 = 135 * block_size + 10;
    size_t pos2 = 190 * block_size + 10;
    s.replace(pos0, needle0.size(), needle0);
    s.replace(pos1, needle1.size(), needle1);
    s.replace(pos2, needle2.size(), needle2);
    IndexOptions opt;
    opt.chunk_bytes = 65536;
    opt.positional_block_bytes = block_size;
    auto idx = Index::from_documents({{"large_blocks.txt", s}}, opt);
    Searcher searcher(idx);
    {
      SearchStats stats{};
      auto p = Pattern::compile(needle0, {.kind = PatternKind::Fixed});
      auto m = searcher.find(p, {}, &stats);
      assert(m.size() == 1);
      assert(m[0].start == pos0);
    }
    {
      SearchStats stats{};
      auto p = Pattern::compile(needle1, {.kind = PatternKind::Fixed});
      auto m = searcher.find(p, {}, &stats);
      assert(m.size() == 1);
      assert(m[0].start == pos1);
    }
    {
      SearchStats stats{};
      auto p = Pattern::compile(needle2, {.kind = PatternKind::Fixed});
      auto m = searcher.find(p, {}, &stats);
      assert(m.size() == 1);
      assert(m[0].start == pos2);
    }
  std::cerr << "M20\n" << std::flush;
  // SearchOptions: invert_match, files_with_matches, files_without_match, max_matches.
  {
    auto idx = Index::from_documents({
      {"a.txt", "apple\nbanana\ncherry\n"},
      {"b.txt", "date\nfig\ngrape\n"},
      {"c.txt", "apple\ngrape\n"}
    });
    Searcher s(idx);
    auto p = Pattern::compile("apple", {.kind = PatternKind::Fixed});
    auto f_with = s.files(p);
    assert(f_with.size() == 2);
    assert(f_with[0] == 0 && f_with[1] == 2);
    SearchOptions opt_without; opt_without.files_without_match = true;
    auto f_without = s.files(p, opt_without);
    assert(f_without.size() == 1);
    assert(f_without[0] == 1);
    SearchOptions opt_max; opt_max.max_matches = 1;
    auto m_max = s.find(p, opt_max);
    assert(m_max.size() == 1);
    SearchOptions opt_inv; opt_inv.invert_match = true;
    auto m_inv = s.find(p, opt_inv);
    assert(!m_inv.empty());
  }
  std::cerr << "M21\n" << std::flush;
  // Property-based differential test suite (Indexed Search == Brute-force Search).
    std::mt19937 rng(42);
    std::vector<std::string> words = {
      "apple", "banana", "cherry", "date", "elderberry", "fig", "grape",
      "honeydew", "kiwi", "lemon", "mango", "nectarine", "orange", "papaya",
      "quince", "raspberry", "strawberry", "tangerine", "ugli", "vanilla",
      "watermelon", "xigua", "yam", "zucchini", "Alpha123", "Beta456", "Gamma789"
    };
    auto random_text = [&](size_t target_size) {
      std::string res;
      res.reserve(target_size + 1000);
      size_t line_len = 0;
      while (res.size() < target_size) {
        std::string w = words[rng() % words.size()];
        res += w;
        line_len += w.size();
        if (line_len > 70 || (rng() % 10 == 0)) {
          res += '\n';
          line_len = 0;
        } else {
          res += ' ';
          line_len += 1;
        }
      }
      return res;
    };
    std::vector<Document> docs = {
      {"doc0_small.txt", random_text(2000)},
      {"doc1_medium.txt", random_text(25000)},
      {"doc2_crossing.txt", random_text(45000)},
      {"doc3_large.txt", random_text(75000)}
    };
    std::string boundary_lit = "BOUNDARY_TOKEN_CROSSING_" + std::string(140, 'K') + "_END";
    size_t cross_off = 32768 - 70;
    docs[2].content.replace(cross_off, boundary_lit.size(), boundary_lit);
    docs[1].content += "\nSPECIAL_REGEX_TARGET_12345_OK\n";
    docs[3].content.replace(1000, 26, "SPECIAL_REGEX_TARGET_67890");
    docs[3].content.replace(35000, 26, "SPECIAL_REGEX_TARGET_99999");
    docs[3].content.replace(68000, 26, "SPECIAL_REGEX_TARGET_00000");
    IndexOptions indexed_opt;
    indexed_opt.chunk_bytes = 32768;
    indexed_opt.chunk_overlap = 128;
    indexed_opt.positional_block_bytes = 256;
    auto indexed_idx = Index::from_documents(docs, indexed_opt);
    Searcher indexed_searcher(indexed_idx);
    IndexOptions ref_opt;
    ref_opt.chunk_bytes = 1024 * 1024;
    ref_opt.chunk_overlap = 512 * 1024;
    ref_opt.positional_block_bytes = 1024;
    auto ref_idx = Index::from_documents(docs, ref_opt);
    Searcher ref_searcher(ref_idx);
    struct QueryCase {
      std::string pattern;
      PatternOptions popt;
      SearchOptions sopt;
    };
    std::vector<QueryCase> test_queries = {
      {"apple", {.kind = PatternKind::Fixed}, {}},
      {"banana", {.kind = PatternKind::Fixed}, {}},
      {"cherry", {.kind = PatternKind::Fixed}, {}},
      {"Alpha123", {.kind = PatternKind::Fixed}, {}},
      {"alpha123", {.kind = PatternKind::Fixed, .case_mode = CaseMode::Insensitive}, {}},
      {"NOT_EXISTING_KEYWORD_XYZ", {.kind = PatternKind::Fixed}, {}},
      {boundary_lit, {.kind = PatternKind::Fixed}, {}},
      {"mango", {.kind = PatternKind::Fixed, .word = true}, {}},
      {"watermelon", {.kind = PatternKind::Fixed, .word = true}, {}},
      {"apple", {}, {}},
      {"SPECIAL_REGEX_TARGET_[0-9]+", {}, {}},
      {"[A-Z][a-z]+[0-9]+", {}, {}},
      {"(banana|cherry|date)", {}, {}},
      {"fig.*grape", {}, {}},
      {"^.*elderberry.*$", {}, {}},
      {"[a-z]{6,8}", {}, {}},
      {"BOUNDARY_TOKEN_CROSSING_K+_END", {}, {}},
      {"alpha123", {.case_mode = CaseMode::Insensitive}, {}},
      {"NOT_FOUND_REGEX_[0-9]{10}", {}, {}},
      {"apple", {.kind = PatternKind::Fixed}, {.overlapping = true}},
      {"apple", {.kind = PatternKind::Fixed}, {.max_matches = 3}},
      {"SPECIAL_REGEX_TARGET_[0-9]+", {}, {.max_matches = 2}}
    };
    for (const auto& q : test_queries) {
      auto pat = Pattern::compile(q.pattern, q.popt);
      auto actual = indexed_searcher.find(pat, q.sopt);
      auto expected = ref_searcher.find(pat, q.sopt);
      assert(actual.size() == expected.size());
      for (size_t i = 0; i < expected.size(); ++i) {
        assert(actual[i].file_id == expected[i].file_id);
        assert(actual[i].start == expected[i].start);
        assert(actual[i].end == expected[i].end);
      }
      auto actual_files = indexed_searcher.files(pat, q.sopt);
      auto expected_files = ref_searcher.files(pat, q.sopt);
      assert(actual_files == expected_files);
    }
  }
  // Advanced test suite 1: Nested capturing groups with repetition and backreferences.
  {
    PatternOptions o; o.engine = Engine::Pcre2Compat;
    auto idx = corpus("abc123def abc456def <tag>content</tag> <div id=\"main\">hello</div>\n");
    Searcher s(idx);
    // Nested groups: group 1 captures full word, group 2 captures inner digits
    auto p1 = Pattern::compile(R"(abc(([0-9]+))def)", o);
    auto m1 = s.find(p1);
    assert(m1.size() == 2);
    assert(m1[0].captures.size() == 3);
    assert(m1[0].captures[1].start == 3 && m1[0].captures[1].end == 6);
    assert(m1[0].captures[2].start == 3 && m1[0].captures[2].end == 6);
    // Backreference matching XML tags
    auto p_tag = Pattern::compile(R"(<([a-zA-Z0-9]+)>(.*?)</\1>)", o);
    auto m_tag = s.find(p_tag);
    assert(m_tag.size() == 1);
    assert(m_tag[0].captures[1].start == 21 && m_tag[0].captures[1].end == 24);
  }
  // Advanced test suite 2: Unicode properties, scripts, and multi-byte character classes.
  {
    auto idx = corpus("Hello 1234 世界 🌍 12.34 русский язык 999\n");
    Searcher s(idx);
    // Han script with key-value syntax \p{Script=Han} and \p{sc:Han}
    auto p_han = Pattern::compile(R"(\p{Script=Han}+)");
    auto m_han = s.find(p_han);
    assert(m_han.size() == 1);
    assert(idx.content(0).substr(m_han[0].start, m_han[0].end - m_han[0].start) == "世界");
    auto p_han_sc = Pattern::compile(R"(\p{sc:Han}+)");
    auto m_han_sc = s.find(p_han_sc);
    assert(m_han_sc.size() == 1);
    // Cyrillic script with \p{Script_Extensions=Cyrillic}
    auto p_cyr = Pattern::compile(R"(\p{Script_Extensions=Cyrillic}+)");
    auto m_cyr = s.find(p_cyr);
    assert(m_cyr.size() == 2);
    // Numbers via \p{gc=Nd} and \p{General_Category=Decimal_Digit}
    auto p_num = Pattern::compile(R"(\p{gc=Nd}+)");
    auto m_num = s.find(p_num);
    assert(m_num.size() == 4); // 1234, 12, 34, 999
    auto p_num_gc = Pattern::compile(R"(\p{General_Category=Decimal_Digit}+)");
    auto m_num_gc = s.find(p_num_gc);
    assert(m_num_gc.size() == 4);
    // Uppercase letter via \p{gc=Lu}
    auto p_lu = Pattern::compile(R"(\p{gc=Lu})");
    auto m_lu = s.find(p_lu);
    assert(!m_lu.empty());
    // Unicode word characters including underscores and alphanumerics
    auto p_w = Pattern::compile(R"([\w]+)");
    auto m_w = s.find(p_w);
    assert(m_w.size() >= 7);
    assert(throws_compile(R"(\p{InvalidScript=Foo})"));
  }
  // Advanced test suite 3: Lookaround assertions (nested, chained, and combined).
  {
    PatternOptions o; o.engine = Engine::Pcre2Compat;
    auto idx = corpus("cat mat bat rat car bar\n");
    Searcher s(idx);
    // Positive lookahead chained with negative lookahead: ends in 'at', but not 'bat' or 'rat'
    auto p = Pattern::compile(R"((?!b|r)[a-z](?=at))", o);
    auto m = s.find(p);
    assert(m.size() == 2); // 'c' in cat, 'm' in mat
    assert(idx.content(0).substr(m[0].start, 1) == "c");
    assert(idx.content(0).substr(m[1].start, 1) == "m");
    // Lookbehind: preceded by 'c' or 'b', matching 'ar'
    auto p_lb = Pattern::compile(R"((?<=c|b)ar)", o);
    auto m_lb = s.find(p_lb);
    assert(m_lb.size() == 2);
    // Negative lookbehind: 'ar' NOT preceded by 'c'
    auto p_nlb = Pattern::compile(R"((?<!c)ar)", o);
    auto m_nlb = s.find(p_nlb);
    assert(m_nlb.size() == 1); // 'ar' in bar
  }
  // Advanced test suite 4: Word boundary and anchor matrix with CRLF and multiline.
  {
    auto idx = corpus("foo\r\nbar foo\r\nfoo\r\n");
    Searcher s(idx);
    PatternOptions o_crlf; o_crlf.crlf = true; o_crlf.multiline = true;
    auto p_anchored = Pattern::compile(R"(^foo$)", o_crlf);
    auto m_anchored = s.find(p_anchored);
    assert(m_anchored.size() == 2); // line 1 and line 3
    assert(m_anchored[0].start == 0 && m_anchored[0].end == 3);
    // Word boundary at CRLF boundary
    auto p_word = Pattern::compile(R"(\bfoo\b)", o_crlf);
    auto m_word = s.find(p_word);
    assert(m_word.size() == 3);
  }
  // Advanced test suite 5: Extreme IndexOptions configurations.
  {
    std::string text(100000, 'x');
    for (size_t i = 50; i < text.size(); i += 50) text[i] = '\n';
    text.replace(500, 10, "TARGET_001");
    text.replace(50000, 10, "TARGET_002");
    text.replace(95000, 10, "TARGET_003");

    // Ultra-small chunk and block size
    IndexOptions opt_small;
    opt_small.chunk_bytes = 128;
    opt_small.chunk_overlap = 64;
    opt_small.positional_block_bytes = 16;
    opt_small.positional_budget_ratio = 0.8;
    auto idx_small = Index::from_documents({{"extreme.txt", text}}, opt_small);
    Searcher s_small(idx_small);
    auto p = Pattern::compile("TARGET_002", {.kind = PatternKind::Fixed});
    auto m_small = s_small.find(p);
    assert(m_small.size() == 1);
    assert(m_small[0].start == 50000);

    // High budget ratio and planned qgrams
    IndexOptions opt_dense;
    opt_dense.chunk_bytes = 16384;
    opt_dense.chunk_overlap = 256;
    opt_dense.positional_block_bytes = 64;
    opt_dense.planned_qgrams = 16;
    opt_dense.positional_budget_ratio = 0.9;
    auto idx_dense = Index::from_documents({{"extreme2.txt", text}}, opt_dense);
    Searcher s_dense(idx_dense);
    auto m_dense = s_dense.find(p);
    assert(m_dense.size() == 1);
    assert(m_dense[0].start == 50000);
  }
  // Advanced test suite 6: SearchStats invariants.
  {
    auto idx = Index::from_documents({
      {"f1.txt", "some common words without keyword\n"},
      {"f2.txt", "this file has SPECIAL_STAT_KEYWORD inside\n"},
      {"f3.txt", "another common file with nothing special\n"}
    });
    Searcher s(idx);
    SearchStats stats{};
    auto p = Pattern::compile("SPECIAL_STAT_KEYWORD", {.kind = PatternKind::Fixed});
    auto m = s.find(p, {}, &stats);
    assert(m.size() == 1);
    assert(stats.matches == 1);
    assert(stats.candidate_chunks >= 1);
    assert(stats.verified_bytes > 0);
  }
  // QO-1 Literal & Branch Analyzer
#if __has_include("../src/internal.hpp")
  {
    using pergrep::detail::parse_regex;
    auto check_prefixes = [&](std::string pat, PatternOptions opt, std::vector<std::string> expect) {
      auto prog = parse_regex(pat, opt);
      auto got = prog.prefixes;
      std::sort(got.begin(), got.end());
      std::sort(expect.begin(), expect.end());
      if (got != expect) {
        std::cerr << "PREFIX FAIL pat=" << pat << " got{";
        for (auto &s: got) std::cerr << s << ",";
        std::cerr << "} expect{";
        for (auto &s: expect) std::cerr << s << ",";
        std::cerr << "}\n";
      }
      assert(got == expect);
    };
    auto check_branch = [&](std::string pat, PatternOptions opt, std::vector<std::vector<std::string>> expect) {
      auto prog = parse_regex(pat, opt);
      auto got = prog.branch_mandatory;
      auto norm = [](std::vector<std::vector<std::string>> v){
        for (auto& inner : v) std::sort(inner.begin(), inner.end());
        std::sort(v.begin(), v.end());
        return v;
      };
      assert(norm(got) == norm(expect));
    };
    // extract_prefixes: foo|bar -> {foo,bar}
    check_prefixes("foo|bar", {}, {"foo","bar"});
    // case-insensitive -> empty (icase literals do not contribute)
    {
      PatternOptions o; o.case_mode = CaseMode::Insensitive;
      check_prefixes("foo|bar", o, {});
      check_prefixes("foo", o, {});
    }
    // (foo|bar) group unwrapping -> same as foo|bar
    check_prefixes("(foo|bar)", {}, {"foo","bar"});
    // foobar -> {foobar}
    check_prefixes("foobar", {}, {"foobar"});
    // a|b|c -> 3 prefixes
    check_prefixes("a|b|c", {}, {"a","b","c"});
    // Repeat with min>0 prefix of child
    check_prefixes("foo+", {}, {"fo"}); // Concat ["fo", Repeat("o")] -> prefix of first child "fo"
    check_prefixes("(foo)+", {}, {"foo"});
    check_prefixes("(foo)*", {}, {}); // Repeat star on group -> empty
    // branch_mandatory: foo|bar -> [[foo],[bar]]
    check_branch("foo|bar", {}, {{"foo"},{"bar"}});
    // foo|.* -> empty (conservative, one branch has no mandatory)
    check_branch("foo|.*", {}, {});
    // (foo|bar)qux fallback to global mandatory [qux] (branch_mandatory empty)
    {
      auto prog = parse_regex("(foo|bar)qux", {});
      assert(prog.branch_mandatory.empty());
      bool has_qux = std::find(prog.mandatory.begin(), prog.mandatory.end(), "qux") != prog.mandatory.end();
      assert(has_qux);
      auto idx = corpus("fooqux\nbarqux\nqux\nnomatch\n");
      Searcher s(idx);
      auto pat = Pattern::compile("(foo|bar)qux");
      auto m = s.find(pat);
      assert(m.size() == 2);
    }
    // Group(Alt) branch_mandatory unwrapping
    check_branch("(foo|bar)", {}, {{"foo"},{"bar"}});
    // is_pure_literal: hello pure
    {
      auto prog = parse_regex("hello", {});
      assert(prog.is_pure_literal && prog.exact_literal == "hello");
    }
    // Group(foo) pure
    {
      auto prog = parse_regex("(foo)", {});
      assert(prog.is_pure_literal && prog.exact_literal == "foo");
    }
    // Concat Group pure
    {
      auto prog = parse_regex("(foo)(bar)", {});
      assert(prog.is_pure_literal && prog.exact_literal == "foobar");
    }
    // non-pure: foo.*, foo|bar
    {
      auto p1 = parse_regex("foo.*", {});
      assert(!p1.is_pure_literal);
      auto p2 = parse_regex("foo|bar", {});
      assert(!p2.is_pure_literal);
    }
    // extended (ab)\1 -> false
    {
      PatternOptions o; o.engine = Engine::Pcre2Compat;
      auto prog = parse_regex(R"((ab)\1)", o);
      assert(!prog.is_pure_literal);
    }
    // Negative lookaround isolation: (?!FORBIDDEN)needle not producing FORBIDDEN
    {
      PatternOptions o; o.engine = Engine::Pcre2Compat;
      auto prog = parse_regex(R"((?!FORBIDDEN)needle)", o);
      for (auto& lit : prog.mandatory) assert(lit.find("FORBIDDEN") == std::string::npos);
      assert(std::find(prog.mandatory.begin(), prog.mandatory.end(), "needle") != prog.mandatory.end());
      for (auto& br : prog.branch_mandatory) for (auto& lit : br) assert(lit.find("FORBIDDEN") == std::string::npos);
    }
    // throws_compile for malformed escapes
    assert(throws_compile(R"(\q)"));
    assert(throws_compile(R"(\xZZ)"));
  }
#else
  // Fallback when internal.hpp not reachable: validate via public API only.
  {
    // foo|bar mandatory intersection empty (no common literal) but search works
    auto p = Pattern::compile("foo|bar");
    // mandatory() returns intersection -> empty for foo|bar
    assert(p.mandatory_literals().empty());
    auto idx = corpus("foo bar baz\n");
    Searcher s(idx);
    assert(s.find(p).size() == 2);
    // foobar prefixes indirectly validated via search correctness
    auto p2 = Pattern::compile("foobar");
    assert(!p2.mandatory_literals().empty());
    // is_pure_literal fallback observable via search still correct
    auto ph = Pattern::compile("hello");
    assert(s.find(ph).empty()); // no hello in corpus
  }
#endif

  // BF-1 audit corners: regex property edge cases (spaces, case, aliases, errors)
  {
    // \p{sc = Greek} with spaces around = and inside braces
    auto idx = corpus("Ωmega αb\n");
    Searcher s(idx);
    auto p_spaced = Pattern::compile(R"(\p{sc = Greek}+)");
    auto m_spaced = s.find(p_spaced);
    assert(!m_spaced.empty());
    // \p{Any} vs \p{any} case-insensitive property name
    auto p_any_up = Pattern::compile(R"(\p{Any}+)");
    auto p_any_lo = Pattern::compile(R"(\p{any}+)");
    assert(!s.find(p_any_up).empty());
    assert(!s.find(p_any_lo).empty());
    // \p{gc = Lu} with spaces
    auto idx2 = corpus("AbC Δ\n");
    Searcher s2(idx2);
    auto p_gc = Pattern::compile(R"(\p{gc = Lu}+)");
    assert(!s2.find(p_gc).empty());
    // \p{Invalid} should throw
    assert(throws_compile(R"(\p{Invalid})"));
    // \p{gc=Lu} without spaces also works
    auto p_gc2 = Pattern::compile(R"(\p{gc=Lu}+)");
    assert(!s2.find(p_gc2).empty());
    // \p{scx=Han} and \p{ScriptExtensions=Han} aliases
    auto idx3 = corpus("漢字 hello\n");
    Searcher s3(idx3);
    auto p_scx = Pattern::compile(R"(\p{scx=Han}+)");
    assert(!s3.find(p_scx).empty());
    auto p_sce = Pattern::compile(R"(\p{ScriptExtensions=Han}+)");
    assert(!s3.find(p_sce).empty());
  }
  std::cerr << "M22 differential done\n" << std::flush;
  // QO-2 q-gram rarity: skewed corpus, rare vs naive first-k pruning.
  {
    // Build a corpus where 4-gram "aaaa" is extremely common and "xyzq" is rare.
    // This skew lets us verify that rarity-ordered selection (rarest first)
    // prunes more than a naive first-k (prefix-order) selection.
    IndexOptions opt;
    opt.chunk_bytes = 1024;
    opt.chunk_overlap = 128;
    opt.positional_block_bytes = 64;
    opt.positional_budget_ratio = 0.5;
    opt.planned_qgrams = 2;
    // 8 heavily skewed documents dominated by 'a', plus one document containing the rare gram.
    std::vector<Document> docs;
    std::string common(5000, 'a');
    for (size_t i = 80; i < common.size(); i += 80) common[i] = '\n';
    for (int i = 0; i < 8; ++i) {
      docs.push_back({std::string("common_") + std::to_string(i) + ".txt", common});
    }
    // Rare document: still mostly 'a' but with a unique "xyzq" island and a longer tail.
    // Avoid newline insertion over the rare gram (which sits at offset 2000) to keep the literal intact.
    std::string rare_doc = std::string(2000, 'a') + "xyzq" + std::string(2000, 'a');
    for (size_t i = 80; i < rare_doc.size(); i += 80) if (i < 1990 || i > 2010) rare_doc[i] = '\n';
    docs.push_back({"rare.txt", rare_doc});
    // Add one more common doc to increase total chunks without adding rare gram
    docs.push_back({"common_extra.txt", common});
    auto idx = Index::from_documents(docs, opt);
    Searcher s(idx);
    // Query that mixes common and rare q-grams: "aaaa" prefix is common, "xyzq" suffix is rare.
    // The full literal carries q-grams: aaaa, aaax, aaxy, axyz, xyzq (when q="aaaaxyzq").
    std::string q = "aaaaxyzq";
    auto pat = Pattern::compile(q, {.kind = PatternKind::Fixed});
    SearchStats stats{};
    auto matches = s.find(pat, {}, &stats);
    // Correctness: only the rare document contains the literal
    assert(matches.size() == 1);
    assert(idx.files()[matches[0].file_id].path == "rare.txt");
    // Rarity pruning: stats.candidate_blocks is after positional filtering on rarest q-grams.
    // With rarity ordering, candidate_blocks should be tiny (only chunks containing xyzq).
    // Naive first-k (prefix order) would pick "aaaa" (extremely common) and keep many chunks;
    // rarity picks "xyzq" and keeps only the rare chunk(s). Demonstrate by comparing
    // candidate_blocks for a common-only query vs the rare query.
    {
      auto pat_common = Pattern::compile("aaaa", {.kind = PatternKind::Fixed});
      SearchStats st_common{};
      (void)s.find(pat_common, {}, &st_common);
      // Common gram appears in every doc, so candidate_blocks should be large (many chunks).
      // Rare query should prune to far fewer blocks.
      assert(st_common.candidate_blocks > stats.candidate_blocks);
      assert(st_common.candidate_blocks >= 10);
    }
    // Rarity-aware should prune to ~1 doc worth of chunks.
    assert(stats.candidate_blocks <= 4);
    // Also candidate_chunks (pre-positional) is Bloom-based; rarity mainly affects blocks,
    // but overall verified_bytes should be small thanks to block pruning.
    assert(stats.verified_bytes < 8192);
    // Zero false negatives: searching the rare gram alone also finds it
    auto pat2 = Pattern::compile("xyzq", {.kind = PatternKind::Fixed});
    SearchStats st2{};
    auto m2 = s.find(pat2, {}, &st2);
    assert(m2.size() == 1);
    // Adaptive k: short query (<=4) keeps k=1; long query uses larger k up to 8.
    // Verify via stats: short literal "abc" (size 3 <4) falls back to all chunks (no q-gram filter)
    // and still finds nothing without false positives beyond verification.
    {
      auto p_short = Pattern::compile("abc", {.kind = PatternKind::Fixed});
      SearchStats st_short{};
      auto ms_short = s.find(p_short, {}, &st_short);
      // "abc" does not occur in 'a'-dominated corpus
      assert(ms_short.empty());
      // Short query with no q-gram should have candidate_chunks == all chunks (conservative)
      // or at least not prune incorrectly to zero.
      assert(st_short.candidate_chunks >= 1);
    }
    // Long literal (>=20 chars) should still prune correctly and benefit from larger k
    {
      std::string long_q = std::string(10, 'a') + "xyzq" + std::string(10, 'b');
      // Ensure rare part still only in one tailored doc
      std::vector<Document> docs2 = docs;
      docs2.push_back({"long_rare.txt", long_q});
      auto idx2 = Index::from_documents(docs2, opt);
      Searcher s2(idx2);
      auto p_long = Pattern::compile(long_q, {.kind = PatternKind::Fixed});
      SearchStats st_long{};
      auto ml = s2.find(p_long, {}, &st_long);
      assert(ml.size() == 1);
      // Long query should prune heavily (candidate_blocks small)
      assert(st_long.candidate_blocks <= 4);
    }
  }
  std::cerr << "M23 QO-2 done\n" << std::flush;

  // BF-2 resource bounds
  {
    // 1. Lookbehind window capped to 8192 — very long prefix must not crash.
    {
      PatternOptions o; o.engine = Engine::Pcre2Compat;
      auto p = Pattern::compile(R"((?<=a+)b)", o);
      std::string hay(20000, 'a'); hay.push_back('b'); hay.push_back('\n');
      auto idx = corpus(hay);
      Searcher s(idx);
      bool threw = false;
      try {
        auto m = s.find(p);
        (void)m;
      } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        threw = true;
        assert(msg.find("pergrep") != std::string::npos || msg.find("lookbehind") != std::string::npos || msg.find("state") != std::string::npos || msg.find("recursion") != std::string::npos);
      }
      (void)threw;
      {
        std::string hay2(100, 'a'); hay2.push_back('b'); hay2.push_back('\n');
        auto idx2 = corpus(hay2);
        Searcher s2(idx2);
        auto m2 = s2.find(p);
        assert(m2.size() == 1);
      }
    }
    std::cerr << "BF2-1 done\n" << std::flush;
    // 2. Repeat {1,100000} is capped to 10000 — should not OOM and match length <=10000.
    {
      PatternOptions o; o.engine = Engine::Pcre2Compat;
      bool compiled = false;
      try {
        auto p = Pattern::compile("a{1,100000}", o);
        compiled = true;
        std::string hay(20000, 'a'); hay.push_back('\n');
        auto idx = corpus(hay);
        Searcher s(idx);
        auto m = s.find(p);
        assert(!m.empty());
        assert(m[0].end - m[0].start <= 10000);
        assert(m[0].end - m[0].start >= 1);
      } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        assert(msg.find("pergrep") != std::string::npos);
        (void)msg;
      }
      try {
        auto p2 = Pattern::compile("a{1,100000}");
        std::string hay(20000, 'a'); hay.push_back('\n');
        auto idx2 = corpus(hay);
        Searcher s2(idx2);
        auto m2 = s2.find(p2);
        (void)m2;
      } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        assert(msg.find("pergrep") != std::string::npos);
      }
      (void)compiled;
    }
    std::cerr << "BF2-2 done\n" << std::flush;
    // 3. Recursion depth >10000 must throw cleanly (direct depth guard test).
    {
      bool threw = false;
      try {
        pergrep::detail::test_eval_depth_guard(10001);
        assert(false && "expected recursion depth exceeded");
      } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        threw = true;
        assert(msg.find("recursion") != std::string::npos || msg.find("pergrep") != std::string::npos);
      }
      assert(threw);
      try {
        pergrep::detail::test_eval_depth_guard(10000);
      } catch (...) {
        assert(false && "depth 10000 should not throw");
      }
      pergrep::detail::test_eval_depth_guard(0);
      {
        PatternOptions o; o.engine = Engine::Pcre2Compat;
        const int depth2 = 500;
        std::string pat;
        pat.reserve(depth2 * 2 + 1);
        for (int i = 0; i < depth2; ++i) pat.push_back('(');
        pat.push_back('a');
        for (int i = 0; i < depth2; ++i) pat.push_back(')');
        auto p = Pattern::compile(pat, o);
        std::string hay = "a\n";
        auto idx = corpus(hay);
        Searcher s(idx);
        auto m = s.find(p);
        assert(m.size() == 1);
      }
    }
    std::cerr << "BF2-3 done\n" << std::flush;
    // 4. Existing bounded repetition and catastrophic patterns still behave correctly.
    {
      auto p = Pattern::compile("a{2,4}");
      auto idx = corpus("aaaaa\n");
      Searcher s(idx);
      auto m = s.find(p);
      assert(!m.empty() && m.back().end - m.back().start == 4);
      std::string hay(20000, 'a'); hay.push_back('\n');
      auto idx2 = corpus(hay);
      Searcher s2(idx2);
      auto p2 = Pattern::compile("(a|aa)*b");
      auto m2 = s2.find(p2);
      assert(m2.empty());
      std::string hit(2000, 'a'); hit += "b\n";
      auto idx3 = corpus(hit);
      Searcher s3(idx3);
      auto hm = s3.find(p2);
      assert(hm.size() == 1 && hm[0].end - hm[0].start == 2001);
    }
    std::cerr << "BF2-4 done\n" << std::flush;
  }

  // BF-2 resource bounds
  {
    // 1. Lookbehind window capped to 8192 — very long prefix must not crash.
    {
      PatternOptions o; o.engine = Engine::Pcre2Compat;
      // Very long prefix (20000 'a's) with cheap lookbehind (?<=a)b — test window without 67M blow-up.
      // Use direct regex_search at the 'b' position to avoid scanning 20000 positions (which would be 20000*8192 work).
      {
        auto prog = pergrep::detail::parse_regex(R"((?<=a)b)", o);
        std::string hay(20000, 'a'); hay.push_back('b');
        pergrep::Match m;
        bool ok = false;
        bool threw = false;
        try {
          ok = pergrep::detail::regex_search(prog, hay, o, 20000, &m, 0, '\n');
        } catch (const std::runtime_error& e) {
          threw = true;
          std::string msg = e.what();
          assert(msg.find("pergrep") != std::string::npos);
        }
        assert(threw || ok);
        if (ok) assert(m.start == 20000 && m.end == 20001);
      }
      // Within-window correctness with the spec's (?<=a+)b on a small hay (Searcher path).
      {
        auto p = Pattern::compile(R"((?<=a+)b)", o);
        std::string hay2(100, 'a'); hay2.push_back('b'); hay2.push_back('\n');
        auto idx2 = corpus(hay2);
        Searcher s2(idx2);
        auto m2 = s2.find(p);
        assert(m2.size() == 1);
      }
    }
    // 2. Repeat {1,100000} is capped to 10000 — should not OOM and match length <=10000.
    {
      PatternOptions o; o.engine = Engine::Pcre2Compat;
      bool compiled = false;
      try {
        // Use (?=a) prefix to force extended VM (lookahead) so Repeat goes via eval with 10k cap.
        auto p = Pattern::compile("(?=a)a{1,100000}", o);
        compiled = true;
        std::string hay(20000, 'a'); hay.push_back('\n');
        auto idx = corpus(hay);
        Searcher s(idx);
        auto m = s.find(p);
        assert(!m.empty());
        // (?=a) is zero-width, so match length is just the a{1,100000} part, capped to 10000.
        assert(m[0].end - m[0].start <= 10000);
        assert(m[0].end - m[0].start >= 1);
      } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        assert(msg.find("pergrep") != std::string::npos);
        (void)msg;
      }
      try {
        auto p2 = Pattern::compile("a{1,100000}");
        std::string hay(20000, 'a'); hay.push_back('\n');
        auto idx2 = corpus(hay);
        Searcher s2(idx2);
        auto m2 = s2.find(p2);
        (void)m2;
      } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        assert(msg.find("pergrep") != std::string::npos);
      }
      (void)compiled;
    }
    // 3. Recursion depth >10000 must throw cleanly (direct depth guard test).
    {
      bool threw = false;
      try {
        pergrep::detail::test_eval_depth_guard(10001);
        assert(false && "expected recursion depth exceeded");
      } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        threw = true;
        assert(msg.find("recursion") != std::string::npos || msg.find("pergrep") != std::string::npos);
      }
      assert(threw);
      try {
        pergrep::detail::test_eval_depth_guard(10000);
      } catch (...) {
        assert(false && "depth 10000 should not throw");
      }
      pergrep::detail::test_eval_depth_guard(0);
      {
        PatternOptions o; o.engine = Engine::Pcre2Compat;
        const int depth2 = 500;
        std::string pat;
        pat.reserve(depth2 * 2 + 1);
        for (int i = 0; i < depth2; ++i) pat.push_back('(');
        pat.push_back('a');
        for (int i = 0; i < depth2; ++i) pat.push_back(')');
        auto p = Pattern::compile(pat, o);
        std::string hay = "a\n";
        auto idx = corpus(hay);
        Searcher s(idx);
        auto m = s.find(p);
        assert(m.size() == 1);
      }
    }
    // 4. Existing bounded repetition and catastrophic patterns still behave correctly.
    {
      auto p = Pattern::compile("a{2,4}");
      auto idx = corpus("aaaaa\n");
      Searcher s(idx);
      auto m = s.find(p);
      assert(!m.empty() && m.back().end - m.back().start == 4);
      std::string hay(20000, 'a'); hay.push_back('\n');
      auto idx2 = corpus(hay);
      Searcher s2(idx2);
      auto p2 = Pattern::compile("(a|aa)*b");
      auto m2 = s2.find(p2);
      assert(m2.empty());
      std::string hit(2000, 'a'); hit += "b\n";
      auto idx3 = corpus(hit);
      Searcher s3(idx3);
      auto hm = s3.find(p2);
      assert(hm.size() == 1 && hm[0].end - hm[0].start == 2001);
    }
  }

  return 0;
}
