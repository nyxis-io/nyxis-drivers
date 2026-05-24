// Adaptive prefetch — Phase 2: pattern detector, strategies, eager background (spec §4–§8.4).
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

#define NXS_PREFETCH_SEQ_THRESHOLD           10u
#define NXS_PREFETCH_RANDOM_THRESHOLD        100u
#define NXS_PREFETCH_HISTORY_SIZE            32u
#define NXS_PREFETCH_MIN_OBSERVATIONS        8u
#define NXS_PREFETCH_UPGRADE_SEQ_THRESHOLD   100u
#define NXS_PREFETCH_EAGER_THRESHOLD_MB      10u
#define NXS_PREFETCH_LAZY_THRESHOLD_MB       50u

typedef enum {
    NXS_HINT_UNKNOWN    = 0,
    NXS_HINT_SEQUENTIAL = 1,
    NXS_HINT_RANDOM     = 2,
    NXS_HINT_FULL       = 3,
    NXS_HINT_PARTIAL    = 4,
} nxs_access_hint_t;

typedef enum {
    NXS_PATTERN_UNKNOWN = 0,
    NXS_PATTERN_SEQUENTIAL,
    NXS_PATTERN_RANDOM,
    NXS_PATTERN_MIXED,
} nxs_access_pattern_t;

typedef enum {
    NXS_STRATEGY_LAZY = 0,
    NXS_STRATEGY_ADAPTIVE,
    NXS_STRATEGY_EAGER,
} nxs_prefetch_strategy_t;

typedef struct {
    int64_t  accesses[NXS_PREFETCH_HISTORY_SIZE];
    uint32_t write_pos;
    uint32_t filled;
    uint32_t sequential_runs;
    uint32_t random_jumps;
    int64_t  last_index;
} nxs_access_pattern_detector_t;

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

void nxs_open_options_init(nxs_open_options_t *opts);
nxs_prefetch_strategy_t nxs_initial_strategy(nxs_access_hint_t hint, size_t file_size);
const char *nxs_strategy_name(nxs_prefetch_strategy_t s);
const char *nxs_pattern_name(nxs_access_pattern_t p);

void nxs_pattern_detector_init(nxs_access_pattern_detector_t *d);
void nxs_pattern_detector_observe(nxs_access_pattern_detector_t *d, uint32_t index);
nxs_access_pattern_t nxs_pattern_detector_pattern(const nxs_access_pattern_detector_t *d);
uint32_t nxs_pattern_detector_sequential_runs(const nxs_access_pattern_detector_t *d);
size_t nxs_pattern_detector_predict_next(const nxs_access_pattern_detector_t *d,
                                         uint32_t depth, uint32_t record_count,
                                         uint32_t *out, size_t out_cap);

nxs_err_t nxs_open_ex(nxs_reader_t *r, const uint8_t *data, size_t size,
                      const nxs_open_options_t *opts);
nxs_err_t nxs_prefetch_init(nxs_reader_t *r, const nxs_open_options_t *opts);
void          nxs_prefetch_destroy(nxs_reader_t *r);
void          nxs_prefetch_on_access(nxs_reader_t *r, uint32_t index);
void          nxs_pause_prefetch(nxs_reader_t *r);
void          nxs_resume_prefetch(nxs_reader_t *r);
void          nxs_prefetch_set_cache_limit(nxs_reader_t *r, size_t max_bytes);
void          nxs_warmup(nxs_reader_t *r);
nxs_err_t nxs_prefetch_viewport(nxs_reader_t *r, uint32_t start_index, uint32_t end_index);
void nxs_cache_stats(const nxs_reader_t *r, nxs_cache_stats_t *stats);
size_t nxs_coalesce_page_indices(const uint32_t *indices, size_t count,
                                 uint32_t gap_pages, uint32_t page_size,
                                 nxs_page_range_t *out, size_t out_cap);

#ifdef __cplusplus
}
#endif
