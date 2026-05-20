#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
// WAL append benchmark — NXS Writer vs snprintf-JSON (C).
// Build: cc -O2 -std=c99 bench_wal.c nxs_writer.c -o bench_wal && ./bench_wal
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include "nxs_writer.h"

static const char *SERVICES[] = {
    "gateway", "auth-svc", "session-svc", "catalogue-svc", "recommend-svc",
    "inventory-svc", "payment-svc", "notify-svc", "search-svc", "cdn-edge",
    "analytics-svc", "feature-flags", "config-svc", "vector-db"
};
static const int NSERVICES = 14;

static const char *OPS[] = {
    "http.server", "http.client", "grpc.server", "grpc.unary",
    "db.select", "db.insert", "db.update", "db.index_scan", "db.ann_search",
    "cache.get", "cache.set", "cache.miss",
    "pubsub.publish", "pubsub.consume",
    "llm.inference", "llm.embed",
    "jwt.verify", "auth.token_exchange",
    "queue.send", "queue.receive"
};
static const int NOPS = 20;

/* Realistic per-op duration bases (ns). Same distribution as wal.html. */
static const int64_t OP_DUR_BASE[] = {
    12000000, 11000000, 2100000, 1900000,
    4200000, 5800000, 4600000, 8100000, 14500000,
    310000, 290000, 350000,
    820000, 790000,
    1800000000, 220000000,
    590000, 1200000,
    1480000, 1510000
};

/* Small payload strings for ~15% of spans. */
static const char *PAYLOADS[] = {
    "{\"model\":\"gpt-4o-mini\",\"prompt_tokens\":418,\"completion_tokens\":91,\"total_tokens\":509,\"finish_reason\":\"stop\"}",
    "{\"model\":\"text-embedding-3-small\",\"prompt_tokens\":256,\"top_k\":20,\"reranked\":8,\"latency_to_first_token_ms\":19}",
    "{\"attempt\":1,\"provider\":\"stripe\",\"error\":\"upstream_timeout\",\"http_status\":504}",
    "{\"attempt\":2,\"provider\":\"adyen\",\"transaction_id\":\"txn_9f3a21c8\",\"http_status\":200}",
    "{\"query_plan\":\"index_scan\",\"rows_examined\":18420,\"rows_returned\":124,\"execution_ms\":7.3}",
    "{\"cache_key\":\"sess:usr_0x3f8a\",\"ttl_remaining_s\":1740,\"hit\":true,\"bytes\":892}",
    "{\"topic\":\"order.confirmed\",\"partition\":3,\"offset\":8847219,\"ack_ms\":0.8}"
};
static const int NPAYLOADS = 7;

static const char *SPAN_KEYS[] = {
    "trace_id_hi", "trace_id_lo", "span_id", "parent_span_id",
    "name", "service", "start_time_ns", "duration_ns", "status_code", "payload"
};
static const int64_t START_NS = (int64_t)1715018000LL * 1000000000LL;

static int64_t span_dur_ns(int op_idx, int i) {
    int64_t base = OP_DUR_BASE[op_idx];
    uint32_t h = (uint32_t)i * 2654435761u;
    int64_t jitter = (int64_t)(h % (uint32_t)(base * 0.8));
    return base + jitter - (int64_t)(base * 0.4);
}

static int span_status(int i) {
    uint32_t h = (uint32_t)i * 2246822519u;
    if (h < 0x07AE147Au) return 1;
    if (h < 0x0A3D70A4u) return 2;
    return 0;
}

/* Returns payload string or NULL (~15% hit rate). */
static const char *span_payload(int op_idx, int i) {
    int is_llm = (op_idx == 14 || op_idx == 15);
    int is_pay = (op_idx == 1 && i % 7 == 0);
    uint32_t h = (uint32_t)(i * 1664525 + 1013904223);
    int stoch  = (h < 0x26666666u);
    if (is_llm || is_pay || stoch)
        return PAYLOADS[i % NPAYLOADS];
    return NULL;
}

static double ns_per_span_nxs(int n) {
    nxs_writer_t w;
    nxs_writer_init(&w, SPAN_KEYS, 10, n * 192);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < n; i++) {
        int op_idx  = i % NOPS;
        int svc_idx = i % NSERVICES;
        const char *op  = OPS[op_idx];
        const char *svc = SERVICES[svc_idx];
        int64_t parent = (i % 8 == 0) ? 0 : (int64_t)(i - 1);
        const char *payload = span_payload(op_idx, i);
        nxs_writer_begin_object(&w);
        nxs_write_i64(&w, 0, (int64_t)i * 1000003);
        nxs_write_i64(&w, 1, -(int64_t)(i * 999983 + 1));
        nxs_write_i64(&w, 2, (int64_t)(i + 1));
        nxs_write_i64(&w, 3, parent);
        nxs_write_str(&w, 4, op,  (uint32_t)strlen(op));
        nxs_write_str(&w, 5, svc, (uint32_t)strlen(svc));
        nxs_write_i64(&w, 6, START_NS + (int64_t)i * 1000000);
        nxs_write_i64(&w, 7, span_dur_ns(op_idx, i));
        nxs_write_i64(&w, 8, (int64_t)span_status(i));
        if (payload)
            nxs_write_str(&w, 9, payload, (uint32_t)strlen(payload));
        nxs_writer_end_object(&w);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    nxs_writer_free(&w);
    double elapsed_ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
    return elapsed_ns / n;
}

static double ns_per_span_json(int n) {
    char buf[768];
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < n; i++) {
        int op_idx  = i % NOPS;
        int svc_idx = i % NSERVICES;
        const char *op  = OPS[op_idx];
        const char *svc = SERVICES[svc_idx];
        int64_t parent = (i % 8 == 0) ? 0 : (int64_t)(i - 1);
        const char *payload = span_payload(op_idx, i);
        if (payload) {
            snprintf(buf, sizeof(buf),
                "{\"trace_id_hi\":%lld,\"trace_id_lo\":%lld,"
                "\"span_id\":%lld,\"parent_span_id\":%lld,"
                "\"name\":\"%s\",\"service\":\"%s\","
                "\"start_time_ns\":%lld,\"duration_ns\":%lld,"
                "\"status_code\":%d,\"payload\":%s}",
                (long long)((int64_t)i * 1000003),
                (long long)(-(int64_t)(i * 999983 + 1)),
                (long long)(i + 1), (long long)parent,
                op, svc,
                (long long)(START_NS + (int64_t)i * 1000000),
                (long long)span_dur_ns(op_idx, i),
                span_status(i), payload);
        } else {
            snprintf(buf, sizeof(buf),
                "{\"trace_id_hi\":%lld,\"trace_id_lo\":%lld,"
                "\"span_id\":%lld,\"parent_span_id\":%lld,"
                "\"name\":\"%s\",\"service\":\"%s\","
                "\"start_time_ns\":%lld,\"duration_ns\":%lld,"
                "\"status_code\":%d}",
                (long long)((int64_t)i * 1000003),
                (long long)(-(int64_t)(i * 999983 + 1)),
                (long long)(i + 1), (long long)parent,
                op, svc,
                (long long)(START_NS + (int64_t)i * 1000000),
                (long long)span_dur_ns(op_idx, i),
                span_status(i));
        }
        (void)buf;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed_ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
    return elapsed_ns / n;
}

static void fmt_ns(double ns, char *out, int sz) {
    if (ns < 1000)           snprintf(out, sz, "%.0f ns", ns);
    else if (ns < 1000000)   snprintf(out, sz, "%.1f µs", ns / 1000);
    else                     snprintf(out, sz, "%.2f ms", ns / 1000000);
}

int main(void) {
    int counts[] = {1000, 10000, 100000};
    printf("WAL append benchmark — C (nxs_writer vs snprintf JSON)\n");
    for (int ci = 0; ci < 3; ci++) {
        int n = counts[ci];
        // warmup
        ns_per_span_nxs(n < 1000 ? n : 1000);
        ns_per_span_json(n < 1000 ? n : 1000);

        double nxs_best = 0, json_best = 0;
        for (int r = 0; r < 3; r++) {
            double nxs_ns  = ns_per_span_nxs(n);
            double json_ns = ns_per_span_json(n);
            if (r == 0 || nxs_ns  < nxs_best)  nxs_best  = nxs_ns;
            if (r == 0 || json_ns < json_best)  json_best = json_ns;
        }
        char nb[32], jb[32];
        fmt_ns(nxs_best, nb, sizeof(nb));
        fmt_ns(json_best, jb, sizeof(jb));
        printf("\n  n = %d\n", n);
        printf("  NXS WAL  %10s  (%.0f k spans/s)\n", nb, 1e9 / nxs_best / 1000);
        printf("  JSON     %10s  (%.0f k spans/s)\n", jb, 1e9 / json_best / 1000);
        if (json_best >= nxs_best)
            printf("  NXS is %.2fx faster than JSON\n", json_best / nxs_best);
        else
            printf("  JSON is %.2fx faster than NXS\n", nxs_best / json_best);
    }
    printf("\n");
    return 0;
}
