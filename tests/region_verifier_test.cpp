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
struct Case { std::vector<Document> docs; std::string expr; PatternOptions po{}; SearchOptions so{}; IndexOptions io{}; uint32_t seed=0; bool bounded=false; };
struct Result { bool ok=true; std::string stage; SearchStats stats{}; };
std::string esc(std::string_view s) { std::ostringstream o; o<<std::hex<<std::setfill('0'); for (unsigned char c:s) { if(c>=0x20&&c<0x7f&&c!='\\') o<<char(c); else if(c=='\\') o<<"\\\\"; else o<<"\\x"<<std::setw(2)<<unsigned(c); } return o.str(); }
std::string po_text(const PatternOptions& o) { std::ostringstream s; s<<"kind="<<(o.kind==PatternKind::Fixed?"fixed":"regex")<<",case="<<(int)o.case_mode<<",engine="<<(int)o.engine<<",word="<<o.word<<",line="<<o.line<<",multiline="<<o.multiline<<",dotall="<<o.dotall<<",unicode="<<o.unicode<<",crlf="<<o.crlf; return s.str(); }
std::string so_text(const SearchOptions& o) { std::ostringstream s; s<<"overlapping="<<o.overlapping<<",invert="<<o.invert_match<<",with_files="<<o.files_with_matches<<",without_files="<<o.files_without_match<<",include_binary="<<o.include_binary<<",max_matches="<<o.max_matches<<",separator="<<unsigned(o.record_separator); return s.str(); }
std::string stat_text(const SearchStats& s) { std::ostringstream o; o<<"verifier="<<s.verifier<<",physical_operator="<<s.physical_operator<<",fallback="<<s.verifier_fallback<<",fallback_reason="<<s.qgram_fallback_reason<<",plan_key_hash="<<s.plan_key_hash<<",semantic_mode="<<s.semantic_mode<<",verified_bytes="<<s.verified_bytes; return o.str(); }
bool same(const Capture&a,const Capture&b){return a.start==b.start&&a.end==b.end&&a.matched==b.matched&&a.name==b.name;}
bool same(const std::vector<Match>&a,const std::vector<Match>&b){if(a.size()!=b.size())return false;for(size_t i=0;i<a.size();++i){if(a[i].file_id!=b[i].file_id||a[i].start!=b[i].start||a[i].end!=b[i].end||a[i].captures.size()!=b[i].captures.size())return false;for(size_t g=0;g<a[i].captures.size();++g)if(!same(a[i].captures[g],b[i].captures[g]))return false;}return true;}
std::vector<Match> full(const Case& c,const Pattern&p){auto program=detail::parse_regex(p.expression(),p.options());std::vector<Match> out;for(uint32_t f=0;f<c.docs.size();++f){auto ms=detail::regex_find_all(program,c.docs[f].content,p.options(),c.so.overlapping,f,0,0,c.so.record_separator);for(auto&m:ms){out.push_back(std::move(m));if(c.so.max_matches&&out.size()>=c.so.max_matches)return out;}}return out;}
struct Bounds{uint64_t begin=0,end=0;};
Bounds record(std::string_view s,uint64_t at,unsigned char sep,bool crlf){auto b=s.rfind(char(sep),at?size_t(at-1):0);uint64_t begin=b==std::string_view::npos?0:b+1;auto e=s.find(char(sep),size_t(at));if(e==std::string_view::npos)e=s.size();if(crlf&&e>begin&&s[e-1]=='\r')--e;return{begin,e};}
std::vector<Match> region(std::string_view source,const Pattern&p,const Case&c,const Match&m){auto q=detail::parse_regex(p.expression(),p.options());auto b=record(source,m.start,c.so.record_separator,p.options().crlf);detail::VerifierContext x; x.source=source;x.source_end=source.size();x.record_end=b.end;x.record_begin=b.begin;x.candidate_begin=m.start;x.candidate_end=std::min(b.end+uint64_t(1),m.start+uint64_t(1));x.separator=c.so.record_separator;x.crlf=p.options().crlf;x.left_context_available=m.start>b.begin;x.right_context_available=m.end<b.end;bool left=p.expression().find('^')!=std::string::npos||p.expression().find("\\A")!=std::string::npos;bool right=p.expression().find('$')!=std::string::npos||p.expression().find("\\z")!=std::string::npos;x.region_begin=left?b.begin:(m.start>b.begin+24?m.start-24:b.begin);x.region_end=right?b.end:std::min(b.end,std::max(m.end+uint64_t(24),m.start+uint64_t(1)));if(x.region_end<=x.region_begin)x.region_end=std::min(b.end,x.region_begin+uint64_t(1));x.bounded_region=true;return detail::regex_find_all(q,x,p.options(),c.so.overlapping,m.file_id,0);}
Result check(const Case&c,bool direct){Result r;try{auto idx=Index::from_documents(c.docs,c.io);auto p=Pattern::compile(c.expr,c.po);auto got=Searcher(idx).find(p,c.so,&r.stats);auto want=full(c,p);if(!same(got,want)){r.ok=false;r.stage="indexed-vs-full-reference";return r;}if(direct)for(auto&m:want){auto got_region=region(c.docs[m.file_id].content,p,c,m);if(got_region.size()!=1||!same(got_region,{m})){r.ok=false;r.stage="region-vs-full-reference";return r;}}}catch(...){r.ok=false;r.stage="compile-or-execution-error";}return r;}
Case minimize(Case c){auto fails=[](const Case&x){return !check(x,true).ok;};for(size_t di=0;di<c.docs.size();++di)for(size_t w=c.docs[di].content.size()/2;w;w/=2){bool again=true;while(again){again=false;for(size_t at=0;at+w<=c.docs[di].content.size();++at){auto x=c;x.docs[di].content.erase(at,w);if(fails(x)){c=std::move(x);again=true;break;}}}}return c;}
void report(const Case&c,const Result&r){std::cerr<<"region-verifier property failure stage="<<r.stage<<" seed="<<c.seed<<"\npattern="<<esc(c.expr)<<"\npattern_options="<<po_text(c.po)<<"\nsearch_options="<<so_text(c.so)<<"\nchunk_bytes="<<c.io.chunk_bytes<<" chunk_overlap="<<c.io.chunk_overlap<<" positional_block_bytes="<<c.io.positional_block_bytes<<"\nstats="<<stat_text(r.stats)<<"\n";for(auto&d:c.docs)std::cerr<<"document path="<<esc(d.path)<<" content="<<esc(d.content)<<"\n";try{auto i=Index::from_documents(c.docs,c.io);auto p=Pattern::compile(c.expr,c.po);auto k=make_plan_key(p,c.so,i);std::cerr<<"plan_key_hash="<<k.hash()<<" semantic_mode="<<semantic_mode_key(k)<<"\n";if(auto*d=static_cast<const detail::IndexData*>(i.debug_index_data()))for(auto&x:detail::estimate_all_candidate_plans(k,*d))std::cerr<<"plan_candidate name="<<x.name<<" verifier="<<to_string(x.verifier)<<" predicted_cost="<<x.predicted_cost<<" chosen="<<x.chosen<<"\n";}catch(const std::exception&e){std::cerr<<"plan_error="<<e.what()<<"\n";}}
std::string noise(std::mt19937&r,size_t n){static constexpr std::string_view a="xyz0123 _-";std::string s;s.reserve(n);for(size_t i=0;i<n;++i)s+=a[r()%a.size()];return s;}
Case make(uint32_t seed){
    std::mt19937 r(seed); Case c; c.seed=seed; c.io.chunk_bytes=64; c.io.chunk_overlap=16; c.io.positional_block_bytes=16;
    unsigned k=seed%12; size_t at=61+(seed*17u)%180; std::string b=noise(r,at);
    switch(k){
    case 0: c.expr=R"(pre(foo|bar)([0-9]{1,3})post)"; b+="prefoo12post"; break;
    case 1: c.expr=R"(pre(a{1,3}?b)post)"; b+="preaaabpost"; c.bounded=true; break;
    case 2: c.expr=R"(foo.{0,12}bar)"; b+="foo"+noise(r,seed%8)+"bar"; c.bounded=true; break;
    case 3: c.expr=R"((foo|bar){1,2})"; b+="foobar"; break;
    case 4: c.expr=R"((?<word>[A-Z]{1,3})foo)"; b+="ABCfoo"; break;
    case 5: c.expr=R"(^foo[0-9]{1,3}bar$)"; c.po.multiline=true; b.push_back(char(10)); b+="foo12bar"; b.push_back(char(10)); c.bounded=true; break;
    case 6: c.expr=R"((?<=pre)foo(?=bar))"; c.po.engine=Engine::Pcre2Compat; b+="prefoobar"; break;
    case 7: c.expr=R"((?!bad)needle)"; c.po.engine=Engine::Pcre2Compat; b+="needle badneedle"; break;
    case 8: c.expr=R"((é|世界)[A-Z]{1,2})"; b+="éAB 世界CD"; b.insert(std::min<size_t>(63,b.size()),std::string(1,char(0xE4))); break;
    case 9: c.expr=R"(foo[0-9]{1,3}bar)"; c.po.word=true; b+=" foo12bar "; c.bounded=true; break;
    case 10: c.expr=R"(a.a)"; c.so.overlapping=true; b+="abaaba"; break;
    default: c.expr=R"(^needle$)"; c.po.multiline=true; c.so.record_separator=static_cast<unsigned char>(0); c.so.include_binary=true; b=noise(r,at); b.push_back(char(0)); b+="needle"; b.push_back(char(0)); b+="tail"; break;
    }
    c.docs=k==11?std::vector<Document>{{"binary.bin",b}}:std::vector<Document>{{"a.txt",b},{"b.txt",noise(r,37)+b.substr(at/2)}}; return c;
} // namespace
}
int main(){size_t bounded=0;for(uint32_t seed=0x36;seed<0x36+96;++seed){auto c=make(seed);auto r=check(c,true);if(!r.ok){auto m=minimize(std::move(c));auto mr=check(m,true);report(m,mr);return 1;}if(r.stats.physical_operator=="RegexBoundedRegion")++bounded;if(c.bounded&&r.stats.physical_operator!="RegexBoundedRegion"){report(c,r);return 1;}}if(!bounded){std::cerr<<"region-verifier property failure stage=no-bounded-region-cases"<<char(10);return 1;}std::cout<<"region-verifier properties: 96 deterministic cases, "<<bounded<<" bounded-region executions"<<char(10);return 0;}
