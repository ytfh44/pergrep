#include <pergrep/pergrep_c.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void){
    assert(pg_version() != NULL);

    // pg_index_build error handling without leaks
    {
        char* err = NULL;
        pg_index* idx = pg_index_build(NULL, NULL, &err);
        assert(idx == NULL);
        assert(err != NULL);
        pg_error_free(err);

        err = NULL;
        idx = pg_index_build("nonexistent_directory_for_test_12345", NULL, &err);
        assert(idx == NULL);
        assert(err != NULL);
        pg_error_free(err);

        // Invalid chunk_bytes (< 64)
        pg_index_options bad_opt = pg_index_options_default();
        bad_opt.chunk_bytes = 10;
        err = NULL;
        idx = pg_index_build(".", &bad_opt, &err);
        assert(idx == NULL);
        assert(err != NULL);
        pg_error_free(err);

        // Invalid positional_block_bytes (< 16)
        bad_opt = pg_index_options_default();
        bad_opt.positional_block_bytes = 8;
        err = NULL;
        idx = pg_index_build(".", &bad_opt, &err);
        assert(idx == NULL);
        assert(err != NULL);
        pg_error_free(err);

        // Invalid planned_qgrams (< 1)
        bad_opt = pg_index_options_default();
        bad_opt.planned_qgrams = 0;
        err = NULL;
        idx = pg_index_build(".", &bad_opt, &err);
        assert(idx == NULL);
        assert(err != NULL);
        pg_error_free(err);
    }

    // pg_index_load error handling
    {
        char* err = NULL;
        pg_index* idx = pg_index_load(NULL, &err);
        assert(idx == NULL);
        assert(err != NULL);
        pg_error_free(err);

        err = NULL;
        idx = pg_index_load("nonexistent_file_12345.bin", &err);
        assert(idx == NULL);
        assert(err != NULL);
        pg_error_free(err);
    }

    // pg_pattern_compile error handling
    {
        char* err = NULL;
        pg_pattern* pat = pg_pattern_compile(NULL, NULL, &err);
        assert(pat == NULL);
        assert(err != NULL);
        pg_error_free(err);

        err = NULL;
        pat = pg_pattern_compile("(unclosed_group", NULL, &err);
        assert(pat == NULL);
        assert(err != NULL);
        pg_error_free(err);
    }

    // pg_searcher_new error handling
    {
        char* err = NULL;
        pg_searcher* searcher = pg_searcher_new(NULL, &err);
        assert(searcher == NULL);
        assert(err != NULL);
        pg_error_free(err);
    }

    // pg_search error handling
    {
        char* err = NULL;
        size_t count = 0;
        pg_match* m = pg_search(NULL, NULL, NULL, &count, NULL, &err);
        assert(m == NULL);
        assert(err != NULL);
        pg_error_free(err);
    }

    // Normal C API search with pg_search_options
    {
        char* err = NULL;
        pg_index_options iopt = pg_index_options_default();
        pg_index* idx = pg_index_build(".", &iopt, &err);
        assert(idx != NULL);
        assert(err == NULL);
        assert(pg_index_file_count(idx) > 0);
        assert(pg_index_corpus_bytes(idx) > 0);
        assert(pg_index_bytes(idx) > 0);
        assert(pg_index_file_path(idx, 0) != NULL);
        assert(pg_index_file_path(idx, 999999) == NULL);

        pg_pattern_options popt = pg_pattern_options_default();
        popt.kind = PG_FIXED;
        pg_pattern* pat = pg_pattern_compile("pergrep", &popt, &err);
        assert(pat != NULL);
        assert(err == NULL);

        pg_searcher* searcher = pg_searcher_new(idx, &err);
        assert(searcher != NULL);
        assert(err == NULL);

        pg_search_options sopt = pg_search_options_default();
        sopt.max_matches = 5;
        size_t count = 0;
        pg_search_stats stats = {0};
        pg_match* matches = pg_search(searcher, pat, &sopt, &count, &stats, &err);
        assert(err == NULL);
        assert(count <= 5);
        if (matches) pg_matches_free(matches);

        pg_searcher_free(searcher);
        pg_pattern_free(pat);
        pg_index_free(idx);
    }

    printf("ok\n");
    return 0;
}
