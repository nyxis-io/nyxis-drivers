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
} nxs_err_t;

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

    // tail-index
    uint32_t       record_count;
    size_t         tail_start;

    // scratch for key string copies
    char           _pool[NXS_MAX_KEYS * 64];
} nxs_reader_t;

// Open a reader over a memory-mapped / pre-loaded buffer.
// The buffer must remain valid for the lifetime of the reader.
nxs_err_t nxs_open(nxs_reader_t *r, const uint8_t *data, size_t size);

// Release any resources held by the reader (currently a no-op; provided
// for API symmetry with implementations that allocate).
void nxs_close(nxs_reader_t *r);

// Total number of top-level records.
uint32_t nxs_record_count(const nxs_reader_t *r);

// Resolve a key name to its integer slot index.
// Returns -1 if not found.
int nxs_slot(const nxs_reader_t *r, const char *key);

// ── Object ────────────────────────────────────────────────────────────────────
typedef struct {
    const nxs_reader_t *reader;
    size_t              offset;     // absolute offset of NYXO magic
    size_t              bitmask_start;
    size_t              offset_table_start;
    int                 staged;     // 0=raw, 1=bitmask located
} nxs_object_t;

// Populate `obj` with a lazy view of record `i`.
nxs_err_t nxs_record(const nxs_reader_t *r, uint32_t i, nxs_object_t *obj);

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

// Slot variants (skip key lookup — call nxs_slot() once, reuse).
nxs_err_t nxs_get_i64_slot (nxs_object_t *obj, int slot, int64_t  *out);
nxs_err_t nxs_get_f64_slot (nxs_object_t *obj, int slot, double   *out);
nxs_err_t nxs_get_bool_slot(nxs_object_t *obj, int slot, int      *out);
nxs_err_t nxs_get_str_slot (nxs_object_t *obj, int slot, char *buf, size_t buf_len);

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

#ifdef __cplusplus
}
#endif
