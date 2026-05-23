// Adaptive prefetch — Phase 1 (sync viewport + LRU page cache).
#include "nxs_prefetch.h"
#include <stdlib.h>
#include <string.h>

typedef struct nxs_cached_page {
    uint32_t                 page_index;
    uint8_t                 *data;
    size_t                   data_len;
    uint64_t                 last_used;
    int                      pinned;
    struct nxs_cached_page  *next;
} nxs_cached_page_t;

typedef struct nxs_prefetch_state {
    nxs_access_hint_t  hint;
    uint32_t           max_pages;
    uint32_t           page_size;
    uint32_t           coalesce_gap;
    uint32_t           prefetch_depth;
    nxs_cached_page_t *cache;
    uint32_t           cache_count;
    uint64_t           clock;
    uint64_t           hits;
    uint64_t           misses;
    uint64_t           fetches_issued;
} nxs_prefetch_state_t;

static inline uint64_t rd_u64_le(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static uint64_t row_record_offset(const nxs_reader_t *r, uint32_t i) {
    size_t entry = r->tail_start + (size_t)i * 10u + 2u;
    if (entry + 8u > r->size) return 0;
    return rd_u64_le(r->data + entry);
}

static nxs_cached_page_t *cache_find(nxs_prefetch_state_t *pf, uint32_t page_index) {
    for (nxs_cached_page_t *e = pf->cache; e; e = e->next) {
        if (e->page_index == page_index) return e;
    }
    return NULL;
}

static int cache_has(nxs_prefetch_state_t *pf, uint32_t page_index) {
    return cache_find(pf, page_index) != NULL;
}

static void cache_touch(nxs_prefetch_state_t *pf, nxs_cached_page_t *e) {
    e->last_used = ++pf->clock;
}

static void cache_free_entry(nxs_cached_page_t *e) {
    free(e->data);
    free(e);
}

static int cache_evict_one(nxs_prefetch_state_t *pf) {
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

static void cache_unpin_all(nxs_prefetch_state_t *pf) {
    for (nxs_cached_page_t *e = pf->cache; e; e = e->next) e->pinned = 0;
}

static nxs_err_t cache_insert(nxs_prefetch_state_t *pf, uint32_t page_index,
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
    return NXS_OK;
}


static void cache_pin_pages(nxs_prefetch_state_t *pf, const uint32_t *indices, size_t count) {
    for (size_t i = 0; i < count; i++) {
        nxs_cached_page_t *e = cache_find(pf, indices[i]);
        if (e) e->pinned = 1;
    }
}

static void prefetch_free(nxs_prefetch_state_t *pf) {
    if (!pf) return;
    for (nxs_cached_page_t *e = pf->cache; e;) {
        nxs_cached_page_t *next = e->next;
        cache_free_entry(e);
        e = next;
    }
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

static size_t collect_page_indices(const nxs_reader_t *r, nxs_prefetch_state_t *pf,
                                   uint32_t start_index, uint32_t end_index,
                                   uint32_t *out, size_t out_cap) {
    size_t n = 0;
    for (uint32_t i = start_index; i <= end_index; i++) {
        uint64_t off = row_record_offset(r, i);
        uint32_t page = (uint32_t)(off / pf->page_size);
        int found = 0;
        for (size_t j = 0; j < n; j++) {
            if (out[j] == page) { found = 1; break; }
        }
        if (!found) {
            if (n < out_cap) out[n++] = page;
        }
    }
    return n;
}

static nxs_err_t fetch_coalesced_range(nxs_reader_t *r, nxs_prefetch_state_t *pf,
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
        if (cache_has(pf, p)) continue;
        uint64_t page_off = (uint64_t)p * pf->page_size;
        if (page_off < range->byte_start) continue;
        size_t rel = (size_t)(page_off - range->byte_start);
        if (rel >= byte_len) continue;
        size_t page_len = pf->page_size;
        if (rel + page_len > byte_len) page_len = (size_t)byte_len - rel;

        uint8_t *page_data = malloc(page_len);
        if (!page_data) {
            free(blob);
            return NXS_ERR_ALLOC;
        }
        memcpy(page_data, blob + rel, page_len);
        nxs_err_t err = cache_insert(pf, p, page_data, page_len, 0);
        if (err != NXS_OK) {
            free(blob);
            return err;
        }
    }
    free(blob);
    return NXS_OK;
}

void nxs_open_options_init(nxs_open_options_t *opts) {
    if (!opts) return;
    opts->hint = NXS_HINT_UNKNOWN;
    opts->max_pages = NXS_PREFETCH_DEFAULT_MAX_PAGES;
    opts->page_size = NXS_PREFETCH_DEFAULT_PAGE_SIZE;
    opts->coalesce_gap_pages = NXS_PREFETCH_DEFAULT_COALESCE_GAP;
    opts->prefetch_depth = NXS_PREFETCH_DEFAULT_DEPTH;
}

nxs_err_t nxs_prefetch_init(nxs_reader_t *r, const nxs_open_options_t *opts) {
    if (!r) return NXS_ERR_OUT_OF_BOUNDS;
    if (r->prefetch) return NXS_OK;

    nxs_open_options_t defaults;
    nxs_open_options_init(&defaults);
    const nxs_open_options_t *o = opts ? opts : &defaults;

    nxs_prefetch_state_t *pf = calloc(1, sizeof(*pf));
    if (!pf) return NXS_ERR_ALLOC;
    pf->hint = o->hint;
    pf->max_pages = o->max_pages ? o->max_pages : NXS_PREFETCH_DEFAULT_MAX_PAGES;
    pf->page_size = o->page_size ? o->page_size : NXS_PREFETCH_DEFAULT_PAGE_SIZE;
    pf->coalesce_gap = o->coalesce_gap_pages;
    pf->prefetch_depth = o->prefetch_depth ? o->prefetch_depth : NXS_PREFETCH_DEFAULT_DEPTH;
    r->prefetch = pf;
    return NXS_OK;
}

void nxs_prefetch_destroy(nxs_reader_t *r) {
    if (!r || !r->prefetch) return;
    prefetch_free((nxs_prefetch_state_t *)r->prefetch);
    r->prefetch = NULL;
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

    nxs_prefetch_state_t *pf = (nxs_prefetch_state_t *)r->prefetch;
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
    for (size_t i = 0; i < n_pages; i++) {
        if (!cache_has(pf, indices[i])) {
            if (n_missing < missing_cap) missing[n_missing++] = indices[i];
        }
    }

    nxs_page_range_t ranges[64];
    size_t n_ranges = nxs_coalesce_page_indices(missing, n_missing, pf->coalesce_gap,
                                                pf->page_size, ranges,
                                                sizeof(ranges) / sizeof(ranges[0]));

    for (size_t ri = 0; ri < n_ranges; ri++) {
        nxs_err_t err = fetch_coalesced_range(r, pf, &ranges[ri]);
        if (err != NXS_OK) {
            if (indices != stack_indices) free(indices);
            if (missing != missing_stack) free(missing);
            return err;
        }
    }

    cache_pin_pages(pf, indices, n_pages);
    cache_unpin_all(pf);

    if (indices != stack_indices) free(indices);
    if (missing != missing_stack) free(missing);
    return NXS_OK;
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

    nxs_prefetch_state_t *pf = (nxs_prefetch_state_t *)r->prefetch;
    stats->pages_cached = pf->cache_count;
    stats->pages_max = pf->max_pages;
    stats->cache_hits = pf->hits;
    stats->cache_misses = pf->misses;
    stats->fetches_issued = pf->fetches_issued;
    for (nxs_cached_page_t *e = pf->cache; e; e = e->next)
        stats->memory_used_bytes += (uint64_t)e->data_len;
}
