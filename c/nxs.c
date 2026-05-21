// NXS Reader implementation (C99)
#include "nxs.h"
#include <string.h>

// ── Little-endian helpers (no UB — memcpy) ────────────────────────────────────

static inline uint16_t rd_u16(const uint8_t *p) {
    uint16_t v; memcpy(&v, p, 2); return v;  /* host is LE on all target archs */
}
static inline uint32_t rd_u32(const uint8_t *p) {
    uint32_t v; memcpy(&v, p, 4); return v;
}
static inline uint64_t rd_u64(const uint8_t *p) {
    uint64_t v; memcpy(&v, p, 8); return v;
}
static inline int64_t rd_i64(const uint8_t *p) {
    int64_t v; memcpy(&v, p, 8); return v;
}
static inline double rd_f64(const uint8_t *p) {
    double v; memcpy(&v, p, 8); return v;
}

// ── Constants ─────────────────────────────────────────────────────────────────

#define MAGIC_FILE   0x4E595842u
#define MAGIC_OBJ    0x4E59584Fu
#define MAGIC_FOOTER 0x2153584Eu
#define FLAG_SCHEMA  0x0002u

#define KEY_HT_EMPTY (-1)

// ── Key index (FNV-1a + open addressing) ─────────────────────────────────────

static uint32_t key_hash(const char *s) {
    uint32_t h = 2166136261u;
    for (unsigned char c = (unsigned char)*s; c; c = (unsigned char)*++s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

static void key_index_build(nxs_reader_t *r) {
    int cap = 64;
    while (cap < r->key_count * 2) cap *= 2;
    if (cap > NXS_MAX_KEYS) cap = NXS_MAX_KEYS;
    r->key_ht_mask = (uint16_t)(cap - 1);
    for (int i = 0; i < cap; i++) r->key_ht[i] = KEY_HT_EMPTY;
    for (int i = 0; i < r->key_count; i++) {
        uint32_t h = key_hash(r->keys[i]);
        int pos = (int)(h & r->key_ht_mask);
        while (r->key_ht[pos] != KEY_HT_EMPTY) {
            pos = (pos + 1) & (int)r->key_ht_mask;
        }
        r->key_ht[pos] = (int16_t)i;
    }
}

// ── MurmurHash3-64 (schema integrity check) ───────────────────────────────────

static uint64_t murmur3_64(const uint8_t *data, size_t len) {
    uint64_t h = 0x93681D6255313A99ULL;
    size_t i = 0;
    while (i < len) {
        uint64_t k = 0;
        for (int b = 0; b < 8 && i + (size_t)b < len; b++)
            k |= (uint64_t)data[i + b] << (b * 8);
        k *= 0xFF51AFD7ED558CCDULL;
        k ^= k >> 33;
        h ^= k;
        h *= 0xC4CEB9FE1A85EC53ULL;
        h ^= h >> 33;
        i += 8;
    }
    h ^= (uint64_t)len;
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33;
    return h;
}

// ── Open / close ──────────────────────────────────────────────────────────────

nxs_err_t nxs_open(nxs_reader_t *r, const uint8_t *data, size_t size) {
    if (!r || !data || size < 32) return NXS_ERR_OUT_OF_BOUNDS;
    memset(r, 0, sizeof(*r));
    r->data = data;
    r->size = size;

    if (rd_u32(data) != MAGIC_FILE)   return NXS_ERR_BAD_MAGIC;
    if (rd_u32(data + size - 4) != MAGIC_FOOTER) return NXS_ERR_BAD_MAGIC;

    r->version  = rd_u16(data + 4);
    r->flags    = rd_u16(data + 6);
    r->dict_hash= rd_u64(data + 8);
    r->tail_ptr = rd_u64(data + 16);
    if (r->tail_ptr == 0) {
        if (size < 44) return NXS_ERR_OUT_OF_BOUNDS;
        r->tail_ptr = rd_u64(data + size - 12);
    }

    // Schema (Flags bit 1 set)
    if (r->flags & FLAG_SCHEMA) {
        size_t off = 32;
        if (off + 2 > size) return NXS_ERR_OUT_OF_BOUNDS;
        uint16_t kc = rd_u16(data + off); off += 2;
        if (kc > NXS_MAX_KEYS) kc = NXS_MAX_KEYS;
        if (off + kc > size) return NXS_ERR_OUT_OF_BOUNDS;
        memcpy(r->key_sigils, data + off, kc);
        off += kc;
        r->key_count = (int)kc;
        char *pool = r->_pool;
        size_t pool_used = 0;
        for (int i = 0; i < r->key_count; i++) {
            const uint8_t *start = data + off;
            while (off < size && data[off] != 0) off++;
            if (off >= size) return NXS_ERR_OUT_OF_BOUNDS;
            size_t len = (size_t)(data + off - start);
            if (pool_used + len + 1 > sizeof(r->_pool)) return NXS_ERR_OUT_OF_BOUNDS;
            memcpy(pool + pool_used, start, len);
            pool[pool_used + len] = '\0';
            r->keys[i] = pool + pool_used;
            pool_used += len + 1;
            off++; // skip NUL
        }
        // Pad to 8-byte boundary
        size_t schema_end = (off + 7) & ~(size_t)7;
        uint64_t computed = murmur3_64(data + 32, schema_end - 32);
        if (computed != r->dict_hash) return NXS_ERR_DICT_MISMATCH;
    }

    // Tail-index
    size_t tp = (size_t)r->tail_ptr;
    if (tp + 4 > size) return NXS_ERR_OUT_OF_BOUNDS;
    r->record_count = rd_u32(data + tp);
    r->tail_start   = tp + 4;
    if (r->key_count > 0) key_index_build(r);
    return NXS_OK;
}

void nxs_close(nxs_reader_t *r) { (void)r; }

uint32_t nxs_record_count(const nxs_reader_t *r) { return r->record_count; }

int nxs_slot(const nxs_reader_t *r, const char *key) {
    if (r->key_count <= 0 || r->key_ht_mask == 0) return -1;
    uint32_t h = key_hash(key);
    int pos = (int)(h & r->key_ht_mask);
    while (r->key_ht[pos] != KEY_HT_EMPTY) {
        int slot = (int)r->key_ht[pos];
        if (strcmp(r->keys[slot], key) == 0) return slot;
        pos = (pos + 1) & (int)r->key_ht_mask;
    }
    return -1;
}

// ── Object ────────────────────────────────────────────────────────────────────

nxs_err_t nxs_record(const nxs_reader_t *r, uint32_t i, nxs_object_t *obj) {
    if (i >= r->record_count) return NXS_ERR_OUT_OF_BOUNDS;
    size_t entry = r->tail_start + (size_t)i * 10 + 2;
    if (entry + 8 > r->size) return NXS_ERR_OUT_OF_BOUNDS;
    uint64_t abs_off = rd_u64(r->data + entry);
    obj->reader = r;
    obj->offset = (size_t)abs_off;
    obj->stage = 0;
    return NXS_OK;
}

static nxs_err_t locate_bitmask(nxs_object_t *obj) {
    if (obj->stage >= 1) return NXS_OK;
    const uint8_t *data = obj->reader->data;
    size_t p = obj->offset;
    if (p + 8 > obj->reader->size) return NXS_ERR_OUT_OF_BOUNDS;
    if (rd_u32(data + p) != MAGIC_OBJ) return NXS_ERR_BAD_MAGIC;
    p += 8;
    obj->bitmask_start = p;
    while (p < obj->reader->size && (data[p] & 0x80)) p++;
    if (p >= obj->reader->size) return NXS_ERR_OUT_OF_BOUNDS;
    p++;
    obj->offset_table_start = p;
    obj->stage = 1;
    return NXS_OK;
}

static nxs_err_t build_rank_cache(nxs_object_t *obj) {
    if (obj->stage >= 2) return NXS_OK;
    nxs_err_t err = locate_bitmask(obj);
    if (err != NXS_OK) return err;

    const nxs_reader_t *r = obj->reader;
    const uint8_t *data = r->data;
    int kc = r->key_count;
    size_t p = obj->bitmask_start;
    int slot = 0;
    memset(obj->present, 0, (size_t)kc);
    while (slot < kc) {
        if (p >= r->size) return NXS_ERR_OUT_OF_BOUNDS;
        uint8_t b = data[p++];
        uint8_t bits = b & 0x7F;
        for (int i = 0; i < 7 && slot < kc; i++) {
            obj->present[slot] = (bits >> i) & 1;
            slot++;
        }
        if (!(b & 0x80)) break;
    }
    uint16_t acc = 0;
    for (int i = 0; i < kc; i++) {
        obj->rank[i] = acc;
        acc += (uint16_t)obj->present[i];
    }
    obj->rank[kc] = acc;
    obj->stage = 2;
    return NXS_OK;
}

nxs_err_t nxs_stage_object(nxs_object_t *obj) {
    return build_rank_cache(obj);
}

int64_t nxs_resolve_slot(nxs_object_t *obj, int slot) {
    if (slot < 0) return -1;
    if (build_rank_cache(obj) != NXS_OK) return -1;
    if (!obj->present[slot]) return -1;
    const uint8_t *data = obj->reader->data;
    size_t ot = obj->offset_table_start + (size_t)obj->rank[slot] * 2;
    if (ot + 2 > obj->reader->size) return -1;
    uint16_t rel = rd_u16(data + ot);
    return (int64_t)(obj->offset + rel);
}

// ── Typed accessors ───────────────────────────────────────────────────────────

nxs_err_t nxs_get_i64_slot(nxs_object_t *obj, int slot, int64_t *out) {
    int64_t off = nxs_resolve_slot(obj, slot);
    if (off < 0) return NXS_ERR_FIELD_ABSENT;
    if ((size_t)off + 8 > obj->reader->size) return NXS_ERR_OUT_OF_BOUNDS;
    *out = rd_i64(obj->reader->data + off);
    return NXS_OK;
}

nxs_err_t nxs_get_f64_slot(nxs_object_t *obj, int slot, double *out) {
    int64_t off = nxs_resolve_slot(obj, slot);
    if (off < 0) return NXS_ERR_FIELD_ABSENT;
    if ((size_t)off + 8 > obj->reader->size) return NXS_ERR_OUT_OF_BOUNDS;
    *out = rd_f64(obj->reader->data + off);
    return NXS_OK;
}

nxs_err_t nxs_get_bool_slot(nxs_object_t *obj, int slot, int *out) {
    int64_t off = nxs_resolve_slot(obj, slot);
    if (off < 0) return NXS_ERR_FIELD_ABSENT;
    if ((size_t)off >= obj->reader->size) return NXS_ERR_OUT_OF_BOUNDS;
    *out = obj->reader->data[off] != 0;
    return NXS_OK;
}

nxs_err_t nxs_get_str_slot(nxs_object_t *obj, int slot, char *buf, size_t buf_len) {
    int64_t off = nxs_resolve_slot(obj, slot);
    if (off < 0) return NXS_ERR_FIELD_ABSENT;
    const uint8_t *data = obj->reader->data;
    size_t sz = obj->reader->size;
    if ((size_t)off + 4 > sz) return NXS_ERR_OUT_OF_BOUNDS;
    uint32_t len = rd_u32(data + off);
    if ((size_t)off + 4 + len > sz) return NXS_ERR_OUT_OF_BOUNDS;
    size_t copy = (len < buf_len - 1) ? len : buf_len - 1;
    memcpy(buf, data + off + 4, copy);
    buf[copy] = '\0';
    return NXS_OK;
}

nxs_err_t nxs_get_i64(nxs_object_t *obj, const char *key, int64_t *out) {
    return nxs_get_i64_slot(obj, nxs_slot(obj->reader, key), out);
}
nxs_err_t nxs_get_f64(nxs_object_t *obj, const char *key, double *out) {
    return nxs_get_f64_slot(obj, nxs_slot(obj->reader, key), out);
}
nxs_err_t nxs_get_bool(nxs_object_t *obj, const char *key, int *out) {
    return nxs_get_bool_slot(obj, nxs_slot(obj->reader, key), out);
}
nxs_err_t nxs_get_str(nxs_object_t *obj, const char *key, char *buf, size_t buf_len) {
    return nxs_get_str_slot(obj, nxs_slot(obj->reader, key), buf, buf_len);
}

// ── Bulk reducers (allocation-free) ──────────────────────────────────────────

static int64_t scan_offset_bulk(const uint8_t *data, size_t obj_off, int slot) {
    size_t p = obj_off + 8; // skip Magic + Length
    int cur = 0, table_idx = 0;
    uint8_t b = 0;
    int found = 0;
    while (1) {
        b = data[p++];
        uint8_t bits = b & 0x7F;
        for (int i = 0; i < 7; i++) {
            if (cur == slot) {
                if (!((bits >> i) & 1)) return -1;
                found = 1;
            } else if (cur < slot && ((bits >> i) & 1)) {
                table_idx++;
            }
            cur++;
        }
        if (found && !(b & 0x80)) break;
        if (cur > slot && found) break;
        if (!(b & 0x80)) return -1;
    }
    while (b & 0x80) b = data[p++];
    uint16_t rel; memcpy(&rel, data + p + table_idx * 2, 2);
    return (int64_t)(obj_off + rel);
}

double nxs_sum_f64(const nxs_reader_t *r, const char *key) {
    int slot = nxs_slot(r, key);
    if (slot < 0) return 0.0;
    const uint8_t *data = r->data;
    double sum = 0.0;
    for (uint32_t i = 0; i < r->record_count; i++) {
        size_t entry = r->tail_start + (size_t)i * 10 + 2;
        size_t abs = (size_t)rd_u64(data + entry);
        int64_t off = scan_offset_bulk(data, abs, slot);
        if (off >= 0) sum += rd_f64(data + off);
    }
    return sum;
}

int64_t nxs_sum_i64(const nxs_reader_t *r, const char *key) {
    int slot = nxs_slot(r, key);
    if (slot < 0) return 0;
    const uint8_t *data = r->data;
    int64_t sum = 0;
    for (uint32_t i = 0; i < r->record_count; i++) {
        size_t entry = r->tail_start + (size_t)i * 10 + 2;
        size_t abs = (size_t)rd_u64(data + entry);
        int64_t off = scan_offset_bulk(data, abs, slot);
        if (off >= 0) sum += rd_i64(data + off);
    }
    return sum;
}

nxs_err_t nxs_min_f64(const nxs_reader_t *r, const char *key, double *out) {
    int slot = nxs_slot(r, key);
    if (slot < 0) return NXS_ERR_KEY_NOT_FOUND;
    const uint8_t *data = r->data;
    double m = 0.0;
    int have = 0;
    for (uint32_t i = 0; i < r->record_count; i++) {
        size_t entry = r->tail_start + (size_t)i * 10 + 2;
        size_t abs = (size_t)rd_u64(data + entry);
        int64_t off = scan_offset_bulk(data, abs, slot);
        if (off < 0) continue;
        double v = rd_f64(data + off);
        if (!have || v < m) { m = v; have = 1; }
    }
    if (!have) return NXS_ERR_FIELD_ABSENT;
    *out = m;
    return NXS_OK;
}

nxs_err_t nxs_max_f64(const nxs_reader_t *r, const char *key, double *out) {
    int slot = nxs_slot(r, key);
    if (slot < 0) return NXS_ERR_KEY_NOT_FOUND;
    const uint8_t *data = r->data;
    double m = 0.0;
    int have = 0;
    for (uint32_t i = 0; i < r->record_count; i++) {
        size_t entry = r->tail_start + (size_t)i * 10 + 2;
        size_t abs = (size_t)rd_u64(data + entry);
        int64_t off = scan_offset_bulk(data, abs, slot);
        if (off < 0) continue;
        double v = rd_f64(data + off);
        if (!have || v > m) { m = v; have = 1; }
    }
    if (!have) return NXS_ERR_FIELD_ABSENT;
    *out = m;
    return NXS_OK;
}

// ── Query engine ──────────────────────────────────────────────────────────────

static NxsPred make_pred(const nxs_reader_t *r, const char *key, NxsOp op, NxsValue val) {
    NxsPred p;
    p.slot  = nxs_slot(r, key);
    p.op    = op;
    p.value = val;
    return p;
}

NxsPred nxs_pred_eq_bool(const nxs_reader_t *r, const char *key, int val) {
    NxsValue v; v.type = NXS_VAL_BOOL; v.u.bool_val = val ? 1 : 0;
    return make_pred(r, key, NXS_OP_EQ, v);
}
NxsPred nxs_pred_eq_str(const nxs_reader_t *r, const char *key, const char *val, size_t len) {
    NxsValue v; v.type = NXS_VAL_STR; v.u.str_val.ptr = val; v.u.str_val.len = len;
    return make_pred(r, key, NXS_OP_EQ, v);
}
NxsPred nxs_pred_eq_i64(const nxs_reader_t *r, const char *key, int64_t val) {
    NxsValue v; v.type = NXS_VAL_I64; v.u.i64_val = val;
    return make_pred(r, key, NXS_OP_EQ, v);
}
NxsPred nxs_pred_eq_f64(const nxs_reader_t *r, const char *key, double val) {
    NxsValue v; v.type = NXS_VAL_F64; v.u.f64_val = val;
    return make_pred(r, key, NXS_OP_EQ, v);
}
NxsPred nxs_pred_gt_f64(const nxs_reader_t *r, const char *key, double val) {
    NxsValue v; v.type = NXS_VAL_F64; v.u.f64_val = val;
    return make_pred(r, key, NXS_OP_GT, v);
}
NxsPred nxs_pred_lt_f64(const nxs_reader_t *r, const char *key, double val) {
    NxsValue v; v.type = NXS_VAL_F64; v.u.f64_val = val;
    return make_pred(r, key, NXS_OP_LT, v);
}
NxsPred nxs_pred_gt_i64(const nxs_reader_t *r, const char *key, int64_t val) {
    NxsValue v; v.type = NXS_VAL_I64; v.u.i64_val = val;
    return make_pred(r, key, NXS_OP_GT, v);
}
NxsPred nxs_pred_lt_i64(const nxs_reader_t *r, const char *key, int64_t val) {
    NxsValue v; v.type = NXS_VAL_I64; v.u.i64_val = val;
    return make_pred(r, key, NXS_OP_LT, v);
}

/* Test a single predicate against a record at objOffset in r->data. */
static int pred_test(const NxsPred *p, const nxs_reader_t *r, size_t obj_off) {
    if (p->slot < 0) return 0;
    int64_t foff = scan_offset_bulk(r->data, obj_off, p->slot);
    if (foff < 0) return 0;
    const uint8_t *data = r->data;
    size_t sz = r->size;

    switch (p->value.type) {
    case NXS_VAL_BOOL: {
        if ((size_t)foff >= sz) return 0;
        int got = data[foff] != 0;
        return p->op == NXS_OP_EQ && got == (p->value.u.bool_val != 0);
    }
    case NXS_VAL_I64: {
        if ((size_t)foff + 8 > sz) return 0;
        int64_t got = rd_i64(data + foff);
        switch (p->op) {
        case NXS_OP_EQ:  return got == p->value.u.i64_val;
        case NXS_OP_GT:  return got >  p->value.u.i64_val;
        case NXS_OP_LT:  return got <  p->value.u.i64_val;
        case NXS_OP_GTE: return got >= p->value.u.i64_val;
        case NXS_OP_LTE: return got <= p->value.u.i64_val;
        default: return 0;
        }
    }
    case NXS_VAL_F64: {
        if ((size_t)foff + 8 > sz) return 0;
        double got = rd_f64(data + foff);
        switch (p->op) {
        case NXS_OP_EQ:  return got == p->value.u.f64_val;
        case NXS_OP_GT:  return got >  p->value.u.f64_val;
        case NXS_OP_LT:  return got <  p->value.u.f64_val;
        case NXS_OP_GTE: return got >= p->value.u.f64_val;
        case NXS_OP_LTE: return got <= p->value.u.f64_val;
        default: return 0;
        }
    }
    case NXS_VAL_STR: {
        if ((size_t)foff + 4 > sz) return 0;
        uint32_t wire_len = rd_u32(data + foff);
        if ((size_t)foff + 4 + wire_len > sz) return 0;
        if (p->op != NXS_OP_EQ) return 0;
        if (wire_len != (uint32_t)p->value.u.str_val.len) return 0;
        return memcmp(data + foff + 4, p->value.u.str_val.ptr, wire_len) == 0;
    }
    default:
        return 0;
    }
}

void nxs_query_init(NxsQuery *q, const nxs_reader_t *r, const NxsPred *preds, int npreds) {
    q->reader = r;
    q->preds  = preds;
    q->npreds = npreds;
    q->index  = 0;
}

int nxs_query_next(NxsQuery *q, nxs_object_t *out) {
    const nxs_reader_t *r = q->reader;
    while (q->index < r->record_count) {
        uint32_t i = q->index++;
        size_t entry = r->tail_start + (size_t)i * 10 + 2;
        if (entry + 8 > r->size) continue;
        size_t abs_off = (size_t)rd_u64(r->data + entry);

        /* AND-combine all predicates */
        int match = 1;
        for (int j = 0; j < q->npreds; j++) {
            if (!pred_test(&q->preds[j], r, abs_off)) { match = 0; break; }
        }
        if (!match) continue;

        out->reader = r;
        out->offset = abs_off;
        out->stage = 0;
        return 1;
    }
    return 0;
}

uint32_t nxs_query_count(NxsQuery *q) {
    uint32_t n = 0;
    nxs_object_t obj;
    while (nxs_query_next(q, &obj)) n++;
    return n;
}
