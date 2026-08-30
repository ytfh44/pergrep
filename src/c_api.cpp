#include "pergrep/pergrep_c.h"
#include "pergrep/pergrep.hpp"
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>

struct pg_index { std::shared_ptr<pergrep::Index> value; };
struct pg_pattern { pergrep::Pattern value; };
struct pg_searcher { std::unique_ptr<pergrep::Searcher> value; };

namespace {
void set_error(char** out, const char* what) {
    if (!out) return;
    *out = nullptr;
    if (!what) return;
    auto n = std::strlen(what) + 1;
    auto p = static_cast<char*>(std::malloc(n));
    if (!p) return;
    std::memcpy(p, what, n);
    *out = p;
}

template<class F, class R>
R guarded(char** error, R fallback, F&& f) {
    if (error) *error = nullptr;
    try { return f(); }
    catch (const std::exception& e) { set_error(error, e.what()); return fallback; }
    catch (...) { set_error(error, "unknown libpergrep error"); return fallback; }
}

pergrep::PatternOptions convert(const pg_pattern_options* x) {
    auto o = pergrep::PatternOptions{};
    if (!x) return o;
    o.kind = x->kind == PG_FIXED ? pergrep::PatternKind::Fixed : pergrep::PatternKind::Regex;
    o.case_mode = x->case_mode == PG_CASE_INSENSITIVE ? pergrep::CaseMode::Insensitive : x->case_mode == PG_CASE_SMART ? pergrep::CaseMode::Smart : pergrep::CaseMode::Sensitive;
    o.engine = x->engine == PG_ENGINE_PCRE2_COMPAT ? pergrep::Engine::Pcre2Compat : x->engine == PG_ENGINE_AUTO ? pergrep::Engine::Auto : pergrep::Engine::Default;
    o.word=x->word; o.line=x->line; o.multiline=x->multiline; o.dotall=x->dotall; o.unicode=x->unicode; o.crlf=x->crlf;
    return o;
}
pergrep::IndexOptions convert(const pg_index_options* x) {
    auto o = pergrep::IndexOptions{}; if(!x) return o;
    o.chunk_bytes = x->chunk_bytes;
    o.chunk_overlap = x->chunk_overlap;
    o.positional_block_bytes = x->positional_block_bytes;
    o.positional_budget_ratio = x->positional_budget_ratio;
    o.planned_qgrams = x->planned_qgrams;
    o.follow_symlinks = x->follow_symlinks;
    // BF-1 audit: pg_index_options has no include_hidden field (C ABI). Keep
    // default include_hidden=true for backward compat. If a future version adds
    // include_hidden as a trailing int, detect via struct size / extra field
    // and map it here to avoid breaking existing callers.
    return o;
}
pergrep::SearchOptions convert(const pg_search_options* x) {
    auto o=pergrep::SearchOptions{}; if(!x)return o;
    o.overlapping=x->overlapping; o.include_binary=x->include_binary; o.max_matches=x->max_matches; o.record_separator=x->record_separator; return o;
}
}

extern "C" {
pg_pattern_options pg_pattern_options_default(void){ return {PG_REGEX,PG_CASE_SENSITIVE,PG_ENGINE_DEFAULT,0,0,0,0,1,0}; }
pg_index_options pg_index_options_default(void){ auto o=pergrep::IndexOptions{}; return {o.chunk_bytes,o.chunk_overlap,o.positional_block_bytes,o.positional_budget_ratio,o.planned_qgrams,0}; }
pg_search_options pg_search_options_default(void){ return {0,0,0,'\n'}; }

pg_index* pg_index_build(const char* root, const pg_index_options* options, char** error) {
    if (!root) { set_error(error, "root is null"); return nullptr; }
    return guarded(error, (pg_index*)nullptr, [&] {
        auto idx = pergrep::Index::build(root, convert(options));
        auto p = std::make_unique<pg_index>();
        p->value = std::make_shared<pergrep::Index>(std::move(idx));
        return p.release();
    });
}
pg_index* pg_index_load(const char* file, char** error) {
    if (!file) { set_error(error, "file is null"); return nullptr; }
    return guarded(error, (pg_index*)nullptr, [&] {
        auto idx = pergrep::Index::load(file);
        auto p = std::make_unique<pg_index>();
        p->value = std::make_shared<pergrep::Index>(std::move(idx));
        return p.release();
    });
}
int pg_index_save(const pg_index* index, const char* file, char** error) {
    if (!index || !index->value || !file) { set_error(error, "index/file is null"); return 0; }
    return guarded(error, 0, [&] { index->value->save(file); return 1; });
}
void pg_index_free(pg_index* p) { delete p; }
uint64_t pg_index_corpus_bytes(const pg_index* p) { return (p && p->value) ? p->value->corpus_bytes() : 0; }
uint64_t pg_index_bytes(const pg_index* p) { return (p && p->value) ? p->value->index_bytes() : 0; }
size_t pg_index_file_count(const pg_index* p) { return (p && p->value) ? p->value->files().size() : 0; }
const char* pg_index_file_path(const pg_index* p, size_t id) {
    if (!p || !p->value || id >= p->value->files().size()) return nullptr;
    return p->value->files()[id].path.c_str();
}
int pg_index_is_snapshot(const pg_index* p) {
    return (p && p->value && p->value->is_snapshot()) ? 1 : 0;
}

pg_pattern* pg_pattern_compile(const char* expr, const pg_pattern_options* options, char** error) {
    if (!expr) { set_error(error, "expression is null"); return nullptr; }
    return guarded(error, (pg_pattern*)nullptr, [&] {
        auto pat = pergrep::Pattern::compile(expr, convert(options));
        auto p = std::make_unique<pg_pattern>();
        p->value = std::move(pat);
        return p.release();
    });
}
void pg_pattern_free(pg_pattern* p) { delete p; }
pg_searcher* pg_searcher_new(const pg_index* idx, char** error) {
    if (!idx || !idx->value) { set_error(error, "index is null"); return nullptr; }
    return guarded(error, (pg_searcher*)nullptr, [&] {
        auto s = std::make_unique<pergrep::Searcher>(idx->value);
        auto p = std::make_unique<pg_searcher>();
        p->value = std::move(s);
        return p.release();
    });
}
void pg_searcher_free(pg_searcher* p) { delete p; }
pg_match* pg_search(pg_searcher* s, const pg_pattern* p, const pg_search_options* options, size_t* count, pg_search_stats* stats, char** error) {
    if (count) *count = 0;
    if (!s || !s->value || !p) { set_error(error, "searcher/pattern is null"); return nullptr; }
    return guarded(error, (pg_match*)nullptr, [&] {
        pergrep::SearchStats st;
        auto v = s->value->find(p->value, convert(options), &st);
        pg_match* out = nullptr;
        if (!v.empty()) {
            out = static_cast<pg_match*>(std::malloc(sizeof(pg_match) * v.size()));
            if (!out) throw std::bad_alloc{};
            for (size_t i = 0; i < v.size(); ++i) out[i] = {v[i].file_id, v[i].start, v[i].end};
        }
        if (count) *count = v.size();
        if (stats) *stats = {st.candidate_chunks, st.candidate_blocks, st.verified_bytes, st.matches};
        return out;
    });
}
void pg_matches_free(pg_match* p){std::free(p);}
void pg_error_free(char* p){std::free(p);}
const char* pg_version(void){static const std::string v=pergrep::version();return v.c_str();}
}
