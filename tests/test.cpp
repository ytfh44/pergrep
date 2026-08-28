#include <pergrep/pergrep.hpp>
#include <cassert>
#include <iostream>
#include <stdexcept>
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
  std::cout<<"ok\n";
}
