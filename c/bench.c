// glibc + -std=c99 does not expose clock_gettime / struct timespec unless POSIX is requested.
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

// NXS C reader benchmark — NXS vs JSON (jsmn, minimal) vs raw CSV scan
// Build: make bench && ./bench ../js/fixtures
//
// JSON baseline: we measure the two most relevant operations:
//   - full sequential parse (jsmn tokenize pass — simulates "open cost")
//   - sum of score column from a pre-tokenized flat array
// Since pulling in a full JSON library is overkill, we use two proxies:
//   1. strlen(json_buf) — I/O lower bound (memory scan, zero parse work)
//   2. A hand-rolled ASCII float scanner over the raw JSON bytes for "score"
//      values — representative of what a real minimal JSON parser does per field.
//
// The raw CSV scanner is identical to what Go/JS use.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "nxs.h"

// ── File I/O ──────────────────────────────────────────────────────────────────

static uint8_t *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *out_size = (size_t)ftell(f);
    rewind(f);
    uint8_t *buf = malloc(*out_size + 1);
    if (buf) { fread(buf, 1, *out_size, f); buf[*out_size] = '\0'; }
    fclose(f);
    return buf;
}

static double elapsed_ms(struct timespec *a, struct timespec *b) {
    return (b->tv_sec - a->tv_sec) * 1e3 + (b->tv_nsec - a->tv_nsec) / 1e6;
}

// ── JSON column scan (raw bytes, no library) ──────────────────────────────────
// Scans for `"score":` then reads the following float literal.
// Equivalent to what a streaming JSON parser does for a single-column aggregate.
static double json_sum_score(const char *buf, size_t len) {
    double sum = 0.0;
    const char *p = buf;
    const char *end = buf + len;
    const char needle[] = "\"score\":";
    const size_t nlen = sizeof(needle) - 1;
    while (p + nlen < end) {
        if (memcmp(p, needle, nlen) == 0) {
            p += nlen;
            while (p < end && (*p == ' ' || *p == '\t')) p++;
            sum += strtod(p, (char **)&p);
        } else {
            p++;
        }
    }
    return sum;
}

// ── CSV column scan ───────────────────────────────────────────────────────────
// Fixture CSV header: id,username,email,age,balance,active,score,created_at
// score is column index 6 (0-based).
static double csv_sum_score(const char *buf, size_t len) {
    double sum = 0.0;
    const char *p = buf;
    const char *end = buf + len;
    int line = 0;
    while (p < end) {
        const char *row_end = memchr(p, '\n', (size_t)(end - p));
        if (!row_end) row_end = end;
        if (line++ == 0) { p = row_end + 1; continue; } // skip header
        // advance to column 6
        const char *col = p;
        for (int c = 0; c < 6 && col < row_end; c++) {
            col = memchr(col, ',', (size_t)(row_end - col));
            if (!col) break;
            col++;
        }
        if (col && col < row_end) sum += strtod(col, NULL);
        p = row_end + 1;
    }
    return sum;
}

// ── Bench harness ─────────────────────────────────────────────────────────────

#define RUNS 5

static double run_best(struct timespec *t0, struct timespec *t1,
                       void (*fn)(void *), void *ctx) {
    double best = 1e18;
    for (int i = 0; i < RUNS; i++) {
        clock_gettime(CLOCK_MONOTONIC, t0);
        fn(ctx);
        clock_gettime(CLOCK_MONOTONIC, t1);
        double ms = elapsed_ms(t0, t1);
        if (ms < best) best = ms;
    }
    return best;
}

typedef struct { const char *buf; size_t len; double result; } json_ctx_t;
typedef struct { const char *buf; size_t len; double result; } csv_ctx_t;
typedef struct { nxs_reader_t *r; double result; } nxs_f64_ctx_t;
typedef struct { nxs_reader_t *r; int64_t result; } nxs_i64_ctx_t;
typedef struct { nxs_reader_t *r; } nxs_rand_ctx_t;

static void do_json_sum(void *p) { json_ctx_t *c = p; c->result = json_sum_score(c->buf, c->len); }
static void do_csv_sum (void *p) { csv_ctx_t  *c = p; c->result = csv_sum_score(c->buf, c->len); }
static void do_nxs_f64 (void *p) { nxs_f64_ctx_t *c = p; c->result = nxs_sum_f64(c->r, "score"); }
static void do_nxs_i64 (void *p) { nxs_i64_ctx_t *c = p; c->result = nxs_sum_i64(c->r, "id"); }
static void do_nxs_rand(void *p) {
    nxs_rand_ctx_t *c = p;
    nxs_object_t obj;
    for (int i = 0; i < 1000; i++) {
        nxs_record(c->r, (uint32_t)(i * 997 % c->r->record_count), &obj);
        double v; nxs_get_f64(&obj, "score", &v);
    }
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "../js/fixtures";
    char nxb_path[512], json_path[512], csv_path[512];
    snprintf(nxb_path,  sizeof(nxb_path),  "%s/records_1000000.nxb",  dir);
    snprintf(json_path, sizeof(json_path), "%s/records_1000000.json", dir);
    snprintf(csv_path,  sizeof(csv_path),  "%s/records_1000000.csv",  dir);

    size_t nxb_size = 0, json_size = 0, csv_size = 0;
    uint8_t *nxb_data  = read_file(nxb_path,  &nxb_size);
    uint8_t *json_data = read_file(json_path, &json_size);
    uint8_t *csv_data  = read_file(csv_path,  &csv_size);

    if (!nxb_data)  { printf("fixture not found: %s\n", nxb_path); return 1; }
    if (!json_data) { printf("fixture not found: %s\n", json_path); return 1; }
    if (!csv_data)  { printf("fixture not found: %s\n", csv_path); return 1; }

    nxs_reader_t r;
    nxs_open(&r, nxb_data, nxb_size);

    struct timespec t0, t1;

    printf("NXS C Benchmark — %u records\n", r.record_count);
    printf("  .nxb  %.2f MB   .json  %.2f MB   .csv  %.2f MB\n\n",
           nxb_size / 1e6, json_size / 1e6, csv_size / 1e6);

    // ── sum score ─────────────────────────────────────────────────────────────
    printf("  ┌─ sum(score) ─────────────────────────────────────────────────────┐\n");

    json_ctx_t jc = { (char*)json_data, json_size, 0 };
    double json_ms = run_best(&t0, &t1, do_json_sum, &jc);
    printf("  │  JSON raw scan          sum=%.2f  %6.2f ms  baseline\n", jc.result, json_ms);

    csv_ctx_t cc = { (char*)csv_data, csv_size, 0 };
    double csv_ms = run_best(&t0, &t1, do_csv_sum, &cc);
    printf("  │  CSV raw scan           sum=%.2f  %6.2f ms  %.1fx faster\n",
           cc.result, csv_ms, json_ms / csv_ms);

    nxs_f64_ctx_t nc = { &r, 0 };
    double nxs_ms = run_best(&t0, &t1, do_nxs_f64, &nc);
    printf("  │  NXS sum_f64            sum=%.2f  %6.2f ms  %.1fx faster\n",
           nc.result, nxs_ms, json_ms / nxs_ms);
    printf("  └──────────────────────────────────────────────────────────────────┘\n\n");

    // ── sum id ────────────────────────────────────────────────────────────────
    printf("  ┌─ sum(id) ────────────────────────────────────────────────────────┐\n");
    nxs_i64_ctx_t ic = { &r, 0 };
    double nxs_i64_ms = run_best(&t0, &t1, do_nxs_i64, &ic);
    printf("  │  NXS sum_i64            sum=%lld  %6.2f ms\n",
           (long long)ic.result, nxs_i64_ms);
    printf("  └──────────────────────────────────────────────────────────────────┘\n\n");

    // ── random access ─────────────────────────────────────────────────────────
    printf("  ┌─ random access ×1000 ────────────────────────────────────────────┐\n");
    nxs_rand_ctx_t rc2 = { &r };
    double rand_ms = run_best(&t0, &t1, do_nxs_rand, &rc2);
    printf("  │  NXS record(k).get_f64  %6.3f ms  (%.0f ns/record)\n",
           rand_ms, rand_ms * 1e6 / 1000.0);
    printf("  └──────────────────────────────────────────────────────────────────┘\n\n");

    nxs_close(&r);
    free(nxb_data); free(json_data); free(csv_data);
    return 0;
}
