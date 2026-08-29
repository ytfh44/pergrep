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
  // Rust-regex surface used by ripgrep: class shorthands inside classes,
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
  }
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
  // Property-based differential test suite (Indexed Search == Brute-force Search).
  {
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
    ref_opt.chunk_bytes = 10 * 1024 * 1024;
    ref_opt.chunk_overlap = 10 * 1024 * 1024;
    ref_opt.positional_block_bytes = 10 * 1024 * 1024;
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
  std::cout<<"ok\n";
}
