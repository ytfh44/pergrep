#include "pergrep/pergrep.hpp"
#include "default_types.hpp"
#include "platform.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <functional>
#include <cerrno>
#include <cstdio>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <sys/stat.h>
#include <archive.h>
#include <archive_entry.h>
#include <unicode/uchar.h>
#include <unicode/utf8.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace fs=std::filesystem;
using namespace pergrep;

// Construct a filesystem path from a UTF-8 narrow string. On Windows the
// narrow-to-wide conversion is ambiguous (ANSI code page by default), so use
// the explicit char8_t constructor; on Unix native narrow paths are already
// UTF-8 in practice.
inline fs::path to_path(std::string_view s) {
#ifdef _WIN32
    return fs::path(std::u8string(s.begin(), s.end()));
#else
    return fs::path(std::string(s));
#endif
}

namespace {
struct Args {
    bool stdin_haystack=false;
    std::vector<std::string> patterns,pattern_files,paths,globs,iglobs,types,type_not,type_add,type_clear,ignore_files,pre_globs,color_specs;
    PatternOptions popt;SearchOptions sopt;
    int after=0,before=0;bool after_explicit=false,before_explicit=false,context_seen=false;bool passthru=false,binary=false,text=false,hidden=false,follow=false,no_ignore=false,no_ignore_vcs=false,no_ignore_dot=false,no_ignore_parent=false,no_ignore_global=false,no_ignore_exclude=false,no_ignore_files=false,no_require_git=false,no_messages=false,no_ignore_messages=false,one_file_system=false;
    bool line_number=false,column=false,byte_offset=false,with_filename=false,no_filename=false,heading=false,no_heading=false,only_matching=false,count=false,count_matches=false,files_mode=false,files_with=false,files_without=false,include_zero=false,quiet=false,vimgrep=false,json=false,stats=false,trim=false,null_out=false,null_data=false,pretty=false,block_buffered=false,line_buffered=false,stop_on_nonmatch=false,max_columns_preview=false,glob_ci=false,ignore_file_ci=false,search_zip=false;
    std::string color="auto",context_sep="--",field_match_sep=":",field_context_sep="-",path_sep,replacement,encoding="auto",engine="default",sort,pre,hyperlink_format,hostname_bin,generate;
    uint64_t max_count=0,max_filesize=0;int max_depth=-1,threads=0,max_columns=0;bool help=false,short_help=false,version=false,type_list=false,pcre2_version=false,debug=false,trace=false,max_count_set=false,context_sep_disabled=false,no_config=false;int unrestricted=0;
};

[[noreturn]] void die(std::string s,int code=2){std::cerr<<"pergrep: "<<s<<"\n";std::exit(code);}
uint64_t num(std::string_view s){
    uint64_t mult=1;
    if(!s.empty()){
        char c=s.back();
        if(c=='K'||c=='k'){mult=1024;s.remove_suffix(1);}
        else if(c=='M'||c=='m'){mult=1024ull*1024;s.remove_suffix(1);}
        else if(c=='G'||c=='g'){mult=1024ull*1024*1024;s.remove_suffix(1);}
    }
    uint64_t x=0;
    auto [p,e]=std::from_chars(s.data(),s.data()+s.size(),x);
    if(e!=std::errc()||p!=s.data()+s.size())die("invalid numeric value");
    if(mult>1&&x>std::numeric_limits<uint64_t>::max()/mult)die("invalid numeric value");
    return x*mult;
}
int num_int(std::string_view s){
    uint64_t v=num(s);
    if(v>static_cast<uint64_t>(std::numeric_limits<int>::max()))die("invalid numeric value");
    return static_cast<int>(v);
}

const std::unordered_map<std::string,std::vector<std::string>>& type_map(){ return pergrep_cli::default_types(); }

using TypeMap = std::unordered_map<std::string,std::vector<std::string>>;

std::vector<std::string> expand_braces(std::string pat) {
    auto l=pat.find('{'); if(l==std::string::npos)return{pat};
    auto r=pat.find('}',l+1); if(r==std::string::npos)return{pat};
    std::vector<std::string> out; std::string mid=pat.substr(l+1,r-l-1); std::size_t b=0;
    for(;;){auto c=mid.find(',',b);auto x=mid.substr(b,c==std::string::npos?std::string::npos:c-b);for(auto&tail:expand_braces(pat.substr(r+1)))out.push_back(pat.substr(0,l)+x+tail);if(c==std::string::npos)break;b=c+1;}
    return out;
}

TypeMap effective_type_map(const Args& a) {
    TypeMap m=type_map();
    for(const auto& name:a.type_clear)m[name].clear();
    for(const auto& spec:a.type_add){
        auto c=spec.find(':'); if(c==std::string::npos||c==0)die("invalid --type-add specification: "+spec);
        auto name=spec.substr(0,c), rule=spec.substr(c+1);
        if(rule.rfind("include:",0)==0){
            std::string rest=rule.substr(8);std::size_t b=0;
            for(;;){auto q=rest.find(',',b);auto src=rest.substr(b,q==std::string::npos?std::string::npos:q-b);auto it=m.find(src);if(it==m.end())die("unknown file type in include: "+src);m[name].insert(m[name].end(),it->second.begin(),it->second.end());if(q==std::string::npos)break;b=q+1;}
        }else for(auto&g:expand_braces(rule))m[name].push_back(g);
    }
    return m;
}


bool globmatch(std::string pat,std::string path,bool ci){
    if(ci){
        // Case-insensitive glob should fold Unicode, not just ASCII.
        // Use ICU case folding per code point so 'É' matches 'é'.
        auto fold_utf8 = [](std::string s){
            std::string out; out.reserve(s.size());
            std::size_t i = 0;
            while (i < s.size()) {
                int32_t pos = static_cast<int32_t>(i);
                int32_t n = static_cast<int32_t>(s.size());
                UChar32 cp; U8_NEXT(s.data(), pos, n, cp);
                if (cp < 0) { out.push_back(s[i]); ++i; continue; }
                UChar32 folded = u_foldCase(cp, U_FOLD_CASE_DEFAULT);
                char buf[U8_MAX_LENGTH]; int32_t bi = 0;
                U8_APPEND_UNSAFE(buf, bi, folded);
                out.append(buf, bi);
                i = static_cast<std::size_t>(pos);
            }
            return out;
        };
        pat = fold_utf8(pat);
        path = fold_utf8(path);
    }
    return pergrep_cli::platform::fnmatch(pat,path)||pergrep_cli::platform::fnmatch(pat,to_path(path).filename().string());
}

std::string need(int&k,int argc,char**argv,std::string_view opt,std::optional<std::string>attached={}){if(attached)return*attached;if(++k>=argc)die("option requires a value: "+std::string(opt));return argv[k];}
void parse_long(Args&a,std::string arg,int&k,int argc,char**argv){auto eq=arg.find('=');std::string name=arg.substr(2,eq==std::string::npos?std::string::npos:eq-2);std::optional<std::string>val;if(eq!=std::string::npos)val=arg.substr(eq+1);auto v=[&](){return need(k,argc,argv,"--"+name,val);};
#define L(n,code) if(name==n){code;return;}
    L("after-context",a.after=num_int(v());a.after_explicit=true;a.context_seen=true) L("before-context",a.before=num_int(v());a.before_explicit=true;a.context_seen=true) L("context",{int z=num_int(v());if(!a.after_explicit)a.after=z;if(!a.before_explicit)a.before=z;a.context_seen=true;})
    L("binary",a.binary=true) L("no-binary",a.binary=false) L("block-buffered",a.block_buffered=true) L("no-block-buffered",a.block_buffered=false) L("byte-offset",a.byte_offset=true) L("no-byte-offset",a.byte_offset=false)
    L("case-sensitive",a.popt.case_mode=CaseMode::Sensitive) L("color",a.color=v()) L("colors",a.color_specs.push_back(v())) L("column",a.column=true;a.line_number=true) L("no-column",a.column=false)
    L("context-separator",a.context_sep=v();a.context_sep_disabled=false) L("no-context-separator",a.context_sep_disabled=true) L("count",if(!a.files_mode){a.count=true;a.count_matches=false;a.files_with=false;a.files_without=false;}) L("count-matches",if(!a.files_mode){a.count_matches=true;a.count=false;a.files_with=false;a.files_without=false;}) L("crlf",a.popt.crlf=true;a.null_data=false) L("no-crlf",a.popt.crlf=false)
    L("debug",a.debug=true) L("dfa-size-limit",(void)v()) L("encoding",a.encoding=v()) L("engine",{a.engine=v();if(a.engine=="default")a.popt.engine=Engine::Default;else if(a.engine=="pcre2")a.popt.engine=Engine::Pcre2Compat;else if(a.engine=="auto")a.popt.engine=Engine::Auto;else die("unrecognized engine: "+a.engine);})
    L("field-context-separator",a.field_context_sep=v()) L("field-match-separator",a.field_match_sep=v()) L("files",a.files_mode=true) L("files-with-matches",if(!a.files_mode){a.files_with=true;a.files_without=false;a.count=false;a.count_matches=false;}) L("files-without-match",if(!a.files_mode){a.files_without=true;a.files_with=false;a.count=false;a.count_matches=false;})
    L("fixed-strings",a.popt.kind=PatternKind::Fixed) L("no-fixed-strings",a.popt.kind=PatternKind::Regex) L("follow",a.follow=true) L("no-follow",a.follow=false) L("generate",a.generate=v()) L("glob",a.globs.push_back(v())) L("glob-case-insensitive",a.glob_ci=true) L("no-glob-case-insensitive",a.glob_ci=false)
    L("heading",a.heading=true;a.no_heading=false) L("no-heading",a.no_heading=true;a.heading=false) L("help",a.help=true) L("hidden",a.hidden=true) L("no-hidden",a.hidden=false) L("hostname-bin",a.hostname_bin=v()) L("hyperlink-format",a.hyperlink_format=v()) L("iglob",a.iglobs.push_back(v()))
    L("ignore-case",a.popt.case_mode=CaseMode::Insensitive) L("no-ignore-case",a.popt.case_mode=CaseMode::Sensitive) L("ignore-file",a.ignore_files.push_back(v())) L("ignore-file-case-insensitive",a.ignore_file_ci=true) L("no-ignore-file-case-insensitive",a.ignore_file_ci=false) L("include-zero",a.include_zero=true) L("no-include-zero",a.include_zero=false) L("invert-match",a.sopt.invert_match=true) L("no-invert-match",a.sopt.invert_match=false)
    L("json",a.json=true) L("no-json",a.json=false) L("line-buffered",a.line_buffered=true) L("no-line-buffered",a.line_buffered=false) L("line-number",a.line_number=true) L("no-line-number",a.line_number=false) L("line-regexp",a.popt.line=true;a.popt.word=false) L("no-line-regexp",a.popt.line=false)
    L("max-columns",a.max_columns=num_int(v())) L("max-columns-preview",a.max_columns_preview=true) L("no-max-columns-preview",a.max_columns_preview=false) L("max-count",a.max_count=num(v());a.max_count_set=true) L("max-depth",a.max_depth=num_int(v())) L("max-filesize",a.max_filesize=num(v()))
    L("mmap",) L("no-mmap",) L("multiline",a.popt.multiline=true;a.stop_on_nonmatch=false) L("no-multiline",a.popt.multiline=false) L("multiline-dotall",a.popt.dotall=true) L("no-multiline-dotall",a.popt.dotall=false) L("no-config",a.no_config=true)
    L("no-ignore",a.no_ignore=true) L("ignore",a.no_ignore=false) L("no-ignore-dot",a.no_ignore_dot=true) L("ignore-dot",a.no_ignore_dot=false) L("no-ignore-exclude",a.no_ignore_exclude=true) L("ignore-exclude",a.no_ignore_exclude=false) L("no-ignore-files",a.no_ignore_files=true) L("ignore-files",a.no_ignore_files=false) L("no-ignore-global",a.no_ignore_global=true) L("ignore-global",a.no_ignore_global=false) L("no-ignore-messages",a.no_ignore_messages=true) L("ignore-messages",a.no_ignore_messages=false) L("no-ignore-parent",a.no_ignore_parent=true) L("ignore-parent",a.no_ignore_parent=false) L("no-ignore-vcs",a.no_ignore_vcs=true) L("ignore-vcs",a.no_ignore_vcs=false)
    L("no-messages",a.no_messages=true) L("messages",a.no_messages=false) L("no-require-git",a.no_require_git=true) L("require-git",a.no_require_git=false) L("no-unicode",a.popt.unicode=false) L("unicode",a.popt.unicode=true) L("null",a.null_out=true) L("no-null",a.null_out=false) L("null-data",a.null_data=true;a.text=true;a.popt.crlf=false) L("no-null-data",a.null_data=false) L("one-file-system",a.one_file_system=true) L("no-one-file-system",a.one_file_system=false)
    L("only-matching",a.only_matching=true) L("no-only-matching",a.only_matching=false) L("path-separator",a.path_sep=v()) L("passthru",a.passthru=true;a.context_seen=false) L("no-passthru",a.passthru=false) L("pcre2",a.popt.engine=Engine::Pcre2Compat;a.engine="pcre2") L("no-pcre2",a.popt.engine=Engine::Default;a.engine="default") L("pcre2-version",a.pcre2_version=true)
    L("pre",a.pre=v()) L("pre-glob",a.pre_globs.push_back(v())) L("pretty",a.pretty=true;a.color="always";a.heading=true;a.line_number=true) L("quiet",a.quiet=true) L("no-quiet",a.quiet=false) L("regex-size-limit",(void)v()) L("regexp",a.patterns.push_back(v())) L("file",a.pattern_files.push_back(v()))
    L("replace",a.replacement=v()) L("search-zip",a.search_zip=true) L("no-search-zip",a.search_zip=false) L("smart-case",a.popt.case_mode=CaseMode::Smart) L("no-smart-case",a.popt.case_mode=CaseMode::Sensitive)
    L("sort",a.sort=v()) L("sortr",a.sort="reverse:"+v()) L("stats",a.stats=true) L("no-stats",a.stats=false) L("stop-on-nonmatch",a.stop_on_nonmatch=true;a.popt.multiline=false) L("no-stop-on-nonmatch",a.stop_on_nonmatch=false) L("text",a.text=true) L("no-text",a.text=false) L("threads",a.threads=num_int(v())) L("trace",a.trace=true) L("trim",a.trim=true) L("no-trim",a.trim=false)
    L("type",a.types.push_back(v())) L("type-not",a.type_not.push_back(v())) L("type-add",a.type_add.push_back(v())) L("type-clear",a.type_clear.push_back(v())) L("type-list",a.type_list=true) L("unrestricted",++a.unrestricted;a.no_ignore=true;if(a.unrestricted>=2)a.hidden=true;if(a.unrestricted>=3)a.binary=true)
    L("version",a.version=true) L("vimgrep",a.vimgrep=true;a.line_number=true;a.column=true) L("with-filename",a.with_filename=true;a.no_filename=false) L("no-filename",a.no_filename=true;a.with_filename=false) L("word-regexp",a.popt.word=true;a.popt.line=false) L("no-word-regexp",a.popt.word=false)
    L("auto-hybrid-regex",a.popt.engine=Engine::Auto;a.engine="auto") L("no-auto-hybrid-regex",a.popt.engine=Engine::Default;a.engine="default") L("no-pcre2-unicode",a.popt.unicode=false) L("pcre2-unicode",a.popt.unicode=true) L("sort-files",a.sort="path") L("no-sort-files",a.sort.clear())
    die("unrecognized flag --"+name);
#undef L
}
void parse_short(Args&a,std::string arg,int&k,int argc,char**argv){for(size_t j=1;j<arg.size();++j){char c=arg[j];auto attached=[&]()->std::optional<std::string>{if(j+1<arg.size()){auto s=arg.substr(j+1);j=arg.size();return s;}return{};};switch(c){
case'A':a.after=num_int(need(k,argc,argv,"-A",attached()));a.after_explicit=true;a.context_seen=true;return;case'B':a.before=num_int(need(k,argc,argv,"-B",attached()));a.before_explicit=true;a.context_seen=true;return;case'C':{int z=num_int(need(k,argc,argv,"-C",attached()));if(!a.after_explicit)a.after=z;if(!a.before_explicit)a.before=z;a.context_seen=true;return;}
case'a':a.text=true;break;case'b':a.byte_offset=true;break;case'c':if(!a.files_mode){a.count=true;a.count_matches=false;a.files_with=false;a.files_without=false;}break;case'0':a.null_out=true;break;case'd':a.max_depth=num_int(need(k,argc,argv,"-d",attached()));return;case'E':a.encoding=need(k,argc,argv,"-E",attached());return;case'e':a.patterns.push_back(need(k,argc,argv,"-e",attached()));return;case'f':a.pattern_files.push_back(need(k,argc,argv,"-f",attached()));return;case'F':a.popt.kind=PatternKind::Fixed;break;case'g':a.globs.push_back(need(k,argc,argv,"-g",attached()));return;case'h':a.short_help=true;break;case'H':a.with_filename=true;a.no_filename=false;break;case'I':a.no_filename=true;a.with_filename=false;break;case'i':a.popt.case_mode=CaseMode::Insensitive;break;case'j':a.threads=num_int(need(k,argc,argv,"-j",attached()));return;case'l':if(!a.files_mode){a.files_with=true;a.files_without=false;a.count=false;a.count_matches=false;}break;case'L':a.follow=true;break;case'm':a.max_count=num(need(k,argc,argv,"-m",attached()));a.max_count_set=true;return;case'M':a.max_columns=num_int(need(k,argc,argv,"-M",attached()));return;case'n':a.line_number=true;break;case'N':a.line_number=false;break;case'o':a.only_matching=true;break;case'p':a.pretty=true;a.color="always";a.heading=true;a.line_number=true;break;case'P':a.popt.engine=Engine::Pcre2Compat;a.engine="pcre2";break;case'q':a.quiet=true;break;case'r':a.replacement=need(k,argc,argv,"-r",attached());return;case's':a.popt.case_mode=CaseMode::Sensitive;break;case'S':a.popt.case_mode=CaseMode::Smart;break;case't':a.types.push_back(need(k,argc,argv,"-t",attached()));return;case'T':a.type_not.push_back(need(k,argc,argv,"-T",attached()));return;case'u':++a.unrestricted;a.no_ignore=true;if(a.unrestricted>=2)a.hidden=true;if(a.unrestricted>=3)a.binary=true;break;case'U':a.popt.multiline=true;a.stop_on_nonmatch=false;break;case'v':a.sopt.invert_match=true;break;case'V':a.version=true;break;case'w':a.popt.word=true;a.popt.line=false;break;case'x':a.popt.line=true;a.popt.word=false;break;case'z':a.search_zip=true;break;default:die(std::string("unrecognized flag -")+c);}}
}

Args parse(int argc,char**argv){Args a;bool positional=false;for(int k=1;k<argc;++k){std::string x=argv[k];if(!positional&&x=="--"){positional=true;continue;}if(!positional&&x.rfind("--",0)==0)parse_long(a,x,k,argc,argv);else if(!positional&&x.size()>1&&x[0]=='-')parse_short(a,x,k,argc,argv);else{if(a.files_mode)a.paths.push_back(x);else if(a.patterns.empty()&&a.pattern_files.empty())a.patterns.push_back(x);else a.paths.push_back(x);}}for(auto&pf:a.pattern_files){std::istream*in=nullptr;std::ifstream f;if(pf=="-")in=&std::cin;else{f.open(to_path(pf));if(!f)die("cannot open pattern file: "+pf);in=&f;}std::string s;while(std::getline(*in,s))a.patterns.push_back(s);}if(a.context_seen)a.passthru=false;if(a.only_matching&&a.count){a.count=false;a.count_matches=true;}if(a.paths.empty()){if(!pergrep_cli::platform::isatty_stdin()){a.paths.push_back("-");a.stdin_haystack=true;}else a.paths.push_back(".");}a.sopt.include_binary=a.text||a.binary||a.unrestricted>=3;if(a.files_with){a.sopt.files_with_matches=true;}if(a.files_without){a.sopt.files_without_match=true;}return a;}

Args parse_with_config(int argc, char** argv) {
    bool disable=false;
    for(int i=1;i<argc;++i) if(std::string_view(argv[i])=="--no-config") { disable=true; break; }
    const char* cfg=std::getenv("RIPGREP_CONFIG_PATH");
    if(disable || !cfg || !*cfg) return parse(argc,argv);
    std::ifstream f(cfg);
    if(!f) die(std::string("could not read RIPGREP_CONFIG_PATH: ")+cfg);
    std::vector<std::string> storage; storage.emplace_back(argv[0]);
    std::string line;
    while(std::getline(f,line)) {
        auto b=line.find_first_not_of(" \t\r");
        if(b==std::string::npos || line[b]=='#') continue;
        auto e=line.find_last_not_of(" \t\r");
        storage.push_back(line.substr(b,e-b+1));
    }
    for(int i=1;i<argc;++i) storage.emplace_back(argv[i]);
    std::vector<char*> av; av.reserve(storage.size());
    for(auto& x:storage) av.push_back(x.data());
    return parse((int)av.size(),av.data());
}

std::string help(){return R"(pergrep 0.1.0
A ripgrep-compatible CLI backed by libpergrep's compiled corpus/query verifier.

USAGE:
    pergrep [OPTIONS] PATTERN [PATH ...]
    pergrep [OPTIONS] -e PATTERN ... [PATH ...]
    pergrep [OPTIONS] -f PATTERNFILE ... [PATH ...]

The command-line surface follows ripgrep 15.2.0. Run `pergrep --help` for this
summary. Search semantics are implemented internally; no grep-like executable
or third-party search engine is invoked.

Core options: -e/--regexp, -f/--file, -F/--fixed-strings, -i/--ignore-case,
-S/--smart-case, -w/--word-regexp, -x/--line-regexp, -v/--invert-match,
-U/--multiline, -P/--pcre2, -g/--glob, -t/--type, -T/--type-not,
-A/-B/-C context, -n/--line-number, --column, -b/--byte-offset,
-o/--only-matching, -c/--count, --count-matches, -l/--files-with-matches,
--files-without-match, --files, --json, --vimgrep, --stats, --sort/--sortr,
-a/--text, --binary, --hidden, -L/--follow, -u/--unrestricted.

Compression is decoded internally with libarchive; matching is never delegated.
--pre executes only the user-specified transformer and searches its stdout.
)";}


std::string decode_utf16_bytes(std::string_view in, bool little, bool bom_allowed=true) {
    std::size_t i=0; if(bom_allowed && in.size()>=2){unsigned a=(unsigned char)in[0],b=(unsigned char)in[1];if((a==0xff&&b==0xfe)||(a==0xfe&&b==0xff))i=2;}
    std::string out; out.reserve(in.size());
    auto unit=[&](std::size_t p)->std::uint16_t{unsigned a=(unsigned char)in[p],b=(unsigned char)in[p+1];return little?std::uint16_t(a|(b<<8)):std::uint16_t((a<<8)|b);};
    auto emit=[&](std::uint32_t cp){if(cp<=0x7f)out.push_back(char(cp));else if(cp<=0x7ff){out.push_back(char(0xc0|(cp>>6)));out.push_back(char(0x80|(cp&63)));}else if(cp<=0xffff){out.push_back(char(0xe0|(cp>>12)));out.push_back(char(0x80|((cp>>6)&63)));out.push_back(char(0x80|(cp&63)));}else{out.push_back(char(0xf0|(cp>>18)));out.push_back(char(0x80|((cp>>12)&63)));out.push_back(char(0x80|((cp>>6)&63)));out.push_back(char(0x80|(cp&63)));}};
    while(i+1<in.size()){std::uint16_t w=unit(i);i+=2;if(w>=0xd800&&w<=0xdbff&&i+1<in.size()){std::uint16_t w2=unit(i);if(w2>=0xdc00&&w2<=0xdfff){i+=2;emit(0x10000+((w-0xd800)<<10)+(w2-0xdc00));continue;}}if(w>=0xdc00&&w<=0xdfff)emit(0xfffd);else emit(w);}return out;
}
#ifdef _WIN32
std::string iconv_to_utf8(std::string_view in, std::string enc) {
    // Windows has no iconv; fall back to the system ANSI code page.
    std::string out;
    if (!pergrep_cli::platform::ansi_to_utf8(in, out))
        die("unknown or unsupported encoding: "+enc);
    if(out.rfind("\xEF\xBB\xBF",0)==0)out.erase(0,3);
    return out;
}
#else
std::string iconv_to_utf8(std::string_view in, std::string enc) {
    iconv_t cd=iconv_open("UTF-8",enc.c_str());if(cd==(iconv_t)-1)die("unknown or unsupported encoding: "+enc);
    std::string out(std::max<std::size_t>(64,in.size()*4+16),'\0');char* pin=const_cast<char*>(in.data());std::size_t left=in.size(),used=0;
    while(left){char* pout=out.data()+used;std::size_t avail=out.size()-used;auto r=iconv(cd,&pin,&left,&pout,&avail);used=out.size()-avail;if(r!=(size_t)-1)break;if(errno==E2BIG){out.resize(out.size()*2);continue;}iconv_close(cd);die("input is not valid for encoding "+enc);}iconv_close(cd);out.resize(used);if(out.rfind("\xEF\xBB\xBF",0)==0)out.erase(0,3);return out;
}
#endif
std::string decode_input(std::string_view in, std::string enc) {
    auto lower=enc;std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return char(std::tolower(c));});
    if(lower=="auto") {if(in.size()>=2&&(unsigned char)in[0]==0xff&&(unsigned char)in[1]==0xfe)return decode_utf16_bytes(in,true);if(in.size()>=2&&(unsigned char)in[0]==0xfe&&(unsigned char)in[1]==0xff)return decode_utf16_bytes(in,false);return std::string(in);}
    if(lower=="none"||lower=="utf-8"||lower=="utf8")return std::string(in);
    if(lower=="utf-16le"||lower=="utf16le")return decode_utf16_bytes(in,true);
    if(lower=="utf-16be"||lower=="utf16be")return decode_utf16_bytes(in,false);
    if(lower=="utf-16"||lower=="utf16"){if(in.size()>=2&&(unsigned char)in[0]==0xfe&&(unsigned char)in[1]==0xff)return decode_utf16_bytes(in,false);return decode_utf16_bytes(in,true);}
    return iconv_to_utf8(in,enc);
}


std::vector<std::string> split_command_words(std::string_view command){
    std::vector<std::string> out;std::string cur;char quote=0;bool escape=false,started=false;
    for(char c:command){
        if(escape){cur.push_back(c);escape=false;started=true;continue;}
        if(quote=='\''){if(c=='\'')quote=0;else cur.push_back(c);started=true;continue;}
        if(quote=='"'){if(c=='"')quote=0;else if(c=='\\')escape=true;else cur.push_back(c);started=true;continue;}
        if(c=='\\'){escape=true;started=true;continue;}
        if(c=='\''||c=='"'){quote=c;started=true;continue;}
        if(std::isspace(static_cast<unsigned char>(c))){if(started){out.push_back(cur);cur.clear();started=false;}continue;}
        cur.push_back(c);started=true;
    }
    if(escape||quote)die("unterminated quote/escape in --pre command");
    if(started)out.push_back(cur);
    if(out.empty())die("--pre command is empty");
    return out;
}
#ifdef _WIN32
std::string run_preprocessor(const std::string& cmd,const fs::path& path){
    auto words=split_command_words(cmd);words.push_back(pergrep_cli::platform::path_to_utf8(path));
    std::string out;
    if(!pergrep_cli::platform::run_capture(words,std::string_view{},out))
        die("--pre command failed for "+pergrep_cli::platform::path_to_utf8(path));
    return out;
}
#else
std::string run_preprocessor(const std::string& cmd,const fs::path& path){
    auto words=split_command_words(cmd);words.push_back(path.string());
    int fds[2];
    if(pipe(fds)!=0)die("failed to create --pre pipe");
    pid_t pid=fork();
    if(pid<0){close(fds[0]);close(fds[1]);die("failed to fork --pre command");}
    if(pid==0){
        int infd=open(path.c_str(),O_RDONLY);
        if(infd>=0){dup2(infd,STDIN_FILENO);close(infd);}
        dup2(fds[1],STDOUT_FILENO);close(fds[0]);close(fds[1]);
        std::vector<char*> av;av.reserve(words.size()+1);for(auto& w:words)av.push_back(w.data());av.push_back(nullptr);
        execvp(av[0],av.data());_exit(127);
    }
    close(fds[1]);std::string out;char buf[16384];
    for(;;){ssize_t n=read(fds[0],buf,sizeof buf);if(n>0)out.append(buf,static_cast<std::size_t>(n));else if(n==0)break;else if(errno!=EINTR){close(fds[0]);die("failed reading --pre output");}}
    close(fds[0]);int status=0;while(waitpid(pid,&status,0)<0&&errno==EINTR){}
    if(!WIFEXITED(status)||WEXITSTATUS(status)!=0)die("--pre command failed for "+path.string());
    return out;
}
#endif
std::string archive_decode_file(const fs::path& path){archive*a=archive_read_new();archive_read_support_filter_all(a);archive_read_support_format_all(a);archive_read_support_format_raw(a);
#ifdef _WIN32
    if(archive_read_open_filename_w(a,path.c_str(),10240)!=ARCHIVE_OK){
#else
    if(archive_read_open_filename(a,path.c_str(),10240)!=ARCHIVE_OK){
#endif
    auto e=archive_error_string(a);std::string m=e?e:"cannot open archive";archive_read_free(a);die("archive decode failed: "+m);}std::string out;archive_entry*ent=nullptr;while(archive_read_next_header(a,&ent)==ARCHIVE_OK){if(archive_entry_filetype(ent)!=AE_IFREG){archive_read_data_skip(a);continue;}char buf[16384];for(;;){la_ssize_t n=archive_read_data(a,buf,sizeof buf);if(n==0)break;if(n<0){auto e=archive_error_string(a);std::string m=e?e:"archive read failed";archive_read_free(a);die(m);}out.append(buf,(size_t)n);}if(!out.empty()&&out.back()!='\n')out.push_back('\n');}archive_read_free(a);return out;}
bool pre_applies(const Args&a,std::string_view rel){if(a.pre.empty())return false;if(a.pre_globs.empty())return true;for(auto&g:a.pre_globs)if(globmatch(g,std::string(rel),false))return true;return false;}

std::string cache_path(const fs::path&root){const char*base=getenv("PERGREP_CACHE_DIR");fs::path dir;
#ifdef _WIN32
    if(!base){const char*la=getenv("LOCALAPPDATA");dir=la?to_path(la)/"pergrep":to_path(".")/"pergrep-cache";}
#else
    if(!base)dir=(getenv("XDG_CACHE_HOME")?to_path(getenv("XDG_CACHE_HOME"))/"pergrep":to_path(getenv("HOME")?getenv("HOME"):".")/".cache/pergrep");
#endif
    if(base)dir=to_path(base);
    auto s=pergrep_cli::platform::path_to_utf8(fs::weakly_canonical(root));uint64_t h=1469598103934665603ull;for(unsigned char c:s){h^=c;h*=1099511628211ull;}
    // Cache identity includes the current on-disk format, so a legacy v5/v6
    // cache can never collide with a portable v7 snapshot.
    for(unsigned char c : pergrep::kIndexFormatIdentity){h^=c;h*=1099511628211ull;}
    std::ostringstream o;o<<std::hex<<h;return(dir/o.str()/"index.pgi").string();}

struct IgnoreRule { fs::path base; std::string pat; bool neg=false, ci=false, dir_only=false; };
struct PathSelector { std::string rel; bool directory=false; };

bool wildcard_path_match(std::string_view pat,std::string_view text,bool ci){
    auto eq=[&](char a,char b){if(ci){a=char(std::tolower((unsigned char)a));b=char(std::tolower((unsigned char)b));}return a==b;};
    const std::size_t W=text.size()+1;std::vector<signed char>memo((pat.size()+1)*W,-1);
    std::function<bool(std::size_t,std::size_t)> go=[&](std::size_t i,std::size_t j)->bool{auto&mm=memo[i*W+j];if(mm!=-1)return mm;bool ok=false;if(i==pat.size())ok=j==text.size();else if(pat[i]=='*'){bool dbl=i+1<pat.size()&&pat[i+1]=='*';std::size_t ni=i+(dbl?2:1);if(dbl&&ni<pat.size()&&pat[ni]=='/'){if(go(ni+1,j))ok=true;for(std::size_t k=j;!ok&&k<text.size();++k)if(text[k]=='/'&&go(ni+1,k+1))ok=true;}else{if(go(ni,j))ok=true;for(std::size_t k=j;!ok&&k<text.size()&&(dbl||text[k]!='/');++k)if(go(ni,k+1))ok=true;}}else if(pat[i]=='?'){
        // '?' must consume one UTF-8 code point, not one byte, and must not be '/'.
        if(j<text.size() && text[j]!='/'){
            std::size_t clen = pergrep_cli::platform::utf8_char_len(static_cast<unsigned char>(text[j]));
            if(j + clen > text.size()) clen = 1;
            ok = go(i+1, j+clen);
        }
    }else if(pat[i]=='['){std::size_t e=pat.find(']',i+1);if(e==std::string_view::npos){ok=j<text.size()&&eq('[',text[j])&&go(i+1,j+1);}else if(j<text.size()&&text[j]!='/'){bool neg=i+1<e&&(pat[i+1]=='!'||pat[i+1]=='^');std::size_t k=i+1+(neg?1:0);bool hit=false;while(k<e){if(k+2<e&&pat[k+1]=='-'){char a=pat[k],b=pat[k+2],c=text[j];if(ci){a=char(std::tolower((unsigned char)a));b=char(std::tolower((unsigned char)b));c=char(std::tolower((unsigned char)c));}if(c>=a&&c<=b)hit=true;k+=3;}else{if(eq(pat[k],text[j]))hit=true;++k;}}if(neg)hit=!hit;ok=hit&&go(e+1,j+1);}}else ok=j<text.size()&&eq(pat[i],text[j])&&go(i+1,j+1);mm=ok?1:0;return ok;};return go(0,0);
}
bool gitignore_rule_match(const IgnoreRule&r,std::string local){
    std::string pat=r.pat;bool anchored=!pat.empty()&&pat[0]=='/';if(anchored)pat.erase(pat.begin());
    auto exact=[&](std::string_view x){return wildcard_path_match(pat,x,r.ci);};
    if(r.dir_only){std::vector<std::string_view>dirs;for(std::size_t p=0;(p=local.find('/',p))!=std::string::npos;++p)dirs.push_back(std::string_view(local).substr(0,p));for(auto d:dirs){if(anchored){if(exact(d))return true;}else if(pat.find('/')!=std::string::npos){if(exact(d))return true;}else{auto pos=d.rfind('/');auto base=pos==std::string_view::npos?d:d.substr(pos+1);if(exact(base))return true;}}return false;}
    if(anchored)return exact(local);
    if(pat.find('/')!=std::string::npos)return exact(local);
    std::size_t b=0;for(;;){auto e=local.find('/',b);auto seg=std::string_view(local).substr(b,e==std::string::npos?std::string::npos:e-b);if(exact(seg))return true;if(e==std::string::npos)break;b=e+1;}return false;
}


std::pair<fs::path,std::vector<PathSelector>> resolve_paths(const std::vector<std::string>& paths) {
    std::vector<fs::path> abs, bases;
    for (auto& raw : paths) {
        std::error_code ec; fs::path p=fs::absolute(to_path(raw),ec); if(ec||!fs::exists(p)) die("path does not exist: "+raw);
        p=fs::weakly_canonical(p,ec); if(ec) die("cannot resolve path: "+raw); abs.push_back(p); bases.push_back(fs::is_directory(p)?p:p.parent_path());
    }
    fs::path root=bases.front();
    auto is_prefix=[](const fs::path&a,const fs::path&b){auto ia=a.begin(),ib=b.begin();for(;ia!=a.end();++ia,++ib)if(ib==b.end()||*ia!=*ib)return false;return true;};
    while(!root.empty()) { bool ok=true; for(auto&b:bases) ok &= is_prefix(root,b); if(ok) break; root=root.parent_path(); }
    if(root.empty()){
#ifdef _WIN32
        root=fs::path("\\");
#else
        root=fs::path("/");
#endif
    }
    std::vector<PathSelector> sel; for(auto&p:abs){auto rel=fs::relative(p,root).generic_string();if(rel.empty())rel=".";sel.push_back({rel,fs::is_directory(p)});} return {root,sel};
}

std::vector<IgnoreRule> load_ignore(const fs::path&root,const Args&a){
    std::vector<IgnoreRule>rs;if(a.no_ignore)return rs;
    auto read=[&](fs::path p,fs::path base,bool ci){std::ifstream f(p);if(!f)return;std::string x;while(std::getline(f,x)){if(!x.empty()&&x.back()=='\r')x.pop_back();if(x.empty()||x[0]=='#')continue;bool neg=x[0]=='!';if(neg)x.erase(x.begin());bool dir=!x.empty()&&x.back()=='/';if(dir)x.pop_back();rs.push_back({fs::absolute(base).lexically_normal(),x,neg,ci,dir});}};
    auto git_root=[&]()->std::optional<fs::path>{for(fs::path p=fs::absolute(root).lexically_normal();!p.empty();p=p.parent_path()){if(fs::exists(p/".git"))return p;if(p==p.root_path())break;}return std::nullopt;};
    auto gr=git_root();bool vcs_active=a.no_require_git||gr.has_value();
    // Manual ignore files have lower precedence than discovered local ignore files.
    if(!a.no_ignore_files)for(auto&p:a.ignore_files)read(fs::absolute(to_path(p)),root,a.ignore_file_ci);
    if(!a.no_ignore_global&&vcs_active){const char*xdg=getenv("XDG_CONFIG_HOME"),*home=getenv("HOME");fs::path gp=xdg?to_path(xdg)/"git/ignore":(home?to_path(home)/".config/git/ignore":fs::path{});if(!gp.empty())read(gp,root,false);}
    if(!a.no_ignore_exclude&&gr)read(*gr/".git/info/exclude",*gr,false);
    std::vector<fs::path> discovered;
    if(!a.no_ignore_parent){for(fs::path p=fs::absolute(root).parent_path();!p.empty();p=p.parent_path()){if(!a.no_ignore_dot){if(fs::exists(p/".ignore"))discovered.push_back(p/".ignore");if(fs::exists(p/".rgignore"))discovered.push_back(p/".rgignore");}if(!a.no_ignore_vcs&&vcs_active&&fs::exists(p/".gitignore"))discovered.push_back(p/".gitignore");if(p==p.root_path())break;}}
    std::error_code ec;for(auto const&e:fs::recursive_directory_iterator(root,fs::directory_options::skip_permission_denied,ec)){if(ec){ec.clear();continue;}if(!e.is_regular_file())continue;auto n=e.path().filename().string();if(!a.no_ignore_dot&&(n==".ignore"||n==".rgignore"))discovered.push_back(e.path());else if(!a.no_ignore_vcs&&vcs_active&&n==".gitignore")discovered.push_back(e.path());}
    std::sort(discovered.begin(),discovered.end(),[](const fs::path&a,const fs::path&b){auto da=std::distance(a.begin(),a.end()),db=std::distance(b.begin(),b.end());if(da!=db)return da<db;auto na=a.filename().string(),nb=b.filename().string();auto rank=[](const std::string&n){return n==".gitignore"?0:n==".ignore"?1:2;};if(a.parent_path()==b.parent_path()&&rank(na)!=rank(nb))return rank(na)<rank(nb);return a.generic_string()<b.generic_string();});
    discovered.erase(std::unique(discovered.begin(),discovered.end()),discovered.end());for(auto&p:discovered)read(p,p.parent_path(),false);return rs;
}

bool selector_match(std::string_view path,const std::vector<PathSelector>&sel){
    if(sel.empty()) return true;
    for(auto&s:sel){
        if(s.rel==".") return true;
        if(!s.directory){ if(path==s.rel) return true; }
        else if(path==s.rel||(path.size()>s.rel.size()&&path.substr(0,s.rel.size())==s.rel&&path[s.rel.size()]=='/')) return true;
    }
    return false;
}

bool allowed_path(const fs::path&root,std::string path,const FileInfo&fi,const Args&a,const std::vector<IgnoreRule>&ign,const std::vector<PathSelector>&sel,const TypeMap&tm){
    if(!selector_match(path,sel)) return false;
    if(a.max_filesize&&fi.size>a.max_filesize) return false;
    if(a.one_file_system){for(const auto&q:sel){bool owns=q.rel=="."||(q.directory&&(path==q.rel||path.rfind(q.rel+"/",0)==0))||(!q.directory&&path==q.rel);if(!owns)continue;fs::path base=q.rel=="."?root:(root/to_path(q.rel));if(!q.directory)base=base.parent_path();if(!pergrep_cli::platform::same_device(root/to_path(path),base))return false;}}
    bool explicit_file=false;for(const auto& q:sel)if(!q.directory&&q.rel==path){explicit_file=true;break;}
    if(explicit_file)return true;
    if(a.max_depth>=0){
        bool depth_ok=false;
        for(const auto& q:sel){
            if(!q.directory)continue;
            std::string rem;
            if(q.rel==".") rem=path;
            else if(path==q.rel) rem="";
            else if(path.rfind(q.rel+"/",0)==0) rem=path.substr(q.rel.size()+1);
            else continue;
            if(rem.empty()) { if(a.max_depth>=0) depth_ok=true; }
            else { int depth=1; for(char c:rem) if(c=='/') ++depth; if(depth<=a.max_depth) depth_ok=true; }
        }
        if(!depth_ok)return false;
    }
    if(!a.hidden){for(auto&part:to_path(path)){auto q=part.string();if(q.size()>1&&q[0]=='.')return false;}}
    bool ignored=false;auto full=(fs::absolute(root)/to_path(path)).lexically_normal();for(auto&r:ign){auto localp=full.lexically_relative(r.base);if(localp.empty())continue;auto local=localp.generic_string();if(local==".."||local.rfind("../",0)==0)continue;if(gitignore_rule_match(r,local))ignored=!r.neg;}if(ignored)return false;
    bool has_pos=false,pos_hit=false;for(auto&g:a.globs){bool neg=!g.empty()&&g[0]=='!';auto pat=neg?g.substr(1):g;if(neg){if(globmatch(pat,path,a.glob_ci))return false;}else{has_pos=true;pos_hit|=globmatch(pat,path,a.glob_ci);}}if(has_pos&&!pos_hit)return false;
    if(!a.iglobs.empty()){bool ok=false;for(auto&g:a.iglobs){bool neg=!g.empty()&&g[0]=='!';auto pat=neg?g.substr(1):g;if(neg&&globmatch(pat,path,true))return false;if(!neg)ok|=globmatch(pat,path,true);}if(std::any_of(a.iglobs.begin(),a.iglobs.end(),[](auto&g){return g.empty()||g[0]!='!';})&&!ok)return false;}
    auto typeok=[&](const std::string& t){auto it=tm.find(t);if(it==tm.end())return false;for(auto&p:it->second)if(globmatch(p,path,false))return true;return false;};auto recognized=[&](){for(const auto&[_,ps]:tm)for(const auto&p:ps)if(globmatch(p,path,false))return true;return false;};if(!a.types.empty()){bool ok=false;for(auto&t:a.types)ok|=(t=="all"?recognized():typeok(t));if(!ok)return false;}for(auto&t:a.type_not)if(t=="all"?recognized():typeok(t))return false;return true;
}

struct LineRecord {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;      // without terminator
    std::uint64_t term_end = 0; // including terminator when present
};

std::vector<LineRecord> split_lines(std::string_view data, char sep) {
    std::vector<LineRecord> out;
    std::uint64_t begin = 0;
    for (std::uint64_t i = 0; i < data.size(); ++i) {
        if (data[i] != sep) continue;
        out.push_back({begin, i, i + 1});
        begin = i + 1;
    }
    if (begin < data.size()) out.push_back({begin, static_cast<std::uint64_t>(data.size()), static_cast<std::uint64_t>(data.size())});
    return out;
}

std::size_t line_for_offset(const std::vector<LineRecord>& lines, std::uint64_t off) {
    if (lines.empty()) return 0;
    auto it = std::upper_bound(lines.begin(), lines.end(), off,
        [](std::uint64_t x, const LineRecord& line) { return x < line.begin; });
    if (it == lines.begin()) return 0;
    auto n = static_cast<std::size_t>(std::distance(lines.begin(), it) - 1);
    return std::min(n, lines.size() - 1);
}

std::string json_escape(std::string_view s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) { char b[7]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else o += static_cast<char>(c);
        }
    }
    return o;
}


bool valid_utf8(std::string_view s) {
    std::size_t i=0;
    while(i<s.size()) {
        unsigned char c=(unsigned char)s[i]; std::size_t n=0; unsigned cp=0;
        if(c<0x80){++i;continue;} else if((c&0xE0)==0xC0){n=2;cp=c&0x1F;if(cp<2)return false;}
        else if((c&0xF0)==0xE0){n=3;cp=c&0x0F;} else if((c&0xF8)==0xF0){n=4;cp=c&0x07;if(cp>4)return false;} else return false;
        if(i+n>s.size()) return false;
        for(std::size_t j=1;j<n;++j){ unsigned char d=(unsigned char)s[i+j]; if((d&0xC0)!=0x80)return false; cp=(cp<<6)|(d&0x3F); }
        if((n==3&&cp<0x800)||(n==4&&cp<0x10000)||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff)) return false;
        i+=n;
    } return true;
}
std::string base64(std::string_view s) {
    static constexpr char T[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; std::string o; o.reserve((s.size()+2)/3*4);
    for(std::size_t i=0;i<s.size();i+=3){unsigned v=(unsigned char)s[i]<<16;int n=1;if(i+1<s.size()){v|=(unsigned char)s[i+1]<<8;++n;}if(i+2<s.size()){v|=(unsigned char)s[i+2];++n;}o.push_back(T[(v>>18)&63]);o.push_back(T[(v>>12)&63]);o.push_back(n>1?T[(v>>6)&63]:'=');o.push_back(n>2?T[v&63]:'=');} return o;
}
std::string json_data(std::string_view s) { return valid_utf8(s) ? std::string("{\"text\":\"")+json_escape(s)+"\"}" : std::string("{\"bytes\":\"")+base64(s)+"\"}"; }
bool color_enabled(const Args& a) { return a.color=="always"||a.color=="ansi"||(a.color=="auto"&&pergrep_cli::platform::isatty_stdout()); }

struct AnsiStyle {
    bool enabled = true;
    std::optional<std::string> fg;
    std::optional<std::string> bg;
    bool bold = false;
    bool intense = false;
    bool underline = false;
    bool italic = false;
};

int parse_color_component(std::string_view s) {
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s.remove_prefix(2); }
    if (s.empty()) die("invalid --colors value");
    unsigned x = 0;
    auto [q, ec] = std::from_chars(s.data(), s.data()+s.size(), x, base);
    if (ec != std::errc() || q != s.data()+s.size() || x > 255) die("invalid --colors value");
    return static_cast<int>(x);
}

std::string color_sgr(std::string_view value, bool background) {
    static const std::unordered_map<std::string,int> named = {
        {"black",30},{"red",31},{"green",32},{"yellow",33},
        {"blue",34},{"magenta",35},{"cyan",36},{"white",37},
    };
    if (auto it=named.find(std::string(value)); it!=named.end())
        return std::to_string(it->second + (background ? 10 : 0));
    if (value.find(',') != std::string_view::npos) {
        std::array<int,3> rgb{}; std::size_t b=0;
        for (int i=0;i<3;++i) {
            auto c=value.find(',',b); auto part=value.substr(b,c==std::string_view::npos?std::string_view::npos:c-b);
            rgb[i]=parse_color_component(part);
            if (i<2 && c==std::string_view::npos) die("invalid RGB --colors value");
            if (i==2 && c!=std::string_view::npos) die("invalid RGB --colors value");
            b=c==std::string_view::npos?value.size():c+1;
        }
        return std::string(background?"48;2;":"38;2;")+std::to_string(rgb[0])+";"+std::to_string(rgb[1])+";"+std::to_string(rgb[2]);
    }
    int n=parse_color_component(value);
    return std::string(background?"48;5;":"38;5;")+std::to_string(n);
}

AnsiStyle default_style(std::string_view type) {
    AnsiStyle s;
    if (type == "match") { s.fg="31"; s.bold=true; }
    else if (type == "path") s.fg="35";
    else if (type == "line" || type == "column") s.fg="32";
    else s.enabled=false;
    return s;
}

AnsiStyle style_for(std::string_view type, const Args& a) {
    AnsiStyle st=default_style(type);
    for (const auto& spec : a.color_specs) {
        auto p1=spec.find(':');
        if (p1==std::string::npos || spec.substr(0,p1)!=type) continue;
        auto rest=spec.substr(p1+1);
        if (rest=="none") { st=AnsiStyle{}; st.enabled=false; continue; }
        auto p2=rest.find(':');
        if (p2==std::string::npos) die("invalid --colors specification: "+spec);
        auto attr=rest.substr(0,p2), val=rest.substr(p2+1); st.enabled=true;
        if (attr=="fg") st.fg=color_sgr(val,false);
        else if (attr=="bg") st.bg=color_sgr(val,true);
        else if (attr=="style") {
            if(val=="bold")st.bold=true; else if(val=="nobold")st.bold=false;
            else if(val=="intense")st.intense=true; else if(val=="nointense")st.intense=false;
            else if(val=="underline")st.underline=true; else if(val=="nounderline")st.underline=false;
            else if(val=="italic")st.italic=true; else if(val=="noitalic")st.italic=false;
            else die("invalid --colors style: "+val);
        } else die("invalid --colors attribute: "+attr);
    }
    return st;
}

std::string ansi_apply(std::string_view text, std::string_view type, const Args& a) {
    if (!color_enabled(a)) return std::string(text);
    auto st=style_for(type,a); if(!st.enabled) return std::string(text);
    std::vector<std::string> codes;
    if(st.bold) codes.push_back("1");
    if(st.intense) codes.push_back("1");
    if(st.underline) codes.push_back("4");
    if(st.italic) codes.push_back("3");
    if(st.fg) codes.push_back(*st.fg);
    if(st.bg) codes.push_back(*st.bg);
    if(codes.empty()) return std::string(text);
    std::string out="\x1b[0m\x1b[";
    for(std::size_t i=0;i<codes.size();++i){if(i)out+=';';out+=codes[i];}
    out+='m'; out.append(text); out+="\x1b[0m"; return out;
}

std::string ansi_match(std::string_view s,const Args&a){return ansi_apply(s,"match",a);}
std::string ansi_line(std::string_view s,const Args&a){return ansi_apply(s,"line",a);}
std::string ansi_column(std::string_view s,const Args&a){return ansi_apply(s,"column",a);}
std::string ansi_path(std::string_view s,const Args&a){
    std::string p(s);
    if(!a.hyperlink_format.empty() && a.hyperlink_format!="none" && color_enabled(a)){
        std::string target;
        if(a.hyperlink_format=="default" || a.hyperlink_format=="file") target="file://"+fs::absolute(to_path(s)).generic_string();
        else {
            target=a.hyperlink_format;
            std::string abs=fs::absolute(to_path(s)).generic_string();
            for(std::string key : {"{path}","{path_abs}"}) for(std::size_t pos=0;(pos=target.find(key,pos))!=std::string::npos;) { target.replace(pos,key.size(),abs); pos+=abs.size(); }
            for(std::string key : {"{line}","{column}"}) for(std::size_t pos=0;(pos=target.find(key,pos))!=std::string::npos;) { target.replace(pos,key.size(),"1"); pos+=1; }
        }
        p="\x1b]8;;"+target+"\x1b\\"+p+"\x1b]8;;\x1b\\";
    }
    return ansi_apply(p,"path",a);
}

std::string display_path(std::string path, const Args& a) {
    if (!a.path_sep.empty()) {
        if (a.path_sep.size() != 1) die("--path-separator must be one byte");
        for (char& c : path) if (c == '/' || c == '\\') c = a.path_sep[0];
    }
    return path;
}

std::string interpolate_replacement(std::string_view repl, std::string_view data, const Match& m) {
    auto capture_num = [&](std::size_t i) -> std::string_view {
        if (i == 0) return data.substr(m.start, m.end - m.start);
        if (i >= m.captures.size() || !m.captures[i].matched) return {};
        return data.substr(m.captures[i].start, m.captures[i].end - m.captures[i].start);
    };
    auto capture_name = [&](std::string_view name) -> std::string_view {
        for (const auto& c : m.captures) if (c.matched && c.name == name)
            return data.substr(c.start, c.end - c.start);
        return {};
    };
    std::string out;
    for (std::size_t i = 0; i < repl.size();) {
        if (repl[i] != '$') { out += repl[i++]; continue; }
        if (i + 1 >= repl.size()) { out += '$'; ++i; continue; }
        if (repl[i + 1] == '$') { out += '$'; i += 2; continue; }
        std::size_t j = i + 1; bool braced = repl[j] == '{'; if (braced) ++j;
        std::size_t begin = j;
        while (j < repl.size() && (std::isalnum(static_cast<unsigned char>(repl[j])) || repl[j] == '_')) ++j;
        if (j == begin) { out += '$'; ++i; continue; }
        if (braced) { if (j >= repl.size() || repl[j] != '}') { out += '$'; ++i; continue; } }
        auto key = repl.substr(begin, j - begin); bool numeric = std::all_of(key.begin(), key.end(), [](unsigned char c){ return std::isdigit(c); });
        if (numeric) { std::size_t n = 0; for (char c : key) n = n * 10 + static_cast<unsigned>(c - '0'); out.append(capture_num(n)); }
        else out.append(capture_name(key));
        i = j + (braced ? 1 : 0);
    }
    return out;
}

std::string replace_line(std::string_view data, const LineRecord& line, const std::vector<Match>& matches, std::string_view repl, const std::vector<LineRecord>& lines = {}, std::size_t line_idx = 0) {
    std::string out;
    std::uint64_t cursor = line.begin;
    for (const auto& m : matches) {
        if (m.end < cursor) continue;
        if (m.start >= line.term_end) break;
        if (m.start < cursor) {
            cursor = std::max(cursor, m.end);
        } else {
            out.append(data.substr(cursor, m.start - cursor));
            out += interpolate_replacement(repl, data, m);
            cursor = m.end;
            if (cursor > line.term_end && !lines.empty() && line_idx < lines.size()) {
                auto last_li = line_for_offset(lines, m.end > m.start ? m.end - 1 : m.start);
                if (last_li < lines.size()) {
                    if (cursor < lines[last_li].term_end) {
                        out.append(data.substr(cursor, lines[last_li].term_end - cursor));
                    }
                    cursor = lines[last_li].term_end;
                }
            }
        }
        if (cursor >= line.term_end) break;
    }
    if (cursor < line.term_end) {
        out.append(data.substr(cursor, line.term_end - cursor));
    }
    return out;
}

std::vector<std::vector<const Match*>> matches_by_line(
    const std::vector<LineRecord>& lines, const std::vector<Match>& matches) {
    std::vector<std::vector<const Match*>> out(lines.size());
    for (const auto& m : matches) {
        if (lines.empty()) break;
        auto first = line_for_offset(lines, m.start);
        auto last = line_for_offset(lines, m.end > m.start ? m.end - 1 : m.start);
        for (std::size_t i = first; i <= last && i < lines.size(); ++i) out[i].push_back(&m);
    }
    return out;
}

} // namespace

#ifdef _WIN32
// Windows: command-line arguments arrive as UTF-16; convert them to UTF-8 so
// paths with non-ANSI characters work. The rest of the CLI is encoding-agnostic.
int run_main(int argc, char** argv);
int wmain(int argc, wchar_t** wargv) {
    // Emit UTF-8 to the console and read stdin as bytes; without this the CRT
    // and console interpret output through the active ANSI code page.
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
    ::SetConsoleOutputCP(CP_UTF8);
    ::SetConsoleCP(CP_UTF8);
    std::vector<std::string> storage;
    storage.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        int n = ::WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        if (n <= 0) { storage.emplace_back(); continue; }
        std::string s(static_cast<std::size_t>(n), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), n, nullptr, nullptr);
        s.pop_back(); // trailing NUL from WideCharToMultiByte
        storage.push_back(std::move(s));
    }
    std::vector<char*> av;
    av.reserve(storage.size());
    for (auto& s : storage) av.push_back(s.data());
    return run_main(argc, av.data());
}
int run_main(int argc, char** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        auto a = parse_with_config(argc, argv);
        if (a.help || a.short_help) { std::cout << help(); return 0; }
        if (a.version) { std::cout << "pergrep " << pergrep::version() << " (ripgrep CLI target 15.2.0)\n"; return 0; }
        if (a.pcre2_version) { std::cout << "PCRE2 compatibility mode: internal pergrep extended VM (no PCRE2 matcher linked)\n"; return 0; }
        if(!a.generate.empty()){std::string opts="--after-context --before-context --context --binary --block-buffered --byte-offset --case-sensitive --color --colors --column --context-separator --count --count-matches --crlf --debug --dfa-size-limit --encoding --engine --field-context-separator --field-match-separator --files --files-with-matches --files-without-match --fixed-strings --follow --glob --glob-case-insensitive --heading --help --hidden --hyperlink-format --iglob --ignore-case --ignore-file --ignore-file-case-insensitive --include-zero --invert-match --json --line-buffered --line-number --line-regexp --max-columns --max-columns-preview --max-count --max-depth --max-filesize --mmap --multiline --multiline-dotall --no-config --no-ignore --no-ignore-dot --no-ignore-exclude --no-ignore-files --no-ignore-global --no-ignore-parent --no-ignore-vcs --no-messages --no-require-git --no-unicode --null --null-data --one-file-system --only-matching --path-separator --passthru --pcre2 --pre --pre-glob --pretty --quiet --regexp --replace --search-zip --smart-case --sort --sortr --stats --stop-on-nonmatch --text --threads --trace --trim --type --type-not --type-add --type-clear --type-list --unrestricted --version --vimgrep --with-filename --word-regexp";if(a.generate=="man"){std::cout<<".TH PERGREP 1\n.SH NAME\npergrep - indexed search\n.SH OPTIONS\n";std::istringstream is(opts);std::string o;while(is>>o)std::cout<<".TP\n\fB"<<o<<"\fP\n";}else if(a.generate=="complete-bash"){std::cout<<"complete -W '"<<opts<<"' pergrep\n";}else if(a.generate=="complete-zsh"){std::cout<<"#compdef pergrep\n_arguments '1:option:("<<opts<<")' '*:file:_files'\n";}else if(a.generate=="complete-fish"){std::istringstream is(opts);std::string o;while(is>>o)if(o.rfind("--",0)==0)std::cout<<"complete -c pergrep -l "<<o.substr(2)<<"\n";}else if(a.generate=="complete-powershell"){std::cout<<"Register-ArgumentCompleter -Native -CommandName pergrep -ScriptBlock { param($wordToComplete) @('"<<opts<<"') }\n";}else die("unsupported --generate target: "+a.generate);return 0;}
        if (a.type_list) {
            auto tm=effective_type_map(a);std::vector<std::string> names;for(auto&[k,_]:tm)names.push_back(k);std::sort(names.begin(),names.end());
            for(const auto& k:names){auto&v=tm[k];std::cout<<k<<":";for(size_t i=0;i<v.size();++i)std::cout<<(i?", ":" ")<<v[i];std::cout<<"\n";}return tm.empty()?1:0;
        }
        if (a.patterns.empty() && !a.files_mode) die("a pattern is required");

        fs::path root; std::vector<PathSelector> selectors; std::shared_ptr<Index> idx;
        if(a.stdin_haystack){
            root=fs::current_path(); selectors={{"-",false}}; std::string data((std::istreambuf_iterator<char>(std::cin)),{}); data=decode_input(data,a.encoding); idx=std::make_shared<Index>(Index::from_documents({{"-",std::move(data)}}));
        }else{
            auto rp=resolve_paths(a.paths);root=std::move(rp.first);selectors=std::move(rp.second);auto cp=cache_path(root);
            if(!a.follow)try { auto x=Index::load(cp); if(x.fresh())idx=std::make_shared<Index>(std::move(x)); } catch(...) {}
            if(!idx){auto t=std::chrono::steady_clock::now();IndexOptions io;io.follow_symlinks=a.follow;idx=std::make_shared<Index>(Index::build(root,io));if(!a.follow)idx->save(cp);if(a.debug)std::cerr<<"pergrep: built index in "<<std::chrono::duration<double>(std::chrono::steady_clock::now()-t).count()<<"s\n";}
        }

        if(!a.stdin_haystack){bool transform=a.encoding!="auto"||!a.pre.empty()||a.search_zip;if(!transform){for(std::size_t i=0;i<idx->files().size();++i){auto d=idx->content(i);if(d.size()>=2&&(((unsigned char)d[0]==0xff&&(unsigned char)d[1]==0xfe)||((unsigned char)d[0]==0xfe&&(unsigned char)d[1]==0xff))){transform=true;break;}}}if(transform){std::vector<Document> docs;docs.reserve(idx->files().size());for(std::size_t i=0;i<idx->files().size();++i){auto rel=idx->files()[i].path;auto full=root/to_path(rel);std::string bytes;if(pre_applies(a,rel))bytes=run_preprocessor(a.pre,full);else if(a.search_zip){std::error_code ec;auto ext=full.extension().string();if(ext==".gz"||ext==".tgz"||ext==".bz2"||ext==".xz"||ext==".lz4"||ext==".zst"||ext==".zip"||ext==".tar")bytes=archive_decode_file(full);else bytes=std::string(idx->content(i));}else bytes=std::string(idx->content(i));docs.push_back({rel,decode_input(bytes,a.encoding)});}idx=std::make_shared<Index>(Index::from_documents(std::move(docs)));}}
        auto ignore = a.stdin_haystack ? std::vector<IgnoreRule>{} : load_ignore(root, a);
        auto tm = effective_type_map(a);
        std::vector<uint8_t> allowed(idx->files().size());
        std::vector<uint32_t> eligible_file_ids;
        for (uint32_t i = 0; i < allowed.size(); ++i) {
            allowed[i] = allowed_path(root, idx->files()[i].path, idx->files()[i], a, ignore, selectors, tm);
            if (allowed[i]) eligible_file_ids.push_back(i);
        }
        if (a.files_mode) {
            std::vector<uint32_t> ids; for(uint32_t i=0;i<allowed.size();++i)if(allowed[i])ids.push_back(i);
            auto key=[&](uint32_t i){return idx->files()[i].path;}; bool rev=a.sort.rfind("reverse:",0)==0; std::string kind=rev?a.sort.substr(8):a.sort;
            if(kind.empty()||kind=="path")std::sort(ids.begin(),ids.end(),[&](auto x,auto y){return rev?key(x)>key(y):key(x)<key(y);});
            for(auto i:ids) std::cout<<display_path(idx->files()[i].path,a)<<(a.null_out?'\0':'\n');
            return 0;
        }

        Searcher search(*idx);
        bool any = false;
        SearchStats total{};
        auto start = std::chrono::steady_clock::now();
        std::vector<std::vector<Match>> perpat;
        SearchOptions core_opt = a.sopt;
        core_opt.invert_match = false;
        core_opt.files_with_matches = false;
        core_opt.files_without_match = false;
        core_opt.max_matches = 0;
        // Quiet positive searches need existence; invert mode must retain all matches
        // so the line-level non-match decision remains exact.
        core_opt.objective = a.quiet && !a.sopt.invert_match ? SearchObjective::FirstHit : SearchObjective::Exhaustive;
        core_opt.include_binary = true;
        core_opt.record_separator = a.null_data ? '\0' : '\n';
        core_opt.eligible_file_ids = eligible_file_ids;
        if (!eligible_file_ids.empty() && !(a.max_count_set && a.max_count == 0)) {
            for (auto& ps : a.patterns) {
                auto p = Pattern::compile(ps, a.popt);
                SearchStats st;
                auto ms = search.find(p, core_opt, &st);
                total.candidate_chunks += st.candidate_chunks;
                total.candidate_blocks += st.candidate_blocks;
                total.verified_bytes += st.verified_bytes;
                total.matches += st.matches;
                perpat.push_back(std::move(ms));
            }
        }

        std::vector<std::vector<Match>> byfile(idx->files().size());
        for (auto& ms : perpat) for (auto& m : ms)
            if (m.file_id < allowed.size() && allowed[m.file_id]) byfile[m.file_id].push_back(m);
        for (auto& v : byfile) {
            std::sort(v.begin(), v.end(), [](const auto& x, const auto& y) {
                return x.start == y.start ? x.end < y.end : x.start < y.start;
            });
            v.erase(std::unique(v.begin(), v.end(), [](const auto& x, const auto& y) {
                return x.start == y.start && x.end == y.end;
            }), v.end());
        }

        std::vector<uint32_t> order(idx->files().size());
        std::iota(order.begin(), order.end(), 0);
        if(!a.sort.empty()){bool rev=a.sort.rfind("reverse:",0)==0;std::string kind=rev?a.sort.substr(8):a.sort;if(kind=="path:reverse"){kind="path";rev=true;}
            auto meta=[&](uint32_t id)->std::int64_t{auto path=root/to_path(idx->files()[id].path);std::error_code ec;if(kind=="modified")return idx->files()[id].mtime_ns;if(kind=="accessed")return pergrep_cli::platform::file_time_ns(path,"accessed");if(kind=="created"){auto v=pergrep_cli::platform::file_time_ns(path,"created");if(v==0)die("creation time unavailable");return v;}return 0;};
            if(kind=="path")std::sort(order.begin(),order.end(),[&](auto x,auto y){return rev?idx->files()[x].path>idx->files()[y].path:idx->files()[x].path<idx->files()[y].path;});
            else if(kind=="modified"||kind=="accessed"||kind=="created")std::stable_sort(order.begin(),order.end(),[&](auto x,auto y){auto a1=meta(x),b1=meta(y);return rev?a1>b1:a1<b1;});
            else die("invalid sort kind: "+kind); }

        const bool default_show_filename = !a.stdin_haystack && (selectors.size() > 1 || std::any_of(selectors.begin(), selectors.end(), [](const PathSelector& s){ return s.directory; }));
        const bool show_filename = !a.no_filename && (a.with_filename || default_show_filename);
        const bool heading = show_filename && (a.heading || (!a.no_heading && pergrep_cli::platform::isatty_stdout()));
        if (a.json && (a.files_mode || a.files_with || a.files_without || a.count || a.count_matches))
            die("--json cannot be combined with file/count summary output modes");

        std::uint64_t json_searches = 0, json_searches_with_match = 0, json_bytes = 0, json_matched_lines = 0, json_matches = 0, json_bytes_printed = 0;
        auto print_path_field = [&](std::string_view path) {
            std::cout << ansi_path(path,a);
            if (a.null_out) std::cout.put('\0'); else std::cout << a.field_match_sep;
        };

        for (auto fid : order) {
            if (!allowed[fid]) continue;
            auto& ms = byfile[fid];

            std::string data(idx->content(fid));
            const auto nul = data.find('\0');
            const bool has_nul = nul != std::string::npos && !a.null_data;
            bool explicit_file = false; for(const auto& q:selectors) if(!q.directory && q.rel==idx->files()[fid].path){explicit_file=true;break;}
            const bool as_text = a.text || a.unrestricted>=3;
            const bool search_suppress = !as_text && has_nul && (a.binary || explicit_file);
            const bool auto_binary = !as_text && has_nul && !search_suppress;
            bool had_match_before_nul=false, had_match_after_nul=false;
            if(has_nul&&!as_text){for(const auto&m:ms){if(m.start<nul)had_match_before_nul=true;else had_match_after_nul=true;}ms.erase(std::remove_if(ms.begin(),ms.end(),[&](const Match&m){return m.start>=nul;}),ms.end());}
            std::string binary_notice;
            if(auto_binary && had_match_before_nul){binary_notice=display_path(idx->files()[fid].path,a)+": WARNING: stopped searching binary file after match (found \"\\0\" byte around offset "+std::to_string(nul)+")";}
            else if(search_suppress && (had_match_before_nul||had_match_after_nul)){binary_notice=(show_filename?display_path(idx->files()[fid].path,a)+": ":std::string{})+"binary file matches (found \"\\0\" byte around offset "+std::to_string(nul)+")";}

            auto lines = split_lines(data, a.null_data ? '\0' : '\n');
            auto line_matches = matches_by_line(lines, ms);
            std::vector<std::uint8_t> selected(lines.size(), 0);
            for (std::size_t i = 0; i < lines.size(); ++i) {
                bool matched = !line_matches[i].empty();
                selected[i] = static_cast<std::uint8_t>(a.sopt.invert_match ? !matched : matched);
            }
            if (a.stop_on_nonmatch) {
                bool seen_match = false;
                for (std::size_t i = 0; i < selected.size(); ++i) {
                    if (selected[i]) { seen_match = true; continue; }
                    if (seen_match) { std::fill(selected.begin()+static_cast<std::ptrdiff_t>(i), selected.end(), 0); break; }
                }
            }

            if (a.max_count_set) {
                std::uint64_t seen = 0;
                for (auto& bit : selected) if (bit) {
                    if (seen >= a.max_count) bit = 0;
                    else ++seen;
                }
            }
            const std::uint64_t selected_count = std::count(selected.begin(), selected.end(), std::uint8_t{1});
            bool hit = selected_count != 0;

            if (a.files_with || a.files_without) {
                bool emit = a.files_with ? hit : !hit;
                if (emit) {
                    any = true;
                    if (a.quiet) return 0;
                    std::cout << display_path(idx->files()[fid].path, a) << (a.null_out ? '\0' : '\n');
                }
                continue;
            }
            if (a.quiet && hit) return 0;

            if (a.count || a.count_matches) {
                std::uint64_t c = 0;
                if (a.count_matches && !a.sopt.invert_match) {
                    if (a.max_count_set) {
                        for (const auto& m : ms) {
                            auto li = line_for_offset(lines, m.start);
                            if (li < selected.size() && selected[li]) ++c;
                        }
                    } else c = ms.size();
                } else c = selected_count;
                if (c || a.include_zero) {
                    any |= c > 0;
                    if (show_filename) print_path_field(display_path(idx->files()[fid].path, a));
                    std::cout << c << '\n';
                }
                continue;
            }
            if (!hit && !a.passthru && a.after == 0 && a.before == 0 && !a.json) { if(!binary_notice.empty() && search_suppress){std::cout<<binary_notice<<'\n';any=true;} continue; }

            if (a.json) {
                if (!hit) continue;
                ++json_searches; ++json_searches_with_match; json_bytes += data.size(); json_matched_lines += selected_count;
                std::uint64_t file_matches = 0;
                if (a.sopt.invert_match) {
                    file_matches = selected_count;
                } else {
                    for (const auto& m : ms) {
                        auto li = line_for_offset(lines, m.start);
                        if (li < selected.size() && selected[li]) ++file_matches;
                    }
                }
                json_matches += file_matches;
                std::uint64_t file_bytes_printed = 0;
                for (std::size_t i = 0; i < lines.size(); ++i) {
                    if (selected[i]) file_bytes_printed += (lines[i].term_end - lines[i].begin);
                }
                json_bytes_printed += file_bytes_printed;
                auto path = display_path(idx->files()[fid].path, a);
                std::cout << "{\"type\":\"begin\",\"data\":{\"path\":" << json_data(path) << "}}\n";
                for (std::size_t i = 0; i < lines.size(); ++i) {
                    if (!selected[i]) continue;
                    const auto& line = lines[i];
                    std::cout << "{\"type\":\"match\",\"data\":{\"path\":" << json_data(path)
                              << ",\"lines\":" << json_data(std::string_view(data).substr(line.begin, line.term_end-line.begin))
                              << ",\"line_number\":" << (i+1) << ",\"absolute_offset\":" << line.begin << ",\"submatches\":[";
                    bool first = true;
                    if (!a.sopt.invert_match) for (const Match* mp : line_matches[i]) {
                        auto sb = std::max<std::uint64_t>(mp->start, line.begin);
                        auto se = std::min<std::uint64_t>(mp->end, line.term_end);
                        if (se < sb) continue;
                        if (!first) std::cout << ',';
                        first = false;
                        std::cout << "{\"match\":" << json_data(std::string_view(data).substr(sb,se-sb));
                        if (!a.replacement.empty()) std::cout << ",\"replacement\":" << json_data(interpolate_replacement(a.replacement,data,*mp));
                        std::cout << ",\"start\":" << (sb-line.begin) << ",\"end\":" << (se-line.begin) << '}';
                    }
                    std::cout << "]}}\n";
                }
                std::cout << "{\"type\":\"end\",\"data\":{\"path\":" << json_data(path)
                          << ",\"binary_offset\":null,\"stats\":{\"elapsed\":{\"secs\":0,\"nanos\":0,\"human\":\"0.000000s\"},"
                             "\"searches\":1,\"searches_with_match\":1,\"bytes_searched\":" << data.size()
                          << ",\"bytes_printed\":" << file_bytes_printed << ",\"matched_lines\":" << selected_count << ",\"matches\":"
                          << file_matches << "}}}\n";
                any = true;
                continue;
            }

            if (a.only_matching) {
                if (hit) any = true;
                if (a.sopt.invert_match) continue;
                for (const auto& m : ms) {
                    auto li = line_for_offset(lines, m.start);
                    if (li >= selected.size() || !selected[li]) continue;
                    auto path = display_path(idx->files()[fid].path, a);
                    if (show_filename) print_path_field(path);
                    if (a.line_number) std::cout << ansi_line(std::to_string(li + 1),a) << a.field_match_sep;
                    if (a.column) std::cout << ansi_column(std::to_string(m.start - lines[li].begin + 1),a) << a.field_match_sep;
                    if (a.byte_offset) std::cout << m.start << a.field_match_sep;
                    if (!a.replacement.empty()) std::cout << interpolate_replacement(a.replacement, data, m);
                    else std::cout << ansi_match(std::string_view(data).substr(m.start,m.end-m.start),a);
                    std::cout << (a.null_data ? '\0' : '\n');
                    any = true;
                }
                continue;
            }

            if (a.vimgrep) {
                if (a.sopt.invert_match) continue;
                for (const auto& m : ms) {
                    auto li = line_for_offset(lines, m.start);
                    if (li >= selected.size() || !selected[li]) continue;
                    auto path = display_path(idx->files()[fid].path, a);
                    std::cout << ansi_path(path,a) << ':' << ansi_line(std::to_string(li+1),a) << ':' << ansi_column(std::to_string(m.start-lines[li].begin+1),a) << ':';
                    std::cout << std::string_view(data).substr(lines[li].begin, lines[li].term_end-lines[li].begin);
                    if (lines[li].term_end == lines[li].end) std::cout << '\n';
                    any = true;
                }
                continue;
            }

            std::vector<std::uint8_t> emit(lines.size(), 0);
            if (a.passthru) std::fill(emit.begin(), emit.end(), 1);
            else {
                for (std::size_t i = 0; i < selected.size(); ++i) if (selected[i]) {
                    std::size_t lo = i > static_cast<std::size_t>(a.before) ? i-a.before : 0;
                    std::size_t hi = std::min(lines.size(), i+static_cast<std::size_t>(a.after)+1);
                    for (std::size_t j=lo;j<hi;++j) emit[j]=1;
                }
            }
            if (heading && std::any_of(emit.begin(), emit.end(), [](auto x){return x!=0;}))
                std::cout << ansi_path(display_path(idx->files()[fid].path, a),a) << '\n';
            bool previous_emitted = false;
            std::size_t previous_index = 0;
            std::size_t skip_until = 0;
            for (std::size_t i = 0; i < lines.size(); ++i) {
                if (i < skip_until) continue;
                if (!emit[i]) continue;
                bool match_line = selected[i] != 0;
                if (!a.passthru && (a.after > 0 || a.before > 0) && previous_emitted && i > previous_index + 1 && !a.context_sep_disabled)
                    std::cout << a.context_sep << '\n';
                previous_emitted = true; previous_index = i;

                const auto& line = lines[i];
                std::string rendered;
                if (match_line && !a.replacement.empty() && !a.sopt.invert_match) {
                    rendered = replace_line(data, line, ms, a.replacement, lines, i);
                    for (const auto& m : ms) {
                        if (m.start >= line.begin && m.start < line.term_end && m.end > line.term_end) {
                            auto last_li = line_for_offset(lines, m.end > m.start ? m.end - 1 : m.start);
                            if (last_li > i) skip_until = std::max(skip_until, last_li + 1);
                        }
                    }
                } else rendered.assign(data.data()+line.begin, data.data()+line.term_end);

                if (match_line && a.replacement.empty() && !a.sopt.invert_match && color_enabled(a)) {
                    std::string colored; std::uint64_t cur=line.begin;
                    for(const Match* mp:line_matches[i]){auto sb=std::max<std::uint64_t>(mp->start,line.begin),se=std::min<std::uint64_t>(mp->end,line.term_end);if(se<=sb||sb<cur)continue;colored.append(data.substr(cur,sb-cur));colored+=ansi_match(data.substr(sb,se-sb),a);cur=se;}colored.append(data.substr(cur,line.term_end-cur));rendered=std::move(colored);
                }

                if (a.trim) {
                    auto cut = rendered.find_first_not_of(" \t");
                    if (cut != std::string::npos) rendered.erase(0, cut);
                    else rendered.clear();
                }
                std::size_t content_len = rendered.size();
                if (!rendered.empty() && (rendered.back()=='\n' || rendered.back()=='\0')) --content_len;
                if (a.max_columns > 0 && content_len > static_cast<std::size_t>(a.max_columns)) {
                    if (!a.max_columns_preview) { rendered = "[Omitted long matching line]"; rendered += a.null_data?'\0':'\n'; }
                    else { rendered.resize(static_cast<std::size_t>(a.max_columns)); rendered += a.null_data?'\0':'\n'; }
                }

                const std::string& sep = match_line ? a.field_match_sep : a.field_context_sep;
                if (show_filename && !heading) {
                    std::cout << ansi_path(display_path(idx->files()[fid].path, a),a);
                    if (a.null_out) std::cout.put('\0'); else std::cout << sep;
                }
                if (a.line_number) std::cout << ansi_line(std::to_string(i+1),a) << sep;
                if (a.column && match_line && !line_matches[i].empty())
                    std::cout << ansi_column(std::to_string(line_matches[i].front()->start-line.begin+1),a) << sep;
                if (a.byte_offset) std::cout << line.begin << sep;
                std::cout << rendered;
                if (rendered.empty() || (rendered.back()!='\n' && rendered.back()!='\0')) std::cout << (a.null_data?'\0':'\n');
                any |= match_line;
            }
            if(!binary_notice.empty()){std::cout<<binary_notice<<'\n';any=true;}
            if (heading && any) std::cout.flush();
        }

        if (a.json) {
            auto dt = std::chrono::steady_clock::now() - start;
            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count();
            auto secs = ns / 1000000000LL, nanos = ns % 1000000000LL;
            std::ostringstream human; human << std::fixed << std::setprecision(6) << std::chrono::duration<double>(dt).count() << 's';
            std::cout << "{\"type\":\"summary\",\"data\":{\"elapsed_total\":{\"human\":\"" << human.str()
                      << "\",\"nanos\":" << nanos << ",\"secs\":" << secs << "},\"stats\":{\"elapsed\":{\"human\":\"" << human.str()
                      << "\",\"nanos\":" << nanos << ",\"secs\":" << secs << "},\"searches\":" << json_searches
                      << ",\"searches_with_match\":" << json_searches_with_match << ",\"bytes_searched\":" << json_bytes
                      << ",\"bytes_printed\":" << json_bytes_printed << ",\"matched_lines\":" << json_matched_lines << ",\"matches\":" << json_matches << "}}}\n";
        }

        if (a.stats && !a.files_with && !a.files_without && !a.count && !a.count_matches && !a.files_mode) {
            auto dt=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
            // BF-3 audit: stats must reflect the final selected lines after invert
            // (OR-then-invert) and per-file max-count truncation, not the raw
            // positive union in byfile. Ripgrep's --stats counts one "match"
            // per selected line in invert mode, and per underlying Match in
            // non-invert mode (filtered to selected lines, thus respecting
            // --max-count). bytes_printed / matched_lines are always derived
            // from selected lines; files_contained is selected_count!=0.
            // CLI forces core_opt.invert_match=false and merges patterns with
            // OR, then applies `selected = !matched` once — this is correct
            // rg parity (NOT (A OR B)). Doubling inversion (library invert +
            // CLI invert) is prevented. --max-count is per-file, matching rg;
            // library SearchOptions::max_matches is global per find() call and
            // is disabled here (core_opt.max_matches=0) so CLI enforces the
            // per-file semantics explicitly.
            std::uint64_t fsrch=0,fmatch=0,mlines=0,mcnt=0,bsearch=0,bprinted=0;
            for (std::size_t fid=0; fid<allowed.size(); ++fid) if(allowed[fid]) {
                ++fsrch; bsearch+=idx->files()[fid].size;
                std::string data(idx->content(fid));
                auto ls=split_lines(data, a.null_data?'\0':'\n');
                auto& ms = byfile[fid];
                auto line_matches = matches_by_line(ls, ms);
                std::vector<std::uint8_t> selected(ls.size(), 0);
                for (std::size_t i=0;i<ls.size();++i) {
                    bool matched = !line_matches[i].empty();
                    selected[i] = static_cast<std::uint8_t>(a.sopt.invert_match ? !matched : matched);
                }
                if (a.stop_on_nonmatch) {
                    bool seen=false;
                    for (std::size_t i=0;i<selected.size();++i) {
                        if (selected[i]) { seen=true; continue; }
                        if (seen) { std::fill(selected.begin()+static_cast<std::ptrdiff_t>(i), selected.end(), 0); break; }
                    }
                }
                if (a.max_count_set) {
                    std::uint64_t seen=0;
                    for (auto& bit: selected) if(bit){ if(seen>=a.max_count) bit=0; else ++seen; }
                }
                std::uint64_t selected_count = std::count(selected.begin(), selected.end(), std::uint8_t{1});
                if (selected_count==0) continue;
                ++fmatch;
                mlines += selected_count;
                if (a.sopt.invert_match) {
                    mcnt += selected_count;
                    for (std::size_t i=0;i<ls.size();++i) if(selected[i]) bprinted += (ls[i].term_end - ls[i].begin);
                } else {
                    // Count only matches whose line survived max-count / stop-on-nonmatch filtering
                    std::uint64_t file_matches=0;
                    for (auto& m: ms) {
                        auto li=line_for_offset(ls, m.start);
                        if (li < selected.size() && selected[li]) ++file_matches;
                    }
                    mcnt += file_matches;
                    for (std::size_t i=0;i<ls.size();++i) if(selected[i]) bprinted += (ls[i].term_end - ls[i].begin);
                }
            }
            std::cout << mcnt << " matches\n" << mlines << " matched lines\n" << fmatch << " files contained matches\n" << fsrch << " files searched\n" << bprinted << " bytes printed\n" << bsearch << " bytes searched\n" << std::fixed << std::setprecision(6) << dt << " seconds spent searching\n";
        }
        return any ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "pergrep: " << e.what() << "\n";
        return 2;
    }
}
