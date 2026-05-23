// Prefetch unit tests — build: make test-prefetch && ./test_prefetch
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "nxs.h"
#include "nxs_prefetch.h"
#include "nxs_writer.h"

static int passed = 0, failed = 0;

#define CHECK(name, expr) do { \
    if (expr) { printf("  ✓ %s\n", name); passed++; } \
    else      { printf("  ✗ %s\n", name); failed++; } \
} while (0)

static uint8_t *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *out_size = (size_t)ftell(f);
    rewind(f);
    uint8_t *buf = malloc(*out_size);
    if (buf) fread(buf, 1, *out_size, f);
    fclose(f);
    return buf;
}

static uint8_t *build_compact_records(size_t n, size_t *out_size) {
    const char *keys[] = {"id", "tag"};
    nxs_writer_t w;
    nxs_writer_init(&w, keys, 2, 65536);
    for (size_t i = 0; i < n; i++) {
        char tag[16];
        snprintf(tag, sizeof(tag), "r%zu", i);
        nxs_writer_begin_object(&w);
        nxs_write_i64(&w, 0, (int64_t)i);
        nxs_write_str(&w, 1, tag, (uint32_t)strlen(tag));
        nxs_writer_end_object(&w);
    }
    nxs_writer_finish(&w);
    *out_size = w.out_size;
    uint8_t *buf = malloc(w.out_size);
    if (buf) memcpy(buf, w.out, w.out_size);
    nxs_writer_free(&w);
    return buf;
}

static uint8_t *build_records(size_t n, size_t *out_size) {
    const char *keys[] = {"id", "username", "score", "active"};
    nxs_writer_t w;
    nxs_writer_init(&w, keys, 4, 65536);
    for (size_t i = 0; i < n; i++) {
        char uname[32];
        snprintf(uname, sizeof(uname), "user_%zu", i);
        nxs_writer_begin_object(&w);
        nxs_write_i64(&w, 0, (int64_t)i);
        nxs_write_str(&w, 1, uname, (uint32_t)strlen(uname));
        nxs_write_f64(&w, 2, (double)i * 0.25);
        nxs_write_bool(&w, 3, (int)(i % 2));
        nxs_writer_end_object(&w);
    }
    nxs_writer_finish(&w);
    *out_size = w.out_size;
    uint8_t *buf = malloc(w.out_size);
    if (buf) memcpy(buf, w.out, w.out_size);
    nxs_writer_free(&w);
    return buf;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("\nNXS C Prefetch — Tests\n\n");

    /* coalescePageIndices [3,4,6,7,12] gap=1 → 3 ranges */
    {
        uint32_t pages[] = {3, 4, 6, 7, 12};
        nxs_page_range_t ranges[8];
        size_t n = nxs_coalesce_page_indices(pages, 5, 1, NXS_PREFETCH_DEFAULT_PAGE_SIZE,
                                             ranges, 8);
        CHECK("coalesce [3,4,6,7,12] gap=1 → 3 ranges", n == 3);
        CHECK("coalesce range 0 is pages 3–4",
              n >= 1 && ranges[0].page_start == 3 && ranges[0].page_end == 4);
        CHECK("coalesce range 1 is pages 6–7",
              n >= 2 && ranges[1].page_start == 6 && ranges[1].page_end == 7);
        CHECK("coalesce range 2 is page 12",
              n >= 3 && ranges[2].page_start == 12 && ranges[2].page_end == 12);
    }

    /* LRU eviction at max_pages=2 */
    {
        size_t sz = 0;
        uint8_t *buf = build_records(20, &sz);
        nxs_open_options_t opts;
        nxs_open_options_init(&opts);
        opts.max_pages = 2;
        opts.page_size = 256;
        opts.coalesce_gap_pages = 0;

        nxs_reader_t r;
        CHECK("prefetch LRU: open_ex ok", nxs_open_ex(&r, buf, sz, &opts) == NXS_OK);
        CHECK("prefetch LRU: viewport(0,0) ok", nxs_prefetch_viewport(&r, 0, 0) == NXS_OK);
        CHECK("prefetch LRU: viewport(19,19) ok", nxs_prefetch_viewport(&r, 19, 19) == NXS_OK);

        nxs_cache_stats_t stats;
        nxs_cache_stats(&r, &stats);
        CHECK("prefetch LRU: pages_cached <= max_pages", stats.pages_cached <= 2);
        nxs_close(&r);
        free(buf);
    }

    /* prefetch_viewport issues ≤3 fetches for 50 records */
    {
        size_t sz = 0;
        uint8_t *buf = build_records(60, &sz);
        nxs_open_options_t opts;
        nxs_open_options_init(&opts);
        opts.max_pages = 64;
        opts.coalesce_gap_pages = 1;

        nxs_reader_t r;
        nxs_open_ex(&r, buf, sz, &opts);
        nxs_prefetch_viewport(&r, 0, 49);

        nxs_cache_stats_t stats;
        nxs_cache_stats(&r, &stats);
        CHECK("prefetch_viewport ≤3 fetches for 50 records", stats.fetches_issued <= 3);
        CHECK("prefetch_viewport fetches_issued > 0", stats.fetches_issued > 0);

        nxs_object_t obj;
        int64_t id = -1;
        nxs_record(&r, 49, &obj);
        CHECK("prefetch_viewport_basic: record(49) readable",
              nxs_get_i64(&obj, "id", &id) == NXS_OK && id == 49);

        nxs_close(&r);
        free(buf);
    }

    /* open options defaults */
    {
        nxs_open_options_t opts;
        nxs_open_options_init(&opts);
        CHECK("open_options default max_pages", opts.max_pages == 128);
        CHECK("open_options default page_size", opts.page_size == 65536);
        CHECK("open_options default coalesce_gap", opts.coalesce_gap_pages == 1);
        CHECK("open_options default prefetch_depth", opts.prefetch_depth == 4);
    }

    {
        nxs_access_pattern_detector_t d;
        nxs_pattern_detector_init(&d);
        for (uint32_t i = 0; i < 8; i++)
            nxs_pattern_detector_observe(&d, i);
        CHECK("pattern unknown until 9th obs",
              nxs_pattern_detector_pattern(&d) == NXS_PATTERN_UNKNOWN);
        for (uint32_t i = 8; i < 20; i++)
            nxs_pattern_detector_observe(&d, i);
        CHECK("pattern sequential after small deltas",
              nxs_pattern_detector_pattern(&d) == NXS_PATTERN_SEQUENTIAL);
        uint32_t next[8];
        size_t nn = nxs_pattern_detector_predict_next(&d, 4, 100, next, 8);
        CHECK("predict_next returns 4 indices", nn == 4);
        CHECK("predict_next starts at 20", nn > 0 && next[0] == 20);
    }

    {
        size_t sz = 0;
        uint8_t *buf = build_compact_records(200, &sz);
        nxs_reader_t r;
        CHECK("sequential upgrade: open_ex", nxs_open_ex(&r, buf, sz, NULL) == NXS_OK);
        nxs_object_t obj;
        for (uint32_t i = 0; i < 150; i++)
            nxs_record(&r, i, &obj);
        nxs_warmup(&r);
        nxs_cache_stats_t stats;
        nxs_cache_stats(&r, &stats);
        CHECK("sequential upgrade: strategy eager",
              stats.strategy && strcmp(stats.strategy, "eager") == 0);
        CHECK("sequential upgrade: pattern sequential",
              stats.pattern && strcmp(stats.pattern, "sequential") == 0);
        nxs_close(&r);
        free(buf);
    }

    {
        size_t sz = 0;
        uint8_t *buf = build_compact_records(200, &sz);
        CHECK("hint full: file under 10MB", sz <= 10u * 1024u * 1024u);
        nxs_open_options_t opts;
        nxs_open_options_init(&opts);
        opts.hint = NXS_HINT_FULL;
        nxs_reader_t r;
        CHECK("hint full: open_ex", nxs_open_ex(&r, buf, sz, &opts) == NXS_OK);
        nxs_warmup(&r);
        nxs_cache_stats_t stats;
        nxs_cache_stats(&r, &stats);
        CHECK("hint full: strategy eager at open",
              stats.strategy && strcmp(stats.strategy, "eager") == 0);
        nxs_close(&r);
        free(buf);
    }

    /* cache_stats on fixture file */
    {
        const char *dir = "../js/fixtures";
        char path[512];
        snprintf(path, sizeof(path), "%s/records_1000.nxb", dir);
        size_t sz = 0;
        uint8_t *buf = read_file(path, &sz);
        if (buf) {
            nxs_reader_t r;
            nxs_open_ex(&r, buf, sz, NULL);
            nxs_prefetch_viewport(&r, 0, 49);
            nxs_cache_stats_t stats;
            nxs_cache_stats(&r, &stats);
            CHECK("cache_stats pattern unknown before access",
                  stats.pattern && strcmp(stats.pattern, "unknown") == 0);
            CHECK("cache_stats pages_max is 128", stats.pages_max == 128);
            CHECK("cache_stats memory_used_bytes > 0", stats.memory_used_bytes > 0);
            nxs_close(&r);
            free(buf);
        } else {
            printf("  - records_1000.nxb missing; skipping fixture prefetch test\n");
        }
    }

    printf("\n%d passed, %d failed\n\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
