// Adaptive prefetch — Phase 2 (pattern detector, strategies, eager background).
#include "nxs_prefetch.h"
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>

typedef struct nxs_cached_page {
    uint32_t                page_index;
    uint8_t                *data;
    size_t                  data_len;
    uint64_t                last_used;
    int                     pinned;
    struct nxs_cached_page *next;
} nxs_cached_page_t;

typedef struct nxs_in_flight_page {
    uint32_t                    page_index;
    struct nxs_in_flight_page  *next;
} nxs_in_flight_page_t;

struct nxs_prefetch_state {
    nxs_access_hint_t           hint;
    uint32_t                    max_pages;
    uint32_t                    page_size;
    uint32_t                    coalesce_gap;
    uint32_t                    prefetch_depth;
    nxs_cached_page_t          *cache;
    uint32_t                    cache_count;
    nxs_in_flight_page_t       *in_flight;
    uint64_t                    clock;
    uint64_t                    hits;
    uint64_t                    misses;
    uint64_t                    fetches_issued;
    nxs_access_pattern_detector_t detector;
    nxs_prefetch_strategy_t     strategy;
    size_t                      file_size;
    int                         closed;
    int                         paused;
    size_t                      cache_max_bytes;
    int                         eager_cancel;
    int                         eager_complete;
    int                         eager_started;
    int                         eager_joined;
    pthread_t                   eager_thread;
    pthread_mutex_t             mu;
};

typedef struct {
    nxs_reader_t           *reader;
    struct nxs_prefetch_state *pf;
} nxs_eager_ctx_t;

static inline uint64_t rd_u64_le(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static uint64_t row_record_offset(const nxs_reader_t *r, uint32_t i) {
    size_t entry = r->tail_start + (size_t)i * 10u + 2u;
    if (entry + 8u > r->size) return UINT64_MAX;
    return rd_u64_le(r->data + entry);
}

static void row_data_sector(size_t tail_start, size_t file_size,
                            size_t *sector_start, size_t *sector_len) {
    *sector_start = 32u;
    if (tail_start > 32u && tail_start <= file_size)
        *sector_len = tail_start - 32u;
    else
        *sector_len = 0u;
}

static nxs_cached_page_t *cache_find(struct nxs_prefetch_state *pf, uint32_t page_index) {
    for (nxs_cached_page_t *e = pf->cache; e; e = e->next) {
        if (e->page_index == page_index) return e;
    }
    return NULL;
}

static int cache_has(struct nxs_prefetch_state *pf, uint32_t page_index) {
    return cache_find(pf, page_index) != NULL;
}

static void cache_touch(struct nxs_prefetch_state *pf, nxs_cached_page_t *e) {
    e->last_used = ++pf->clock;
}

static void cache_get(struct nxs_prefetch_state *pf, uint32_t page_index) {
    nxs_cached_page_t *e = cache_find(pf, page_index);
    if (e) {
        cache_touch(pf, e);
        pf->hits++;
    } else {
        pf->misses++;
    }
}

static void cache_free_entry(nxs_cached_page_t *e) {
    free(e->data);
    free(e);
}

static int cache_evict_one(struct nxs_prefetch_state *pf) {
    nxs_cached_page_t *victim = NULL;
    nxs_cached_page_t *prev_victim = NULL;
    uint64_t oldest = UINT64_MAX;

    for (nxs_cached_page_t *e = pf->cache, *prev = NULL; e; prev = e, e = e->next) {
        if (e->pinned) continue;
        if (e->last_used < oldest) {
            oldest = e->last_used;
            victim = e;
            prev_victim = prev;
        }
    }
    if (!victim) return 0;

    if (prev_victim) prev_victim->next = victim->next;
    else pf->cache = victim->next;
    pf->cache_count--;
    cache_free_entry(victim);
    return 1;
}

static size_t cache_memory_bytes(struct nxs_prefetch_state *pf) {
    size_t total = 0;
    for (nxs_cached_page_t *e = pf->cache; e; e = e->next)
        total += e->data_len;
    return total;
}

static void cache_enforce_byte_limit(struct nxs_prefetch_state *pf) {
    if (pf->cache_max_bytes == 0) return;
    while (cache_memory_bytes(pf) > pf->cache_max_bytes) {
        if (!cache_evict_one(pf)) break;
    }
}

static void cache_unpin_all(struct nxs_prefetch_state *pf) {
    for (nxs_cached_page_t *e = pf->cache; e; e = e->next) e->pinned = 0;
}

static nxs_err_t cache_insert(struct nxs_prefetch_state *pf, uint32_t page_index,
                              uint8_t *data, size_t data_len, int pinned) {
    if (pf->max_pages == 0) {
        free(data);
        return NXS_OK;
    }
    while (pf->cache_count >= pf->max_pages) {
        if (!cache_evict_one(pf)) break;
    }

    nxs_cached_page_t *e = calloc(1, sizeof(*e));
    if (!e) {
        free(data);
        return NXS_ERR_ALLOC;
    }
    e->page_index = page_index;
    e->data = data;
    e->data_len = data_len;
    e->pinned = pinned;
    cache_touch(pf, e);
    e->next = pf->cache;
    pf->cache = e;
    pf->cache_count++;
    cache_enforce_byte_limit(pf);
    return NXS_OK;
}

static void cache_pin_pages(struct nxs_prefetch_state *pf, const uint32_t *indices, size_t count) {
    for (size_t i = 0; i < count; i++) {
        nxs_cached_page_t *e = cache_find(pf, indices[i]);
        if (e) e->pinned = 1;
    }
}

static int in_flight_has(struct nxs_prefetch_state *pf, uint32_t page_index) {
    for (nxs_in_flight_page_t *e = pf->in_flight; e; e = e->next) {
        if (e->page_index == page_index) return 1;
    }
    return 0;
}

static void in_flight_add(struct nxs_prefetch_state *pf, uint32_t page_index) {
    if (in_flight_has(pf, page_index)) return;
    nxs_in_flight_page_t *e = malloc(sizeof(*e));
    if (!e) return;
    e->page_index = page_index;
    e->next = pf->in_flight;
    pf->in_flight = e;
}

static void in_flight_remove(struct nxs_prefetch_state *pf, uint32_t page_index) {
    nxs_in_flight_page_t **pp = &pf->in_flight;
    while (*pp) {
        if ((*pp)->page_index == page_index) {
            nxs_in_flight_page_t *dead = *pp;
            *pp = dead->next;
            free(dead);
            return;
        }
        pp = &(*pp)->next;
    }
}

static void in_flight_clear(struct nxs_prefetch_state *pf) {
    while (pf->in_flight) {
        nxs_in_flight_page_t *next = pf->in_flight->next;
        free(pf->in_flight);
        pf->in_flight = next;
    }
}

static void prefetch_free_locked(struct nxs_prefetch_state *pf) {
    for (nxs_cached_page_t *e = pf->cache; e;) {
        nxs_cached_page_t *next = e->next;
        cache_free_entry(e);
        e = next;
    }
    pf->cache = NULL;
    pf->cache_count = 0;
    in_flight_clear(pf);
}

static void prefetch_free(struct nxs_prefetch_state *pf) {
    if (!pf) return;
    pthread_mutex_destroy(&pf->mu);
    free(pf);
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t aa = *(const uint32_t *)a;
    uint32_t bb = *(const uint32_t *)b;
    return (aa > bb) - (aa < bb);
}

static size_t dedupe_sort(uint32_t *indices, size_t count) {
    if (count == 0) return 0;
    qsort(indices, count, sizeof(uint32_t), cmp_u32);
    size_t w = 1;
    for (size_t i = 1; i < count; i++) {
        if (indices[i] != indices[w - 1]) indices[w++] = indices[i];
    }
    return w;
}

size_t nxs_coalesce_page_indices(const uint32_t *indices, size_t count,
                                 uint32_t gap_pages, uint32_t page_size,
                                 nxs_page_range_t *out, size_t out_cap) {
    if (!indices || count == 0 || !out || out_cap == 0) return 0;

    uint32_t *uniq = malloc(count * sizeof(uint32_t));
    if (!uniq) return 0;
    memcpy(uniq, indices, count * sizeof(uint32_t));
    count = dedupe_sort(uniq, count);

    size_t n_ranges = 0;
    uint32_t start = uniq[0];
    uint32_t end = uniq[0];

    for (size_t i = 1; i < count; i++) {
        if (uniq[i] - end <= gap_pages) {
            end = uniq[i];
        } else {
            if (n_ranges < out_cap) {
                out[n_ranges].page_start = start;
                out[n_ranges].page_end = end;
                out[n_ranges].byte_start = (uint64_t)start * page_size;
                out[n_ranges].byte_length = (uint64_t)(end - start + 1u) * page_size;
                n_ranges++;
            }
            start = end = uniq[i];
        }
    }
    if (n_ranges < out_cap) {
        out[n_ranges].page_start = start;
        out[n_ranges].page_end = end;
        out[n_ranges].byte_start = (uint64_t)start * page_size;
        out[n_ranges].byte_length = (uint64_t)(end - start + 1u) * page_size;
        n_ranges++;
    }
    free(uniq);
    return n_ranges;
}

static size_t clamp_page_ranges(nxs_page_range_t *ranges, size_t n_ranges, size_t file_size) {
    size_t w = 0;
    for (size_t i = 0; i < n_ranges; i++) {
        nxs_page_range_t r = ranges[i];
        if (r.byte_start >= file_size) continue;
        if (r.byte_start + r.byte_length > file_size)
            r.byte_length = file_size - r.byte_start;
        if (r.byte_length == 0) continue;
        ranges[w++] = r;
    }
    return w;
}

static size_t collect_page_indices(const nxs_reader_t *r, struct nxs_prefetch_state *pf,
                                   uint32_t start_index, uint32_t end_index,
                                   uint32_t *out, size_t out_cap) {
    size_t n = 0;
    for (uint32_t i = start_index; i <= end_index; i++) {
        uint64_t off = row_record_offset(r, i);
        if (off == UINT64_MAX) continue;
        uint32_t page = (uint32_t)(off / pf->page_size);
        int found = 0;
        for (size_t j = 0; j < n; j++) {
            if (out[j] == page) { found = 1; break; }
        }
        if (!found && n < out_cap) out[n++] = page;
    }
    return n;
}

static nxs_err_t fetch_coalesced_range_locked(nxs_reader_t *r, struct nxs_prefetch_state *pf,
                                              const nxs_page_range_t *range) {
    if (range->byte_start >= r->size) return NXS_OK;
    uint64_t byte_len = range->byte_length;
    if (range->byte_start + byte_len > r->size)
        byte_len = r->size - range->byte_start;
    if (byte_len == 0) return NXS_OK;

    pf->fetches_issued++;

    uint8_t *blob = malloc((size_t)byte_len);
    if (!blob) return NXS_ERR_ALLOC;
    memcpy(blob, r->data + range->byte_start, (size_t)byte_len);

    for (uint32_t p = range->page_start; p <= range->page_end; p++) {
        in_flight_add(pf, p);
    }

    for (uint32_t p = range->page_start; p <= range->page_end; p++) {
        if (cache_has(pf, p)) {
            in_flight_remove(pf, p);
            continue;
        }
        uint64_t page_off = (uint64_t)p * pf->page_size;
        if (page_off < range->byte_start) {
            in_flight_remove(pf, p);
            continue;
        }
        size_t rel = (size_t)(page_off - range->byte_start);
        if (rel >= byte_len) {
            in_flight_remove(pf, p);
            continue;
        }
        size_t page_len = pf->page_size;
        if (rel + page_len > byte_len) page_len = (size_t)byte_len - rel;

        uint8_t *page_data = malloc(page_len);
        if (!page_data) {
            in_flight_remove(pf, p);
            free(blob);
            return NXS_ERR_ALLOC;
        }
        memcpy(page_data, blob + rel, page_len);
        nxs_err_t err = cache_insert(pf, p, page_data, page_len, 0);
        in_flight_remove(pf, p);
        if (err != NXS_OK) {
            free(blob);
            return err;
        }
    }
    free(blob);
    return NXS_OK;
}

static nxs_err_t fetch_coalesced_range(nxs_reader_t *r, struct nxs_prefetch_state *pf,
                                         const nxs_page_range_t *range) {
    pthread_mutex_lock(&pf->mu);
    nxs_err_t err = fetch_coalesced_range_locked(r, pf, range);
    pthread_mutex_unlock(&pf->mu);
    return err;
}

static void *eager_worker(void *arg) {
    nxs_eager_ctx_t *ctx = (nxs_eager_ctx_t *)arg;
    nxs_reader_t *r = ctx->reader;
    struct nxs_prefetch_state *pf = ctx->pf;
    free(ctx);

    size_t sector_start = 0, sector_len = 0;
    row_data_sector(r->tail_start, r->size, &sector_start, &sector_len);
    if (sector_len == 0) {
        pthread_mutex_lock(&pf->mu);
        if (!pf->eager_cancel) pf->eager_complete = 1;
        pthread_mutex_unlock(&pf->mu);
        return NULL;
    }

    size_t end = sector_start + sector_len;
    if (end > r->size) end = r->size;
    if (sector_start >= end) {
        pthread_mutex_lock(&pf->mu);
        if (!pf->eager_cancel) pf->eager_complete = 1;
        pthread_mutex_unlock(&pf->mu);
        return NULL;
    }

    uint32_t page_size = pf->page_size;
    uint32_t first_page = (uint32_t)(sector_start / page_size);
    uint32_t last_page = (uint32_t)((end - 1u) / page_size);

    size_t n_pages = (size_t)(last_page - first_page + 1u);
    uint32_t *indices = malloc(n_pages * sizeof(uint32_t));
    if (!indices) return NULL;
    for (uint32_t p = first_page; p <= last_page; p++)
        indices[p - first_page] = p;

    nxs_page_range_t ranges[64];
    size_t n_ranges = nxs_coalesce_page_indices(indices, n_pages, pf->coalesce_gap,
                                                page_size, ranges,
                                                sizeof(ranges) / sizeof(ranges[0]));
    free(indices);
    n_ranges = clamp_page_ranges(ranges, n_ranges, r->size);

    for (size_t ri = 0; ri < n_ranges; ri++) {
        pthread_mutex_lock(&pf->mu);
        int cancelled = pf->eager_cancel;
        pthread_mutex_unlock(&pf->mu);
        if (cancelled) return NULL;

        nxs_err_t err = fetch_coalesced_range(r, pf, &ranges[ri]);
        if (err != NXS_OK) return NULL;
    }

    pthread_mutex_lock(&pf->mu);
    if (!pf->eager_cancel) pf->eager_complete = 1;
    pthread_mutex_unlock(&pf->mu);
    return NULL;
}

static int start_eager_background(nxs_reader_t *r, struct nxs_prefetch_state *pf) {
    pthread_mutex_lock(&pf->mu);
    if (pf->paused || pf->strategy != NXS_STRATEGY_EAGER || pf->eager_started || pf->closed) {
        pthread_mutex_unlock(&pf->mu);
        return 0;
    }

    size_t sector_start = 0, sector_len = 0;
    row_data_sector(r->tail_start, r->size, &sector_start, &sector_len);
    if (sector_len == 0) {
        pf->eager_complete = 1;
        pf->eager_joined = 1;
        pthread_mutex_unlock(&pf->mu);
        return 1;
    }

    pf->eager_started = 1;
    pthread_mutex_unlock(&pf->mu);

    nxs_eager_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) return 0;
    ctx->reader = r;
    ctx->pf = pf;

    if (pthread_create(&pf->eager_thread, NULL, eager_worker, ctx) != 0) {
        free(ctx);
        pthread_mutex_lock(&pf->mu);
        pf->eager_started = 0;
        pthread_mutex_unlock(&pf->mu);
        return 0;
    }
    return 1;
}

static int maybe_upgrade_to_eager_locked(struct nxs_prefetch_state *pf) {
    if (pf->paused || pf->strategy != NXS_STRATEGY_ADAPTIVE) return 0;
    if (nxs_pattern_detector_pattern(&pf->detector) != NXS_PATTERN_SEQUENTIAL) return 0;
    if (nxs_pattern_detector_sequential_runs(&pf->detector) < NXS_PREFETCH_UPGRADE_SEQ_THRESHOLD)
        return 0;
    if (pf->file_size / (1024u * 1024u) > NXS_PREFETCH_EAGER_THRESHOLD_MB) return 0;
    pf->strategy = NXS_STRATEGY_EAGER;
    return 1;
}

static void speculative_prefetch(nxs_reader_t *r, struct nxs_prefetch_state *pf) {
    pthread_mutex_lock(&pf->mu);
    if (pf->paused) {
        pthread_mutex_unlock(&pf->mu);
        return;
    }
    pthread_mutex_unlock(&pf->mu);

    uint32_t depth = pf->prefetch_depth;
    uint32_t predicted[32];
    size_t n_pred = 0;

    pthread_mutex_lock(&pf->mu);
    n_pred = nxs_pattern_detector_predict_next(&pf->detector, depth, r->record_count,
                                               predicted, sizeof(predicted) / sizeof(predicted[0]));
    pthread_mutex_unlock(&pf->mu);
    if (n_pred == 0) return;

    uint32_t page_indices[32];
    size_t n_pages = 0;
    for (size_t i = 0; i < n_pred; i++) {
        uint64_t off = row_record_offset(r, predicted[i]);
        if (off == UINT64_MAX) continue;
        uint32_t page = (uint32_t)(off / pf->page_size);
        int found = 0;
        for (size_t j = 0; j < n_pages; j++) {
            if (page_indices[j] == page) { found = 1; break; }
        }
        if (found) continue;
        pthread_mutex_lock(&pf->mu);
        int need = !cache_has(pf, page) && !in_flight_has(pf, page);
        pthread_mutex_unlock(&pf->mu);
        if (need && n_pages < sizeof(page_indices) / sizeof(page_indices[0]))
            page_indices[n_pages++] = page;
    }
    if (n_pages == 0) return;

    qsort(page_indices, n_pages, sizeof(uint32_t), cmp_u32);

    nxs_page_range_t ranges[16];
    size_t n_ranges = nxs_coalesce_page_indices(page_indices, n_pages, pf->coalesce_gap,
                                                pf->page_size, ranges,
                                                sizeof(ranges) / sizeof(ranges[0]));
    n_ranges = clamp_page_ranges(ranges, n_ranges, r->size);

    for (size_t ri = 0; ri < n_ranges; ri++)
        (void)fetch_coalesced_range(r, pf, &ranges[ri]);
}

void nxs_open_options_init(nxs_open_options_t *opts) {
    if (!opts) return;
    opts->hint = NXS_HINT_UNKNOWN;
    opts->max_pages = NXS_PREFETCH_DEFAULT_MAX_PAGES;
    opts->page_size = NXS_PREFETCH_DEFAULT_PAGE_SIZE;
    opts->coalesce_gap_pages = NXS_PREFETCH_DEFAULT_COALESCE_GAP;
    opts->prefetch_depth = NXS_PREFETCH_DEFAULT_DEPTH;
}

nxs_prefetch_strategy_t nxs_initial_strategy(nxs_access_hint_t hint, size_t file_size) {
    size_t file_size_mb = file_size / (1024u * 1024u);
    if (hint == NXS_HINT_FULL && file_size_mb <= NXS_PREFETCH_EAGER_THRESHOLD_MB)
        return NXS_STRATEGY_EAGER;
    if (file_size_mb > NXS_PREFETCH_LAZY_THRESHOLD_MB)
        return NXS_STRATEGY_LAZY;
    return NXS_STRATEGY_ADAPTIVE;
}

const char *nxs_strategy_name(nxs_prefetch_strategy_t s) {
    switch (s) {
    case NXS_STRATEGY_ADAPTIVE: return "adaptive";
    case NXS_STRATEGY_EAGER:    return "eager";
    default:                    return "lazy";
    }
}

const char *nxs_pattern_name(nxs_access_pattern_t p) {
    switch (p) {
    case NXS_PATTERN_SEQUENTIAL: return "sequential";
    case NXS_PATTERN_RANDOM:     return "random";
    case NXS_PATTERN_MIXED:      return "mixed";
    default:                     return "unknown";
    }
}

void nxs_pattern_detector_init(nxs_access_pattern_detector_t *d) {
    if (!d) return;
    memset(d, 0, sizeof(*d));
    d->last_index = -1;
    for (uint32_t i = 0; i < NXS_PREFETCH_HISTORY_SIZE; i++)
        d->accesses[i] = -1;
}

void nxs_pattern_detector_observe(nxs_access_pattern_detector_t *d, uint32_t index) {
    if (!d) return;
    int64_t idx = (int64_t)index;
    if (d->last_index >= 0) {
        uint64_t delta;
        if (idx >= d->last_index)
            delta = (uint64_t)(idx - d->last_index);
        else
            delta = (uint64_t)(d->last_index - idx);
        if (delta <= NXS_PREFETCH_SEQ_THRESHOLD) {
            if (d->sequential_runs < UINT32_MAX) d->sequential_runs++;
        } else if (delta > NXS_PREFETCH_RANDOM_THRESHOLD) {
            if (d->random_jumps < UINT32_MAX) d->random_jumps++;
        }
    }
    d->accesses[d->write_pos] = idx;
    d->write_pos = (d->write_pos + 1u) % NXS_PREFETCH_HISTORY_SIZE;
    if (d->filled < NXS_PREFETCH_HISTORY_SIZE) d->filled++;
    d->last_index = idx;
}

nxs_access_pattern_t nxs_pattern_detector_pattern(const nxs_access_pattern_detector_t *d) {
    if (!d) return NXS_PATTERN_UNKNOWN;
    uint32_t total = d->sequential_runs + d->random_jumps;
    if (total < NXS_PREFETCH_MIN_OBSERVATIONS) return NXS_PATTERN_UNKNOWN;
    if (d->sequential_runs > d->random_jumps * 3u) return NXS_PATTERN_SEQUENTIAL;
    if (d->random_jumps > d->sequential_runs) return NXS_PATTERN_RANDOM;
    return NXS_PATTERN_MIXED;
}

uint32_t nxs_pattern_detector_sequential_runs(const nxs_access_pattern_detector_t *d) {
    return d ? d->sequential_runs : 0u;
}

size_t nxs_pattern_detector_predict_next(const nxs_access_pattern_detector_t *d,
                                         uint32_t depth, uint32_t record_count,
                                         uint32_t *out, size_t out_cap) {
    if (!d || !out || out_cap == 0) return 0;
    if (nxs_pattern_detector_pattern(d) != NXS_PATTERN_SEQUENTIAL || d->last_index < 0)
        return 0;
    uint32_t start = (uint32_t)(d->last_index + 1);
    size_t n = 0;
    for (uint32_t i = 0; i < depth && n < out_cap; i++) {
        uint32_t idx = start + i;
        if (idx < record_count) out[n++] = idx;
    }
    return n;
}

nxs_err_t nxs_prefetch_init(nxs_reader_t *r, const nxs_open_options_t *opts) {
    if (!r) return NXS_ERR_OUT_OF_BOUNDS;
    if (r->prefetch) return NXS_OK;

    nxs_open_options_t defaults;
    nxs_open_options_init(&defaults);
    const nxs_open_options_t *o = opts ? opts : &defaults;

    struct nxs_prefetch_state *pf = calloc(1, sizeof(*pf));
    if (!pf) return NXS_ERR_ALLOC;

    pf->hint = o->hint;
    pf->max_pages = o->max_pages ? o->max_pages : NXS_PREFETCH_DEFAULT_MAX_PAGES;
    pf->page_size = o->page_size ? o->page_size : NXS_PREFETCH_DEFAULT_PAGE_SIZE;
    pf->coalesce_gap = o->coalesce_gap_pages;
    pf->prefetch_depth = o->prefetch_depth ? o->prefetch_depth : NXS_PREFETCH_DEFAULT_DEPTH;
    pf->file_size = r->size;
    pf->strategy = nxs_initial_strategy(o->hint, r->size);
    nxs_pattern_detector_init(&pf->detector);
    pthread_mutex_init(&pf->mu, NULL);

    r->prefetch = pf;

    if (pf->strategy == NXS_STRATEGY_EAGER)
        (void)start_eager_background(r, pf);

    return NXS_OK;
}

static void prefetch_join_eager(struct nxs_prefetch_state *pf) {
    if (!pf->eager_started || pf->eager_joined) return;
    pthread_join(pf->eager_thread, NULL);
    pf->eager_joined = 1;
}

void nxs_prefetch_destroy(nxs_reader_t *r) {
    if (!r || !r->prefetch) return;
    struct nxs_prefetch_state *pf = r->prefetch;

    pthread_mutex_lock(&pf->mu);
    pf->closed = 1;
    pf->eager_cancel = 1;
    pthread_mutex_unlock(&pf->mu);

    prefetch_join_eager(pf);

    pthread_mutex_lock(&pf->mu);
    prefetch_free_locked(pf);
    pthread_mutex_unlock(&pf->mu);

    prefetch_free(pf);
    r->prefetch = NULL;
}

void nxs_prefetch_on_access(nxs_reader_t *r, uint32_t index) {
    if (!r || !r->prefetch || r->record_count == 0) return;
    struct nxs_prefetch_state *pf = r->prefetch;

    int start_eager = 0;
    int strategy_eager = 0;
    int adaptive_seq = 0;

    pthread_mutex_lock(&pf->mu);
    if (pf->closed || pf->paused) {
        pthread_mutex_unlock(&pf->mu);
        return;
    }
    nxs_pattern_detector_observe(&pf->detector, index);
    start_eager = maybe_upgrade_to_eager_locked(pf);
    strategy_eager = (pf->strategy == NXS_STRATEGY_EAGER);
    adaptive_seq = (pf->strategy == NXS_STRATEGY_ADAPTIVE &&
                    nxs_pattern_detector_pattern(&pf->detector) == NXS_PATTERN_SEQUENTIAL);
    pthread_mutex_unlock(&pf->mu);

    if (start_eager)
        (void)start_eager_background(r, pf);

    if (strategy_eager) return;

    uint64_t off = row_record_offset(r, index);
    if (off != UINT64_MAX) {
        uint32_t page = (uint32_t)(off / pf->page_size);
        pthread_mutex_lock(&pf->mu);
        cache_get(pf, page);
        pthread_mutex_unlock(&pf->mu);
    }

    if (adaptive_seq)
        speculative_prefetch(r, pf);
}

void nxs_pause_prefetch(nxs_reader_t *r) {
    if (!r || !r->prefetch) return;
    pthread_mutex_lock(&r->prefetch->mu);
    r->prefetch->paused = 1;
    pthread_mutex_unlock(&r->prefetch->mu);
}

void nxs_resume_prefetch(nxs_reader_t *r) {
    if (!r || !r->prefetch) return;
    pthread_mutex_lock(&r->prefetch->mu);
    r->prefetch->paused = 0;
    pthread_mutex_unlock(&r->prefetch->mu);
}

void nxs_prefetch_set_cache_limit(nxs_reader_t *r, size_t max_bytes) {
    if (!r || !r->prefetch) return;
    struct nxs_prefetch_state *pf = r->prefetch;
    pthread_mutex_lock(&pf->mu);
    pf->cache_max_bytes = max_bytes;
    cache_enforce_byte_limit(pf);
    pthread_mutex_unlock(&pf->mu);
}

void nxs_warmup(nxs_reader_t *r) {
    if (!r || !r->prefetch) return;
    struct nxs_prefetch_state *pf = r->prefetch;

    for (;;) {
        pthread_mutex_lock(&pf->mu);
        int done = pf->eager_complete || pf->eager_cancel || !pf->eager_started;
        pthread_mutex_unlock(&pf->mu);
        if (done) break;
        sched_yield();
    }
    prefetch_join_eager(pf);
}

nxs_err_t nxs_prefetch_viewport(nxs_reader_t *r, uint32_t start_index, uint32_t end_index) {
    if (!r) return NXS_ERR_OUT_OF_BOUNDS;
    if (!r->prefetch) {
        nxs_err_t err = nxs_prefetch_init(r, NULL);
        if (err != NXS_OK) return err;
    }
    if (r->layout != NXS_LAYOUT_ROW) return NXS_OK;
    if (start_index > end_index || end_index >= r->record_count)
        return NXS_ERR_OUT_OF_BOUNDS;

    struct nxs_prefetch_state *pf = r->prefetch;
    uint32_t stack_indices[512];
    size_t cap = sizeof(stack_indices) / sizeof(stack_indices[0]);
    uint32_t *indices = stack_indices;
    size_t need = (size_t)(end_index - start_index + 1u);
    if (need > cap) {
        indices = malloc(need * sizeof(uint32_t));
        if (!indices) return NXS_ERR_ALLOC;
        cap = need;
    }

    size_t n_pages = collect_page_indices(r, pf, start_index, end_index, indices, cap);

    uint32_t missing_stack[512];
    size_t missing_cap = sizeof(missing_stack) / sizeof(missing_stack[0]);
    uint32_t *missing = missing_stack;
    if (n_pages > missing_cap) {
        missing = malloc(n_pages * sizeof(uint32_t));
        if (!missing) {
            if (indices != stack_indices) free(indices);
            return NXS_ERR_ALLOC;
        }
        missing_cap = n_pages;
    }

    size_t n_missing = 0;
    pthread_mutex_lock(&pf->mu);
    for (size_t i = 0; i < n_pages; i++) {
        if (!cache_has(pf, indices[i]) && !in_flight_has(pf, indices[i])) {
            if (n_missing < missing_cap) missing[n_missing++] = indices[i];
        }
    }

    nxs_page_range_t ranges[64];
    size_t n_ranges = nxs_coalesce_page_indices(missing, n_missing, pf->coalesce_gap,
                                                pf->page_size, ranges,
                                                sizeof(ranges) / sizeof(ranges[0]));
    n_ranges = clamp_page_ranges(ranges, n_ranges, r->size);

    nxs_err_t err = NXS_OK;
    for (size_t ri = 0; ri < n_ranges; ri++) {
        err = fetch_coalesced_range_locked(r, pf, &ranges[ri]);
        if (err != NXS_OK) break;
    }

    if (err == NXS_OK) {
        cache_pin_pages(pf, indices, n_pages);
        cache_unpin_all(pf);
    }
    pthread_mutex_unlock(&pf->mu);

    if (indices != stack_indices) free(indices);
    if (missing != missing_stack) free(missing);
    return err;
}

void nxs_cache_stats(const nxs_reader_t *r, nxs_cache_stats_t *stats) {
    if (!stats) return;
    memset(stats, 0, sizeof(*stats));
    stats->strategy = "lazy";
    stats->pattern = "unknown";
    if (!r || !r->prefetch) {
        stats->pages_max = NXS_PREFETCH_DEFAULT_MAX_PAGES;
        return;
    }

    struct nxs_prefetch_state *pf = r->prefetch;
    pthread_mutex_lock(&pf->mu);
    stats->pages_cached = pf->cache_count;
    stats->pages_max = pf->max_pages;
    stats->cache_hits = pf->hits;
    stats->cache_misses = pf->misses;
    stats->fetches_issued = pf->fetches_issued;
    stats->strategy = nxs_strategy_name(pf->strategy);
    stats->pattern = nxs_pattern_name(nxs_pattern_detector_pattern(&pf->detector));
    for (nxs_cached_page_t *e = pf->cache; e; e = e->next)
        stats->memory_used_bytes += (uint64_t)e->data_len;
    pthread_mutex_unlock(&pf->mu);
}
