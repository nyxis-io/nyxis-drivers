// Adaptive prefetch — Phase 1: sync viewport prefetch + LRU page cache (spec §6–§7.2).
#pragma once
#include "nxs.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXS_PREFETCH_DEFAULT_MAX_PAGES       128u
#define NXS_PREFETCH_DEFAULT_PAGE_SIZE       65536u
#define NXS_PREFETCH_DEFAULT_COALESCE_GAP    1u
#define NXS_PREFETCH_DEFAULT_DEPTH           4u

typedef enum {
    NXS_HINT_UNKNOWN    = 0,
    NXS_HINT_SEQUENTIAL = 1,
    NXS_HINT_RANDOM     = 2,
    NXS_HINT_FULL       = 3,
    NXS_HINT_PARTIAL    = 4,
} nxs_access_hint_t;

typedef struct {
    nxs_access_hint_t hint;
    uint32_t          max_pages;
    uint32_t          page_size;
    uint32_t          coalesce_gap_pages;
    uint32_t          prefetch_depth;
} nxs_open_options_t;

typedef struct {
    uint32_t     pages_cached;
    uint32_t     pages_max;
    uint64_t     memory_used_bytes;
    uint64_t     cache_hits;
    uint64_t     cache_misses;
    uint64_t     fetches_issued;
    const char  *strategy;
    const char  *pattern;
} nxs_cache_stats_t;

typedef struct {
    uint32_t page_start;
    uint32_t page_end;
    uint64_t byte_start;
    uint64_t byte_length;
} nxs_page_range_t;

/** Set defaults: hint=UNKNOWN, max_pages=128, page_size=65536, coalesce_gap=1. */
void nxs_open_options_init(nxs_open_options_t *opts);

/** Open reader with prefetch open-options (Phase 1). NULL opts → defaults. */
nxs_err_t nxs_open_ex(nxs_reader_t *r, const uint8_t *data, size_t size,
                      const nxs_open_options_t *opts);

/** Called from nxs_open / nxs_open_ex; not required for callers using nxs_open only. */
nxs_err_t nxs_prefetch_init(nxs_reader_t *r, const nxs_open_options_t *opts);
void          nxs_prefetch_destroy(nxs_reader_t *r);

/**
 * Synchronous viewport prefetch for row-layout files (§7.2, §9.3).
 * Issues coalesced range reads; no background threads.
 */
nxs_err_t nxs_prefetch_viewport(nxs_reader_t *r, uint32_t start_index, uint32_t end_index);

/** Diagnostic cache / prefetch counters. */
void nxs_cache_stats(const nxs_reader_t *r, nxs_cache_stats_t *stats);

/**
 * Coalesce sorted unique page indices into byte ranges (§7.2).
 * Returns number of ranges written to `out` (0 if out_cap == 0).
 */
size_t nxs_coalesce_page_indices(const uint32_t *indices, size_t count,
                                 uint32_t gap_pages, uint32_t page_size,
                                 nxs_page_range_t *out, size_t out_cap);

#ifdef __cplusplus
}
#endif
