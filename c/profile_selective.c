// Workload A selective-read step profiler (C).
// Build: make profile-selective && ./profile-selective <path.nxb>
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nxs.h"

static const char *SELECTIVE_KEYS[] = { "i01", "s21", "f36", "b46", "i10" };
static const int N_SELECTIVE = 5;

static uint8_t *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *out_size = (size_t)ftell(f);
    rewind(f);
    uint8_t *buf = malloc(*out_size ? *out_size : 1);
    if (!buf) { fclose(f); return NULL; }
    if (*out_size && fread(buf, 1, *out_size, f) != *out_size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    return buf;
}

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

typedef struct {
    int64_t tail_ns;
    int64_t stage_ns;
    int64_t slot_ns;
    int64_t resolve_ns;
    int64_t cell_ns;
    int64_t full_ns;
    int64_t full_cached_slots_ns;
} sample_t;

static int cmp_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

static int64_t median_ns(int64_t *v, int n) {
    if (n <= 0) return 0;
    qsort(v, (size_t)n, sizeof(*v), cmp_i64);
    return v[n / 2];
}

#define INNER 32

static volatile int64_t g_sink_i64;
static volatile double g_sink_f64;
static volatile int g_sink_b;
static char g_sink_str[256];

static void measure_one(
    const nxs_reader_t *r,
    uint32_t idx,
    int slots[N_SELECTIVE],
    sample_t *out
) {
    nxs_object_t obj;
    int64_t t0, t1;
    int64_t i64;
    double f64;
    int b;

    t0 = now_ns();
    for (int k = 0; k < INNER; k++) {
        nxs_record(r, idx, &obj);
        g_sink_i64 ^= (int64_t)obj.offset;
    }
    t1 = now_ns();
    out->tail_ns = (t1 - t0) / INNER;

    nxs_record(r, idx, &obj);
    t0 = now_ns();
    for (int k = 0; k < INNER; k++) {
        obj.stage = 0;
        nxs_stage_object(&obj);
        g_sink_i64 ^= (int64_t)obj.offset_table_start;
    }
    t1 = now_ns();
    out->stage_ns = (t1 - t0) / INNER;

    t0 = now_ns();
    for (int k = 0; k < INNER; k++) {
        for (int i = 0; i < N_SELECTIVE; i++)
            g_sink_i64 ^= nxs_slot(r, SELECTIVE_KEYS[i]);
    }
    t1 = now_ns();
    out->slot_ns = (t1 - t0) / INNER;

    nxs_record(r, idx, &obj);
    nxs_stage_object(&obj);
    t0 = now_ns();
    for (int k = 0; k < INNER; k++) {
        for (int i = 0; i < N_SELECTIVE; i++)
            g_sink_i64 ^= nxs_resolve_slot(&obj, slots[i]);
    }
    t1 = now_ns();
    out->resolve_ns = (t1 - t0) / INNER;

    t0 = now_ns();
    for (int k = 0; k < INNER; k++) {
        nxs_get_i64_slot(&obj, slots[0], &i64);
        nxs_get_str_slot(&obj, slots[1], g_sink_str, sizeof(g_sink_str));
        nxs_get_f64_slot(&obj, slots[2], &f64);
        nxs_get_bool_slot(&obj, slots[3], &b);
        nxs_get_i64_slot(&obj, slots[4], &i64);
        g_sink_i64 ^= i64;
        g_sink_f64 += f64;
        g_sink_b ^= b;
        g_sink_i64 ^= (int64_t)(unsigned char)g_sink_str[0];
    }
    t1 = now_ns();
    out->cell_ns = (t1 - t0) / INNER;

    t0 = now_ns();
    for (int k = 0; k < INNER; k++) {
        nxs_record(r, idx, &obj);
        nxs_get_i64(&obj, "i01", &i64);
        nxs_get_str(&obj, "s21", g_sink_str, sizeof(g_sink_str));
        nxs_get_f64(&obj, "f36", &f64);
        nxs_get_bool(&obj, "b46", &b);
        nxs_get_i64(&obj, "i10", &i64);
        g_sink_i64 ^= i64;
        g_sink_f64 += f64;
        g_sink_b ^= b;
        g_sink_i64 ^= (int64_t)(unsigned char)g_sink_str[0];
    }
    t1 = now_ns();
    out->full_ns = (t1 - t0) / INNER;

    nxs_record(r, idx, &obj);
    nxs_stage_object(&obj);
    t0 = now_ns();
    for (int k = 0; k < INNER; k++) {
        nxs_get_i64_slot(&obj, slots[0], &i64);
        nxs_get_str_slot(&obj, slots[1], g_sink_str, sizeof(g_sink_str));
        nxs_get_f64_slot(&obj, slots[2], &f64);
        nxs_get_bool_slot(&obj, slots[3], &b);
        nxs_get_i64_slot(&obj, slots[4], &i64);
        g_sink_i64 ^= i64;
        g_sink_f64 += f64;
        g_sink_b ^= b;
    }
    t1 = now_ns();
    out->full_cached_slots_ns = (t1 - t0) / INNER;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <workload_A.nxb> [samples]\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    int samples = argc >= 3 ? atoi(argv[2]) : 10000;

    size_t len = 0;
    uint8_t *data = read_file(path, &len);
    if (!data) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }

    nxs_reader_t r;
    if (nxs_open(&r, data, len) != NXS_OK) {
        fprintf(stderr, "nxs_open failed\n");
        free(data);
        return 1;
    }

    int slots[N_SELECTIVE];
    for (int i = 0; i < N_SELECTIVE; i++) {
        slots[i] = nxs_slot(&r, SELECTIVE_KEYS[i]);
        if (slots[i] < 0) {
            fprintf(stderr, "key not found: %s\n", SELECTIVE_KEYS[i]);
            nxs_close(&r);
            free(data);
            return 1;
        }
    }

    uint32_t n = nxs_record_count(&r);
    if (n == 0) {
        fprintf(stderr, "empty file\n");
        return 1;
    }

    sample_t *samps = calloc((size_t)samples, sizeof(sample_t));
    if (!samps) return 1;

    uint32_t rec_idx = 0;
    for (int i = 0; i < 100; i++) {
        rec_idx = (rec_idx * 997u + 1u) % n;
        measure_one(&r, rec_idx, slots, &samps[0]);
    }
    for (int i = 0; i < samples; i++) {
        rec_idx = (rec_idx * 997u + 1u) % n;
        measure_one(&r, rec_idx, slots, &samps[i]);
    }

    int64_t tail[samples], stage[samples], slot[samples], resolve[samples];
    int64_t cell[samples], full[samples], cached[samples];
    for (int i = 0; i < samples; i++) {
        tail[i] = samps[i].tail_ns;
        stage[i] = samps[i].stage_ns;
        slot[i] = samps[i].slot_ns;
        resolve[i] = samps[i].resolve_ns;
        cell[i] = samps[i].cell_ns;
        full[i] = samps[i].full_ns;
        cached[i] = samps[i].full_cached_slots_ns;
    }

    int64_t p_tail = median_ns(tail, samples);
    int64_t p_stage = median_ns(stage, samples);
    int64_t p_slot = median_ns(slot, samples);
    int64_t p_resolve = median_ns(resolve, samples);
    int64_t p_cell = median_ns(cell, samples);
    int64_t p_full = median_ns(full, samples);
    int64_t p_cached = median_ns(cached, samples);
    int64_t p_sum_parts = p_tail + p_stage + p_slot + p_resolve + p_cell;

    printf("{\n");
    printf("  \"path\": \"%s\",\n", path);
    printf("  \"record_count\": %u,\n", n);
    printf("  \"key_count\": %d,\n", r.key_count);
    printf("  \"samples\": %d,\n", samples);
    printf("  \"p50_ns\": {\n");
    printf("    \"1_tail_index\": %lld,\n", (long long)p_tail);
    printf("    \"2_bitmask_stage\": %lld,\n", (long long)p_stage);
    printf("    \"3_key_slot_lookup\": %lld,\n", (long long)p_slot);
    printf("    \"4_offset_resolve\": %lld,\n", (long long)p_resolve);
    printf("    \"5_cell_read\": %lld,\n", (long long)p_cell);
    printf("    \"sum_isolated_steps\": %lld,\n", (long long)p_sum_parts);
    printf("    \"full_harness_path\": %lld,\n", (long long)p_full);
    printf("    \"cached_slots_staged\": %lld\n", (long long)p_cached);
    printf("  },\n");
    printf("  \"pct_of_full\": {\n");
    if (p_full > 0) {
        printf("    \"1_tail_index\": %.1f,\n", 100.0 * p_tail / p_full);
        printf("    \"2_bitmask_stage\": %.1f,\n", 100.0 * p_stage / p_full);
        printf("    \"3_key_slot_lookup\": %.1f,\n", 100.0 * p_slot / p_full);
        printf("    \"4_offset_resolve\": %.1f,\n", 100.0 * p_resolve / p_full);
        printf("    \"5_cell_read\": %.1f,\n", 100.0 * p_cell / p_full);
        printf("    \"sum_isolated_steps\": %.1f\n", 100.0 * p_sum_parts / p_full);
    }
    printf("  }\n");
    printf("}\n");

    free(samps);
    nxs_close(&r);
    free(data);
    return 0;
}
