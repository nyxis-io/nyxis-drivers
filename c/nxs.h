// NXS Reader — zero-copy .nxb parser for C99
// Implements the Nyxis v1.1 binary wire format spec.
//
// Usage:
//   nxs_reader_t r;
//   if (nxs_open(&r, data, size) != NXS_OK) { ... }
//   nxs_object_t obj;
//   nxs_record(&r, 42, &obj);
//   int64_t id; nxs_get_i64(&obj, "id", &id);
//   nxs_close(&r);
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Error codes ───────────────────────────────────────────────────────────────
typedef enum {
    NXS_OK                 = 0,
    NXS_ERR_BAD_MAGIC      = 1,
    NXS_ERR_OUT_OF_BOUNDS  = 2,
    NXS_ERR_KEY_NOT_FOUND  = 3,
    NXS_ERR_FIELD_ABSENT   = 4,
    NXS_ERR_ALLOC          = 5,
    NXS_ERR_DICT_MISMATCH  = 6,
    NXS_ERR_INVALID_FLAGS  = 0x10,
    NXS_ERR_INCOMPATIBLE   = 0x11,
    NXS_ERR_UNSUPPORTED    = 0x12,
    NXS_ERR_UNSUPPORTED_FLAGS = 0x14,
    NXS_ERR_BAD_PAGE_MAGIC = 0x13,
    NXS_ERR_UNSUPPORTED_TYPE = 0x15,
} nxs_err_t;

typedef enum {
    NXS_LAYOUT_ROW = 0,
    NXS_LAYOUT_COLUMNAR = 1,
    NXS_LAYOUT_PAX = 2,
} nxs_layout_t;

// ── Reader ────────────────────────────────────────────────────────────────────
#define NXS_MAX_KEYS 256

typedef struct {
    const uint8_t *data;
    size_t         size;
    uint16_t       version;
    uint16_t       flags;
    uint64_t       dict_hash;
    uint64_t       tail_ptr;

    // schema
    int            key_count;
    char          *keys[NXS_MAX_KEYS];
    uint8_t        key_sigils[NXS_MAX_KEYS];

    // tail-index / layout
    uint32_t       record_count;
    size_t         tail_start;
    nxs_layout_t   layout;

    // columnar: per-field buffer location (indexed by slot)
    uint64_t       col_buf_off[NXS_MAX_KEYS];
    uint64_t       col_buf_len[NXS_MAX_KEYS];
    uint8_t        col_warmed[NXS_MAX_KEYS];
    uint32_t       col_fetches;

    // PAX: page table (heap-allocated in nxs_open, freed in nxs_close)
    uint32_t       page_count;
    uint32_t       page_size_hint;
    uint32_t      *page_index;
    uint64_t      *page_rec_start;
    uint32_t      *page_rec_count;
    uint64_t      *page_offset;
    uint32_t      *page_length;

    // O(1) key name → slot (open-addressing; built at nxs_open)
    uint16_t       key_ht_mask;
    int16_t        key_ht[NXS_MAX_KEYS * 2];

    // scratch for key string copies
    char           _pool[NXS_MAX_KEYS * 64];

    // adaptive prefetch (Phase 1); see nxs_prefetch.h
    struct nxs_prefetch_state *prefetch;
} nxs_reader_t;

// Open a reader over a memory-mapped / pre-loaded buffer.
// The buffer must remain valid for the lifetime of the reader.
nxs_err_t nxs_open(nxs_reader_t *r, const uint8_t *data, size_t size);

// Release any resources held by the reader.
// Frees the PAX page-table arrays (page_index, page_rec_start, page_rec_count,
// page_offset, page_length) allocated by nxs_open for PAX-layout files.
// Safe to call on row and columnar readers (no-op when no pages were allocated).
void nxs_close(nxs_reader_t *r);

// Total number of top-level records.
uint32_t nxs_record_count(const nxs_reader_t *r);

/** Wait for eager / in-flight background prefetch (requires nxs_open_ex). */
void nxs_warmup(nxs_reader_t *r);

/** Stop scheduling speculative and eager prefetch (§8.1). */
void nxs_pause_prefetch(nxs_reader_t *r);

/** Re-enable speculative prefetch after nxs_pause_prefetch. */
void nxs_resume_prefetch(nxs_reader_t *r);

/** Prefetch one column buffer (columnar layout only; §7.4). */
nxs_err_t nxs_prefetch_column(nxs_reader_t *r, const char *field);

/** Cap page-cache resident memory; evicts unpinned pages (requires nxs_open_ex). */
void nxs_reader_set_cache_limit(nxs_reader_t *r, size_t max_bytes);

// Resolve a key name to its integer slot index.
// Returns -1 if not found.
int nxs_slot(const nxs_reader_t *r, const char *key);

// ── Object ────────────────────────────────────────────────────────────────────
typedef struct {
    const nxs_reader_t *reader;
    size_t              offset;     // row: NYXO offset; columnar/PAX: record index
    size_t              bitmask_start;
    size_t              offset_table_start;
    uint8_t             stage;      // 0=raw, 1=bitmask located, 2=present+rank cache
    uint8_t             present[NXS_MAX_KEYS];
    uint16_t            rank[NXS_MAX_KEYS + 1];
    uint32_t            record_index; // columnar/PAX record index
} nxs_object_t;

// Populate `obj` with a lazy view of record `i`.
nxs_err_t nxs_record(const nxs_reader_t *r, uint32_t i, nxs_object_t *obj);

// Decode bitmask + build present/rank cache (idempotent; also runs on first field access).
nxs_err_t nxs_stage_object(nxs_object_t *obj);

// Resolve slot to absolute byte offset of its value, or -1 if absent.
// Locates the bitmask/offset-table on first call.
int64_t   nxs_resolve_slot(nxs_object_t *obj, int slot);

// ── Typed accessors ───────────────────────────────────────────────────────────
// All return NXS_OK on success, NXS_ERR_FIELD_ABSENT if the field is not
// present, or NXS_ERR_KEY_NOT_FOUND if the key is unknown.

nxs_err_t nxs_get_i64 (nxs_object_t *obj, const char *key, int64_t  *out);
nxs_err_t nxs_get_f64 (nxs_object_t *obj, const char *key, double   *out);
nxs_err_t nxs_get_bool(nxs_object_t *obj, const char *key, int      *out);
// Writes a NUL-terminated string into `buf` (max `buf_len` bytes incl. NUL).
nxs_err_t nxs_get_str (nxs_object_t *obj, const char *key, char *buf, size_t buf_len);
// Copies up to `buf_len` bytes; sets *out_len to bytes written (may be < field length).
nxs_err_t nxs_get_binary(nxs_object_t *obj, const char *key, uint8_t *buf, size_t buf_len,
                          size_t *out_len);

// Slot variants (skip key lookup — call nxs_slot() once, reuse).
nxs_err_t nxs_get_i64_slot (nxs_object_t *obj, int slot, int64_t  *out);
nxs_err_t nxs_get_f64_slot (nxs_object_t *obj, int slot, double   *out);
nxs_err_t nxs_get_bool_slot(nxs_object_t *obj, int slot, int      *out);
nxs_err_t nxs_get_str_slot (nxs_object_t *obj, int slot, char *buf, size_t buf_len);
nxs_err_t nxs_get_binary_slot(nxs_object_t *obj, int slot, uint8_t *buf, size_t buf_len,
                              size_t *out_len);

// ── Bulk reducers ─────────────────────────────────────────────────────────────
double  nxs_sum_f64(const nxs_reader_t *r, const char *key);
int64_t nxs_sum_i64(const nxs_reader_t *r, const char *key);
// Returns NXS_OK and sets *out; returns NXS_ERR_FIELD_ABSENT if no records.
nxs_err_t nxs_min_f64(const nxs_reader_t *r, const char *key, double *out);
nxs_err_t nxs_max_f64(const nxs_reader_t *r, const char *key, double *out);

/* ── Query engine ─────────────────────────────────────────────────────────── */

typedef enum {
    NXS_OP_EQ  = 0,
    NXS_OP_GT  = 1,
    NXS_OP_LT  = 2,
    NXS_OP_GTE = 3,
    NXS_OP_LTE = 4,
} NxsOp;

typedef enum {
    NXS_VAL_BOOL = 0,
    NXS_VAL_I64  = 1,
    NXS_VAL_F64  = 2,
    NXS_VAL_STR  = 3,
} NxsValType;

typedef struct {
    NxsValType type;
    union {
        int     bool_val;
        int64_t i64_val;
        double  f64_val;
        struct { const char *ptr; size_t len; } str_val;
    } u;
} NxsValue;

typedef struct {
    int      slot;   /* pre-resolved key slot (-1 = not found) */
    NxsOp    op;
    NxsValue value;
} NxsPred;

/* Predicate constructors — resolve key to slot immediately */
NxsPred nxs_pred_eq_bool(const nxs_reader_t *r, const char *key, int val);
NxsPred nxs_pred_eq_str (const nxs_reader_t *r, const char *key, const char *val, size_t len);
NxsPred nxs_pred_eq_i64 (const nxs_reader_t *r, const char *key, int64_t val);
NxsPred nxs_pred_eq_f64 (const nxs_reader_t *r, const char *key, double val);
NxsPred nxs_pred_gt_f64 (const nxs_reader_t *r, const char *key, double val);
NxsPred nxs_pred_lt_f64 (const nxs_reader_t *r, const char *key, double val);
NxsPred nxs_pred_gt_i64 (const nxs_reader_t *r, const char *key, int64_t val);
NxsPred nxs_pred_lt_i64 (const nxs_reader_t *r, const char *key, int64_t val);

/* Iterator state — stack-allocated, no heap */
typedef struct {
    const nxs_reader_t *reader;
    const NxsPred      *preds;  /* array of predicates (AND-combined) */
    int                 npreds;
    uint32_t            index;  /* current position in tail-index */
} NxsQuery;

/* Initialize a query. Pass preds=NULL / npreds=0 to iterate all records. */
void nxs_query_init(NxsQuery *q, const nxs_reader_t *r, const NxsPred *preds, int npreds);

/* Advance to the next matching record. Returns 1 on success, 0 when exhausted. */
int nxs_query_next(NxsQuery *q, nxs_object_t *out);

/* Count matching records (consumes the iterator). */
uint32_t nxs_query_count(NxsQuery *q);

/* ── Columnar / PAX OLAP API (OLAP.md) ─────────────────────────────────────── */

double  nxs_col_sum_f64(const nxs_reader_t *r, const char *field);
double  nxs_col_min_f64(const nxs_reader_t *r, const char *field);
double  nxs_col_max_f64(const nxs_reader_t *r, const char *field);
int64_t nxs_col_sum_i64(const nxs_reader_t *r, const char *field);

const void*   nxs_col_buffer(const nxs_reader_t *r, const char *field, size_t *out_len);
const uint8_t* nxs_col_null_bitmap(const nxs_reader_t *r, const char *field, size_t *out_len);

/**
 * Zero-copy string (`"`) or binary (`<`) column in columnar/PAX layout.
 * `bitmap`: null bitmap; `offsets`: (N+1)×4 u32 LE; `values`: concatenated payload.
 * Returns NXS_ERR_UNSUPPORTED on row or PAX layout, NXS_ERR_UNSUPPORTED_TYPE on numeric fields.
 */
nxs_err_t nxs_col_var_buffer(const nxs_reader_t *r, const char *field,
                             const uint8_t **bitmap, size_t *bitmap_len,
                             const uint8_t **offsets, size_t *offsets_len,
                             const uint8_t **values, size_t *values_len);

typedef struct nxs_page nxs_page_t;
struct nxs_page {
    const nxs_reader_t *reader;
    int                  page_idx;
};

nxs_page_t* nxs_page_first(const nxs_reader_t *r);
nxs_page_t* nxs_page_next(nxs_page_t *page);
const void* nxs_page_col_buffer(nxs_page_t *page, const char *field, size_t *out_len);
void        nxs_page_free(nxs_page_t *page);

/* ── PAX streaming (OLAP.md §4.5, TailPtr=0 until seal) ─────────────────────── */

/**
 * Returns the 8-byte-aligned byte length of a complete NXSP page at `off`,
 * or 0 if the page header is present but the full page is not yet in `data`.
 * `field_count` must match the schema (flat-8 fixed cells per field).
 */
size_t nxs_pax_complete_page_at(const uint8_t *data, size_t size, size_t off,
                                uint16_t field_count);

typedef struct {
    const uint8_t *data;
    size_t         size;
    uint16_t       version;
    uint16_t       flags;
    uint64_t       dict_hash;
    int            key_count;
    char          *keys[NXS_MAX_KEYS];
    uint8_t        key_sigils[NXS_MAX_KEYS];

    size_t         data_start;
    size_t         scan_cursor;
    int            sealed;

    uint32_t       page_count;
    uint32_t       page_capacity;
    uint64_t       records_available;
    uint32_t      *page_index;
    uint64_t      *page_rec_start;
    uint32_t      *page_rec_count;
    uint64_t      *page_offset;
    uint32_t      *page_length;

    char           _pool[NXS_MAX_KEYS * 64];
} nxs_pax_stream_reader_t;

/** Open an unsealed PAX stream (`FLAG_PAX`, preamble `TailPtr == 0`). */
nxs_err_t nxs_pax_stream_open(nxs_pax_stream_reader_t *sr,
                              const uint8_t *data, size_t size);

/** Release heap page tables allocated by the stream reader. */
void nxs_pax_stream_close(nxs_pax_stream_reader_t *sr);

/** Scan for newly complete pages after the buffer grows. Returns new page count. */
uint32_t nxs_pax_stream_poll(nxs_pax_stream_reader_t *sr);

int      nxs_pax_stream_is_sealed(const nxs_pax_stream_reader_t *sr);
uint32_t nxs_pax_stream_page_count(const nxs_pax_stream_reader_t *sr);
uint64_t nxs_pax_stream_records_available(const nxs_pax_stream_reader_t *sr);

/** Sum f64 field across all records in complete pages only. */
double nxs_pax_stream_col_sum_f64(const nxs_pax_stream_reader_t *sr, const char *field);

#ifdef __cplusplus
}
#endif
