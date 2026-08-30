#ifndef PERGREP_C_H
#define PERGREP_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pg_index pg_index;
typedef struct pg_pattern pg_pattern;
typedef struct pg_searcher pg_searcher;

typedef enum pg_pattern_kind { PG_REGEX = 0, PG_FIXED = 1 } pg_pattern_kind;
typedef enum pg_case_mode { PG_CASE_SENSITIVE = 0, PG_CASE_INSENSITIVE = 1, PG_CASE_SMART = 2 } pg_case_mode;
typedef enum pg_engine { PG_ENGINE_DEFAULT = 0, PG_ENGINE_PCRE2_COMPAT = 1, PG_ENGINE_AUTO = 2 } pg_engine;

typedef struct pg_pattern_options {
    pg_pattern_kind kind;
    pg_case_mode case_mode;
    pg_engine engine;
    int word;
    int line;
    int multiline;
    int dotall;
    int unicode;
    int crlf;
} pg_pattern_options;

typedef struct pg_index_options {
    size_t chunk_bytes;
    size_t chunk_overlap;
    size_t positional_block_bytes;
    double positional_budget_ratio;
    size_t planned_qgrams;
    int follow_symlinks;
} pg_index_options;

typedef struct pg_search_options {
    int overlapping;
    int include_binary;
    uint64_t max_matches;
    unsigned char record_separator;
} pg_search_options;

typedef struct pg_match {
    uint32_t file_id;
    uint64_t start;
    uint64_t end;
} pg_match;

typedef struct pg_search_stats {
    uint64_t candidate_chunks;
    uint64_t candidate_blocks;
    uint64_t verified_bytes;
    uint64_t matches;
} pg_search_stats;

pg_pattern_options pg_pattern_options_default(void);
pg_index_options pg_index_options_default(void);
pg_search_options pg_search_options_default(void);

pg_index* pg_index_build(const char* root, const pg_index_options* options, char** error);
pg_index* pg_index_load(const char* file, char** error);
int pg_index_save(const pg_index* index, const char* file, char** error);
void pg_index_free(pg_index* index);
uint64_t pg_index_corpus_bytes(const pg_index* index);
uint64_t pg_index_bytes(const pg_index* index);
size_t pg_index_file_count(const pg_index* index);
const char* pg_index_file_path(const pg_index* index, size_t file_id);
int pg_index_is_snapshot(const pg_index* index);

pg_pattern* pg_pattern_compile(const char* expression, const pg_pattern_options* options, char** error);
void pg_pattern_free(pg_pattern* pattern);

pg_searcher* pg_searcher_new(const pg_index* index, char** error);
void pg_searcher_free(pg_searcher* searcher);
pg_match* pg_search(pg_searcher* searcher, const pg_pattern* pattern,
                    const pg_search_options* options, size_t* count,
                    pg_search_stats* stats, char** error);
void pg_matches_free(pg_match* matches);
void pg_error_free(char* error);

const char* pg_version(void);

#ifdef __cplusplus
}
#endif
#endif
