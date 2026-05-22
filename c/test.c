// NXS C reader + writer smoke tests
// Build: cc -std=c99 -O2 -o test test.c nxs.c nxs_writer.c -lm && ./test ../js/fixtures
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "nxs.h"
#include "nxs_writer.h"

// Minimal JSON parser for the fixture — just enough to validate numbers/strings.
// We read the JSON ourselves rather than pulling in a library.
typedef struct { int64_t id; double score; int active; char username[64]; } Record;

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

static int passed = 0, failed = 0;

static uint32_t rd_u32_le(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

#define CHECK(name, expr) do { \
    if (expr) { printf("  ✓ %s\n", name); passed++; } \
    else      { printf("  ✗ %s\n", name); failed++; } \
} while(0)

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "../js/fixtures";
    char nxb_path[512], json_path[512];
    snprintf(nxb_path,  sizeof(nxb_path),  "%s/records_1000.nxb",  dir);
    snprintf(json_path, sizeof(json_path), "%s/records_1000.json", dir);

    size_t nxb_size = 0;
    uint8_t *nxb_data = read_file(nxb_path, &nxb_size);
    if (!nxb_data) {
        printf("fixtures not found at %s\n", dir);
        printf("generate them: cargo run --release --bin gen_fixtures -- js/fixtures\n");
        return 1;
    }

    printf("\nNXS C Reader — Tests\n\n");

    nxs_reader_t r;
    nxs_err_t err = nxs_open(&r, nxb_data, nxb_size);
    CHECK("opens without error", err == NXS_OK);
    CHECK("reads correct record count", r.record_count == 1000);

    int has_id = 0, has_username = 0, has_score = 0;
    for (int i = 0; i < r.key_count; i++) {
        if (strcmp(r.keys[i], "id")       == 0) has_id = 1;
        if (strcmp(r.keys[i], "username") == 0) has_username = 1;
        if (strcmp(r.keys[i], "score")    == 0) has_score = 1;
    }
    CHECK("reads schema keys", has_id && has_username && has_score);

    // record(0) id reads without error
    {
        nxs_object_t obj;
        nxs_record(&r, 0, &obj);
        int64_t id = -1;
        nxs_err_t e = nxs_get_i64(&obj, "id", &id);
        CHECK("record(0) id reads without error", e == NXS_OK);
    }

    // record(42) has a non-empty username
    {
        nxs_object_t obj;
        nxs_record(&r, 42, &obj);
        char uname[64] = {0};
        nxs_get_str(&obj, "username", uname, sizeof(uname));
        CHECK("record(42) username non-empty", uname[0] != '\0');
    }

    // record(500) score is a finite float
    {
        nxs_object_t obj;
        nxs_record(&r, 500, &obj);
        double score = 0.0;
        nxs_get_f64(&obj, "score", &score);
        CHECK("record(500) score is finite", isfinite(score));
    }

    // record(999) active is 0 or 1
    {
        nxs_object_t obj;
        nxs_record(&r, 999, &obj);
        int active = -1;
        nxs_get_bool(&obj, "active", &active);
        CHECK("record(999) active is bool", active == 0 || active == 1);
    }

    // out-of-bounds returns error
    {
        nxs_object_t obj;
        nxs_err_t e = nxs_record(&r, 10000, &obj);
        CHECK("out-of-bounds record returns error", e == NXS_ERR_OUT_OF_BOUNDS);
    }

    // sum_f64 is a finite non-zero number
    {
        double sum = nxs_sum_f64(&r, "score");
        CHECK("sum_f64(score) is finite", isfinite(sum) && sum != 0.0);
    }

    // sum_i64 is positive
    {
        int64_t s = nxs_sum_i64(&r, "id");
        CHECK("sum_i64(id) is positive", s > 0);
    }

    // min <= max
    {
        double mn = 0.0, mx = 0.0;
        nxs_min_f64(&r, "score", &mn);
        nxs_max_f64(&r, "score", &mx);
        CHECK("min_f64 <= max_f64", mn <= mx);
    }

    nxs_close(&r);

    // PAX string column across pages (optional conformance vector)
    {
        const char *paths[] = {
            "../../nyxis/conformance/pax_flat8_strings_p128_300.nxb",
            "../nyxis/conformance/pax_flat8_strings_p128_300.nxb",
            "nyxis/conformance/pax_flat8_strings_p128_300.nxb"
        };
        uint8_t *pax_data = NULL;
        size_t pax_size = 0;
        for (int i = 0; i < 3 && !pax_data; i++) {
            pax_data = read_file(paths[i], &pax_size);
        }
        if (pax_data) {
            nxs_reader_t pr;
            nxs_err_t e = nxs_open(&pr, pax_data, pax_size);
            CHECK("PAX strings: opens without error", e == NXS_OK);
            if (e == NXS_OK) {
                CHECK("PAX strings: layout is PAX", pr.layout == NXS_LAYOUT_PAX);
                nxs_object_t obj;
                char name[32] = {0};
                nxs_record(&pr, 257, &obj);
                e = nxs_get_str(&obj, "name", name, sizeof(name));
                CHECK("PAX strings: record(257) name", e == NXS_OK && strcmp(name, "user_257") == 0);
                e = nxs_get_str(&obj, "name", name, 0);
                CHECK("PAX strings: zero-length buffer rejected", e == NXS_ERR_OUT_OF_BOUNDS);
                nxs_close(&pr);
            }
            free(pax_data);
        } else {
            printf("  - PAX string conformance vector missing; skipping\n");
        }
    }

    // ── Security tests ────────────────────────────────────────────────────────
    {
        uint8_t *bad = malloc(nxb_size);
        memcpy(bad, nxb_data, nxb_size);
        bad[0] = 0x00;
        nxs_reader_t r2;
        nxs_err_t e = nxs_open(&r2, bad, nxb_size);
        CHECK("bad magic returns ERR_BAD_MAGIC", e == NXS_ERR_BAD_MAGIC);
        free(bad);
    }

    {
        uint8_t tiny[16] = {0x4E,0x58,0x53,0x42,0x00,0x01,0x00,0x00,
                            0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
        nxs_reader_t r2;
        nxs_err_t e = nxs_open(&r2, tiny, sizeof(tiny));
        CHECK("truncated file returns error", e != NXS_OK);
    }

    {
        uint8_t *bad = malloc(nxb_size);
        memcpy(bad, nxb_data, nxb_size);
        bad[8] ^= 0xFF;
        nxs_reader_t r2;
        nxs_err_t e = nxs_open(&r2, bad, nxb_size);
        CHECK("corrupt DictHash returns ERR_DICT_MISMATCH", e == NXS_ERR_DICT_MISMATCH);
        free(bad);
    }

    free(nxb_data);

    // ── Writer round-trip tests ───────────────────────────────────────────────
    printf("\nNXS C Writer — Round-trip Tests\n\n");

    // 3-record round-trip
    {
        const char *keys[] = {"id", "username", "score", "active"};
        nxs_writer_t w;
        nxs_writer_init(&w, keys, 4, 1024);

        struct { int64_t id; const char *name; double score; int active; }
            recs[] = {{1,"alice",9.5,1},{2,"bob",7.2,0},{3,"carol",8.8,1}};

        for (int i = 0; i < 3; i++) {
            nxs_writer_begin_object(&w);
            nxs_write_i64 (&w, 0, recs[i].id);
            nxs_write_str (&w, 1, recs[i].name, (uint32_t)strlen(recs[i].name));
            nxs_write_f64 (&w, 2, recs[i].score);
            nxs_write_bool(&w, 3, recs[i].active);
            nxs_writer_end_object(&w);
        }
        nxs_writer_finish(&w);

        nxs_reader_t rr;
        nxs_err_t e = nxs_open(&rr, w.out, w.out_size);
        CHECK("writer round-trip: opens without error", e == NXS_OK);
        CHECK("writer round-trip: record count == 3", rr.record_count == 3);

        nxs_object_t obj;
        nxs_record(&rr, 0, &obj);
        int64_t id0 = 0; nxs_get_i64(&obj, "id", &id0);
        CHECK("writer round-trip: record(0) id == 1", id0 == 1);

        nxs_record(&rr, 1, &obj);
        char uname[64] = {0}; nxs_get_str(&obj, "username", uname, sizeof(uname));
        CHECK("writer round-trip: record(1) username == bob", strcmp(uname, "bob") == 0);

        nxs_record(&rr, 2, &obj);
        double sc = 0; nxs_get_f64(&obj, "score", &sc);
        CHECK("writer round-trip: record(2) score ~= 8.8", fabs(sc - 8.8) < 1e-9);

        nxs_record(&rr, 0, &obj);
        int act = -1; nxs_get_bool(&obj, "active", &act);
        CHECK("writer round-trip: record(0) active == 1", act == 1);

        nxs_record(&rr, 1, &obj);
        act = -1; nxs_get_bool(&obj, "active", &act);
        CHECK("writer round-trip: record(1) active == 0", act == 0);

        nxs_writer_free(&w);
    }

    // null field
    {
        const char *keys[] = {"a", "b"};
        nxs_writer_t w;
        nxs_writer_init(&w, keys, 2, 256);
        nxs_writer_begin_object(&w);
        nxs_write_i64 (&w, 0, 99);
        nxs_write_null(&w, 1);
        nxs_writer_end_object(&w);
        nxs_writer_finish(&w);

        nxs_reader_t rr; nxs_open(&rr, w.out, w.out_size);
        nxs_object_t obj; nxs_record(&rr, 0, &obj);
        int64_t av = 0; nxs_get_i64(&obj, "a", &av);
        CHECK("writer null field: a == 99", av == 99);
        nxs_writer_free(&w);
    }

    // bool fields
    {
        const char *keys[] = {"flag"};
        nxs_writer_t w;
        nxs_writer_init(&w, keys, 1, 256);
        nxs_writer_begin_object(&w); nxs_write_bool(&w, 0, 1); nxs_writer_end_object(&w);
        nxs_writer_begin_object(&w); nxs_write_bool(&w, 0, 0); nxs_writer_end_object(&w);
        nxs_writer_finish(&w);

        nxs_reader_t rr; nxs_open(&rr, w.out, w.out_size);
        nxs_object_t obj;
        int b0 = -1, b1 = -1;
        nxs_record(&rr, 0, &obj); nxs_get_bool(&obj, "flag", &b0);
        nxs_record(&rr, 1, &obj); nxs_get_bool(&obj, "flag", &b1);
        CHECK("writer bool: record(0) == 1", b0 == 1);
        CHECK("writer bool: record(1) == 0", b1 == 0);
        nxs_writer_free(&w);
    }

    // unicode string
    {
        const char *keys[] = {"msg"};
        nxs_writer_t w;
        nxs_writer_init(&w, keys, 1, 256);
        const char *s = "h\xC3\xA9llo w\xC3\xB6rld"; // héllo wörld UTF-8
        nxs_writer_begin_object(&w);
        nxs_write_str(&w, 0, s, (uint32_t)strlen(s));
        nxs_writer_end_object(&w);
        nxs_writer_finish(&w);

        nxs_reader_t rr; nxs_open(&rr, w.out, w.out_size);
        nxs_object_t obj; nxs_record(&rr, 0, &obj);
        char buf[64] = {0}; nxs_get_str(&obj, "msg", buf, sizeof(buf));
        CHECK("writer unicode string round-trip", strcmp(buf, s) == 0);
        nxs_writer_free(&w);
    }

    // many fields — multi-byte bitmask (9 fields, needs 2 bitmask bytes)
    {
        const char *keys[] = {"f0","f1","f2","f3","f4","f5","f6","f7","f8"};
        nxs_writer_t w;
        nxs_writer_init(&w, keys, 9, 512);
        nxs_writer_begin_object(&w);
        for (int i = 0; i < 9; i++) nxs_write_i64(&w, i, (int64_t)(i * 100));
        nxs_writer_end_object(&w);
        nxs_writer_finish(&w);

        nxs_reader_t rr; nxs_open(&rr, w.out, w.out_size);
        nxs_object_t obj; nxs_record(&rr, 0, &obj);
        int all_ok = 1;
        for (int i = 0; i < 9; i++) {
            int64_t v = 0; nxs_get_i64(&obj, keys[i], &v);
            if (v != (int64_t)(i * 100)) { all_ok = 0; break; }
        }
        CHECK("writer many fields (multi-byte bitmask)", all_ok);
        nxs_writer_free(&w);
    }

    // ── Query engine tests ────────────────────────────────────────────────────
    printf("\nNXS C Query Engine — Tests\n\n");

    {
        size_t nxb2_size = 0;
        uint8_t *nxb2_data = read_file(nxb_path, &nxb2_size);
        if (nxb2_data) {
            nxs_reader_t qr;
            nxs_open(&qr, nxb2_data, nxb2_size);

            /* test_query_count_all — NULL preds iterates every record */
            {
                NxsQuery q;
                nxs_query_init(&q, &qr, NULL, 0);
                uint32_t cnt = nxs_query_count(&q);
                CHECK("query count_all == record_count", cnt == qr.record_count);
            }

            /* test_query_eq_bool — filter active==true */
            {
                NxsPred p = nxs_pred_eq_bool(&qr, "active", 1);
                NxsQuery q;
                nxs_query_init(&q, &qr, &p, 1);
                uint32_t cnt = nxs_query_count(&q);
                /* must be > 0 and < total */
                CHECK("query eq_bool(active=true) > 0", cnt > 0);
                CHECK("query eq_bool(active=true) < total", cnt < qr.record_count);
            }

            /* test_query_gt_f64 — filter score > 5.0 (scores are in [0.0,9.9]) */
            {
                NxsPred p = nxs_pred_gt_f64(&qr, "score", 5.0);
                NxsQuery q;
                nxs_query_init(&q, &qr, &p, 1);
                uint32_t cnt = nxs_query_count(&q);
                /* verify every returned record actually satisfies the predicate */
                nxs_query_init(&q, &qr, &p, 1);
                nxs_object_t obj;
                int all_gt = 1;
                while (nxs_query_next(&q, &obj)) {
                    double sc = 0.0;
                    nxs_get_f64(&obj, "score", &sc);
                    if (sc <= 5.0) { all_gt = 0; break; }
                }
                CHECK("query gt_f64(score>5.0) > 0", cnt > 0);
                CHECK("query gt_f64(score>5.0) all satisfy predicate", all_gt);
            }

            /* test_query_and — active==true AND score>5.0 */
            {
                NxsPred preds[2];
                preds[0] = nxs_pred_eq_bool(&qr, "active", 1);
                preds[1] = nxs_pred_gt_f64(&qr, "score", 5.0);
                NxsQuery q;
                nxs_query_init(&q, &qr, preds, 2);
                uint32_t cnt_and = nxs_query_count(&q);

                /* count each individually to verify AND <= min(A, B) */
                NxsPred pa = nxs_pred_eq_bool(&qr, "active", 1);
                NxsQuery qa; nxs_query_init(&qa, &qr, &pa, 1);
                uint32_t cnt_a = nxs_query_count(&qa);

                NxsPred pb = nxs_pred_gt_f64(&qr, "score", 5.0);
                NxsQuery qb; nxs_query_init(&qb, &qr, &pb, 1);
                uint32_t cnt_b = nxs_query_count(&qb);

                uint32_t min_ab = cnt_a < cnt_b ? cnt_a : cnt_b;
                CHECK("query AND(active=true, score>5.0) <= min(A,B)", cnt_and <= min_ab);
                CHECK("query AND result > 0", cnt_and > 0);
            }

            nxs_close(&qr);
            free(nxb2_data);
        }
    }

    /* Columnar variable-length strings (Phase 3 conformance vector) */
    {
        char col_path[512];
        const char *col_candidates[] = {
            "%s/../../nyxis/conformance/columnar_flat8_strings_100.nxb",
            "%s/../nyxis/conformance/columnar_flat8_strings_100.nxb",
            "%s/../../conformance/columnar_flat8_strings_100.nxb",
        };
        size_t col_size = 0;
        uint8_t *col_data = NULL;
        for (size_t ci = 0; ci < sizeof(col_candidates) / sizeof(col_candidates[0]); ci++) {
            snprintf(col_path, sizeof(col_path), col_candidates[ci], dir);
            col_data = read_file(col_path, &col_size);
            if (col_data) break;
        }
        if (col_data) {
            nxs_reader_t cr;
            if (nxs_open(&cr, col_data, col_size) == NXS_OK) {
                int name_slot = nxs_slot(&cr, "name");
                nxs_object_t obj;
                char name[64] = {0};

                nxs_record(&cr, 0, &obj);
                CHECK("columnar strings record(0) name",
                      nxs_get_str_slot(&obj, name_slot, name, sizeof(name)) == NXS_OK &&
                      strcmp(name, "user_0") == 0);

                nxs_record(&cr, 42, &obj);
                memset(name, 0, sizeof(name));
                CHECK("columnar strings record(42) name",
                      nxs_get_str_slot(&obj, name_slot, name, sizeof(name)) == NXS_OK &&
                      strcmp(name, "user_42") == 0);

                {
                    const uint8_t *bm = NULL, *off = NULL, *vals = NULL;
                    size_t bm_len = 0, off_len = 0, val_len = 0;
                    CHECK("columnar col_var_buffer opens",
                          nxs_col_var_buffer(&cr, "name", &bm, &bm_len, &off, &off_len,
                                             &vals, &val_len) == NXS_OK);
                    CHECK("columnar col_var_buffer offsets size",
                          off_len == (size_t)(cr.record_count + 1) * 4);
                    CHECK("columnar col_var_buffer user_0 len",
                          rd_u32_le(off) == 0 && rd_u32_le(off + 4) == 6 &&
                          val_len >= 6 && memcmp(vals, "user_0", 6) == 0);
                }

                nxs_close(&cr);
            } else {
                CHECK("columnar strings conformance opens", 0);
            }
            free(col_data);
        }
    }

    /* PAX sealed col sum + streaming first page */
    {
        char pax_path[512];
        snprintf(pax_path, sizeof(pax_path), "%s/../../nyxis/conformance/pax_flat8_dense_p256_1000.nxb", dir);
        size_t pax_size = 0;
        uint8_t *pax_data = read_file(pax_path, &pax_size);
        if (!pax_data) {
            snprintf(pax_path, sizeof(pax_path), "%s/../nyxis/conformance/pax_flat8_dense_p256_1000.nxb", dir);
            pax_data = read_file(pax_path, &pax_size);
        }
        if (pax_data) {
            nxs_reader_t pr;
            if (nxs_open(&pr, pax_data, pax_size) == NXS_OK) {
                double sum = nxs_col_sum_f64(&pr, "score");
                double want = 0.0;
                for (int i = 0; i < 1000; i++) want += (double)i * 0.5;
                CHECK("PAX ColSumF64 matches dense conformance", fabs(sum - want) < 1e-3);
                nxs_close(&pr);
            } else {
                CHECK("PAX conformance opens", 0);
            }
            size_t off = 32;
            if (off + 2 <= pax_size) {
                uint16_t kc;
                memcpy(&kc, pax_data + off, 2);
                off += 2 + kc;
                for (uint16_t ki = 0; ki < kc; ki++) {
                    while (off < pax_size && pax_data[off]) off++;
                    off++;
                }
                off = (off + 7) & ~(size_t)7;
                size_t plen = nxs_pax_complete_page_at(pax_data, pax_size, off, kc);
                if (plen > 0) {
                    uint8_t *partial = malloc(off + plen);
                    if (partial) {
                        memcpy(partial, pax_data, off + plen);
                        memset(partial + 16, 0, 8);
                    nxs_pax_stream_reader_t psr;
                    if (partial && nxs_pax_stream_open(&psr, partial, off + plen) == NXS_OK) {
                        nxs_pax_stream_poll(&psr);
                        CHECK("PAX stream first page has 256 records",
                              nxs_pax_stream_records_available(&psr) == 256);
                        double s1 = nxs_pax_stream_col_sum_f64(&psr, "score");
                        double w1 = 0.0;
                        for (int i = 0; i < 256; i++) w1 += (double)i * 0.5;
                        CHECK("PAX stream page1 sum", fabs(s1 - w1) < 1e-3);
                        nxs_pax_stream_close(&psr);
                    }
                    free(partial);
                    }
                }
            }
            free(pax_data);
        }
    }

    printf("\n%d passed, %d failed\n\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
