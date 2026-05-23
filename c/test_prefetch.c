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
            CHECK("cache_stats strategy is lazy", stats.strategy && strcmp(stats.strategy, "lazy") == 0);
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
