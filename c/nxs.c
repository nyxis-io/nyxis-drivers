// NXS Reader implementation (C99)
#include "nxs.h"
#include <stdlib.h>
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
#define FLAG_SCHEMA     0x0002u
#define FLAG_COLUMNAR   0x0001u
#define FLAG_PAX        0x0004u
#define MAGIC_PAGE      0x4E585350u /* NXSP */

#define FOOTER_ROW_BYTES       12u
#define FOOTER_COL_BYTES       20u
#define FOOTER_PAX_BYTES       28u
#define COL_TAIL_ENTRY_BYTES   20u
#define PAX_TAIL_ENTRY_BYTES   28u

#define KEY_HT_EMPTY (-1)

static size_t null_bitmap_bytes(uint32_t n) {
    size_t raw = (size_t)((n + 7u) / 8u);
    return (raw + 7u) & ~(size_t)7u;
}

static int col_bit(const uint8_t *bm, uint32_t rec) {
    return (int)((bm[rec / 8u] >> (rec % 8u)) & 1u);
}

static int null_bitmap_dense(const uint8_t *bm, uint32_t n) {
    if (n == 0) return 1;
    uint32_t full = n / 8u;
    uint32_t rem = n % 8u;
    for (uint32_t i = 0; i < full; i++) {
        if (bm[i] != 0xFFu) return 0;
    }
    if (rem == 0) return 1;
    uint8_t mask = (uint8_t)((1u << rem) - 1u);
    return (bm[full] & mask) == mask;
}

static double col_sum_f64_dense(const double *values, uint32_t n) {
    double sum = 0.0;
#if defined(__GNUC__) || defined(__clang__)
    #pragma GCC ivdep
#endif
    for (uint32_t i = 0; i < n; i++) {
        sum += values[i];
    }
    return sum;
}

static nxs_err_t col_field_parts(const nxs_reader_t *r, int slot,
                               const uint8_t **bitmap, size_t *bm_len,
                               const uint8_t **values, size_t *val_len) {
    if (slot < 0 || slot >= r->key_count) return NXS_ERR_KEY_NOT_FOUND;
    uint64_t off = r->col_buf_off[slot];
    uint64_t len = r->col_buf_len[slot];
    if (off + len > r->size) return NXS_ERR_OUT_OF_BOUNDS;
    *bm_len = null_bitmap_bytes(r->record_count);
    if (len < *bm_len) return NXS_ERR_OUT_OF_BOUNDS;
    *bitmap = r->data + off;
    *values = r->data + off + *bm_len;
    *val_len = (size_t)(len - *bm_len);
    return NXS_OK;
}

static int pax_find_page(const nxs_reader_t *r, uint32_t rec, int *local_idx) {
    if (r->page_count == 0) return -1;
    int lo = 0, hi = (int)r->page_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint64_t start = r->page_rec_start[mid];
        uint32_t count = r->page_rec_count[mid];
        if (rec < start) {
            hi = mid - 1;
        } else if (rec >= start + count) {
            lo = mid + 1;
        } else {
            *local_idx = (int)(rec - start);
            return mid;
        }
    }
    return -1;
}

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
    if (cap > NXS_MAX_KEYS * 2) cap = NXS_MAX_KEYS * 2;
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

static void pax_free_pages(nxs_reader_t *r) {
    free(r->page_index);
    free(r->page_rec_start);
    free(r->page_rec_count);
    free(r->page_offset);
    free(r->page_length);
    r->page_index = NULL;
    r->page_rec_start = NULL;
    r->page_rec_count = NULL;
    r->page_offset = NULL;
    r->page_length = NULL;
    r->page_count = 0;
}

nxs_err_t nxs_open(nxs_reader_t *r, const uint8_t *data, size_t size) {
    if (!r || !data || size < 32) return NXS_ERR_OUT_OF_BOUNDS;
    memset(r, 0, sizeof(*r));
    r->data = data;
    r->size = size;
    r->layout = NXS_LAYOUT_ROW;

    if (rd_u32(data) != MAGIC_FILE)   return NXS_ERR_BAD_MAGIC;
    if (rd_u32(data + size - 4) != MAGIC_FOOTER) return NXS_ERR_BAD_MAGIC;

    r->version  = rd_u16(data + 4);
    r->flags    = rd_u16(data + 6);
    r->dict_hash= rd_u64(data + 8);
    r->tail_ptr = rd_u64(data + 16);

    if ((r->flags & FLAG_COLUMNAR) && (r->flags & FLAG_PAX))
        return NXS_ERR_INVALID_FLAGS;
    if ((r->flags & FLAG_COLUMNAR) && r->tail_ptr == 0)
        return NXS_ERR_INCOMPATIBLE;

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

    if (r->flags & FLAG_COLUMNAR) {
        if (size < FOOTER_COL_BYTES) return NXS_ERR_OUT_OF_BOUNDS;
        r->layout = NXS_LAYOUT_COLUMNAR;
        size_t fo = size - FOOTER_COL_BYTES;
        r->tail_ptr = rd_u64(data + fo);
        r->record_count = (uint32_t)rd_u64(data + fo + 8);
        r->tail_start = (size_t)r->tail_ptr;
        for (int i = 0; i < r->key_count; i++) {
            size_t e = r->tail_start + (size_t)i * COL_TAIL_ENTRY_BYTES;
            if (e + COL_TAIL_ENTRY_BYTES > size) return NXS_ERR_OUT_OF_BOUNDS;
            uint16_t fid = rd_u16(data + e);
            if ((int)fid >= r->key_count) return NXS_ERR_OUT_OF_BOUNDS;
            r->col_buf_off[fid] = rd_u64(data + e + 4);
            r->col_buf_len[fid] = rd_u64(data + e + 12);
        }
    } else if (r->flags & FLAG_PAX) {
        if (size < FOOTER_PAX_BYTES) return NXS_ERR_OUT_OF_BOUNDS;
        r->layout = NXS_LAYOUT_PAX;
        size_t fo = size - FOOTER_PAX_BYTES;
        r->tail_ptr = rd_u64(data + fo);
        r->record_count = (uint32_t)rd_u64(data + fo + 8);
        r->page_count = rd_u32(data + fo + 16);
        r->page_size_hint = rd_u32(data + fo + 20);
        r->tail_start = (size_t)r->tail_ptr;
        if (r->page_count > 0) {
            r->page_index = calloc(r->page_count, sizeof(uint32_t));
            r->page_rec_start = calloc(r->page_count, sizeof(uint64_t));
            r->page_rec_count = calloc(r->page_count, sizeof(uint32_t));
            r->page_offset = calloc(r->page_count, sizeof(uint64_t));
            r->page_length = calloc(r->page_count, sizeof(uint32_t));
            if (!r->page_index || !r->page_rec_start || !r->page_rec_count ||
                !r->page_offset || !r->page_length) {
                pax_free_pages(r);
                return NXS_ERR_ALLOC;
            }
            for (uint32_t i = 0; i < r->page_count; i++) {
                size_t e = r->tail_start + (size_t)i * PAX_TAIL_ENTRY_BYTES;
                if (e + PAX_TAIL_ENTRY_BYTES > size) return NXS_ERR_OUT_OF_BOUNDS;
                r->page_index[i] = rd_u32(data + e);
                r->page_rec_start[i] = rd_u64(data + e + 4);
                r->page_rec_count[i] = rd_u32(data + e + 12);
                r->page_offset[i] = rd_u64(data + e + 16);
                r->page_length[i] = rd_u32(data + e + 24);
            }
            for (uint32_t i = 0; i < r->page_count; i++) {
                size_t poff = (size_t)r->page_offset[i];
                if (poff + 4 > size || rd_u32(data + poff) != MAGIC_PAGE)
                    return NXS_ERR_BAD_PAGE_MAGIC;
            }
        }
    } else {
        if (r->tail_ptr == 0) {
            if (size < 44) return NXS_ERR_OUT_OF_BOUNDS;
            r->tail_ptr = rd_u64(data + size - FOOTER_ROW_BYTES);
        }
        size_t tp = (size_t)r->tail_ptr;
        if (tp + 4 > size) return NXS_ERR_OUT_OF_BOUNDS;
        r->record_count = rd_u32(data + tp);
        r->tail_start = tp + 4;
    }

    if (r->key_count > 0) key_index_build(r);
    return NXS_OK;
}

void nxs_close(nxs_reader_t *r) {
    if (r) pax_free_pages(r);
}

uint32_t nxs_record_count(const nxs_reader_t *r) { return r->record_count; }

int nxs_slot(const nxs_reader_t *r, const char *key) {
    if (!key || r->key_count <= 0 || r->key_ht_mask == 0) return -1;
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
    obj->reader = r;
    obj->record_index = i;
    obj->stage = 0;
    if (r->layout == NXS_LAYOUT_ROW) {
        size_t entry = r->tail_start + (size_t)i * 10 + 2;
        if (entry + 8 > r->size) return NXS_ERR_OUT_OF_BOUNDS;
        obj->offset = (size_t)rd_u64(r->data + entry);
    } else {
        obj->offset = (size_t)i;
    }
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
    memset(obj->present, 0, sizeof obj->present);
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
    if (slot >= obj->reader->key_count) return -1;
    if (build_rank_cache(obj) != NXS_OK) return -1;
    if (!obj->present[slot]) return -1;
    const uint8_t *data = obj->reader->data;
    size_t ot = obj->offset_table_start + (size_t)obj->rank[slot] * 2;
    if (ot + 2 > obj->reader->size) return -1;
    uint16_t rel = rd_u16(data + ot);
    return (int64_t)(obj->offset + rel);
}

// ── Typed accessors ───────────────────────────────────────────────────────────

size_t nxs_pax_complete_page_at(const uint8_t *data, size_t size, size_t off,
                                uint16_t field_count) {
    if (!data || off + 28 > size || field_count == 0) return 0;
    if (rd_u32(data + off) != MAGIC_PAGE) return 0;
    uint32_t rc = rd_u32(data + off + 16);
    size_t body = 24;
    for (uint16_t fi = 0; fi < field_count; fi++) {
        body += null_bitmap_bytes(rc) + (size_t)rc * 8u;
    }
    size_t page_len = body + 4;
    size_t aligned = (page_len + 7u) & ~(size_t)7u;
    if (off + page_len > size) return 0;
    if (rd_u32(data + off + page_len - 4) != (uint32_t)page_len) return 0;
    if (off + aligned > size) return 0;
    return aligned;
}

static nxs_err_t pax_page_field_parts_at(const uint8_t *data, size_t size,
                                         size_t poff, int slot,
                                         const uint8_t **bm, size_t *bm_len,
                                         const uint8_t **vals, size_t *val_len,
                                         uint32_t *record_count) {
    if (poff + 24 > size || rd_u32(data + poff) != MAGIC_PAGE)
        return NXS_ERR_BAD_PAGE_MAGIC;
    uint32_t rc = rd_u32(data + poff + 16);
    uint16_t fc = rd_u16(data + poff + 20);
    if (slot < 0 || slot >= (int)fc) return NXS_ERR_OUT_OF_BOUNDS;
    if (fc > (uint16_t)NXS_MAX_KEYS) return NXS_ERR_OUT_OF_BOUNDS;
    size_t bl = null_bitmap_bytes(rc);
    size_t field_stride = bl + (size_t)rc * 8u;
    if (field_stride == 0 || poff + 24 > size) return NXS_ERR_OUT_OF_BOUNDS;
    size_t body = poff + 24 + (size_t)slot * field_stride;
    if (body + bl + (size_t)rc * 8u > size) return NXS_ERR_OUT_OF_BOUNDS;
    *bm = data + body;
    *bm_len = bl;
    *vals = data + body + bl;
    *val_len = (size_t)rc * 8u;
    *record_count = rc;
    return NXS_OK;
}

static double pax_col_sum_f64_slot(const nxs_reader_t *r, int slot) {
    double sum = 0.0;
    for (uint32_t pi = 0; pi < r->page_count; pi++) {
        const uint8_t *bm, *vals;
        size_t bm_len, val_len;
        uint32_t rc;
        size_t poff = (size_t)r->page_offset[pi];
        if (pax_page_field_parts_at(r->data, r->size, poff, slot,
                                    &bm, &bm_len, &vals, &val_len, &rc) != NXS_OK)
            continue;
        for (uint32_t i = 0; i < rc; i++) {
            if (!col_bit(bm, i)) continue;
            size_t off = (size_t)i * 8u;
            if (off + 8 <= val_len) sum += rd_f64(vals + off);
        }
    }
    return sum;
}

static nxs_err_t pax_field_values(const nxs_reader_t *r, uint32_t rec, int slot,
                                  const uint8_t **vals, size_t *val_len,
                                  uint32_t *local_idx) {
    int li = 0;
    int pi = pax_find_page(r, rec, &li);
    if (pi < 0) return NXS_ERR_OUT_OF_BOUNDS;
    size_t poff = (size_t)r->page_offset[pi];
    if (poff + 24 > r->size || rd_u32(r->data + poff) != MAGIC_PAGE)
        return NXS_ERR_BAD_PAGE_MAGIC;
    uint16_t fc = rd_u16(r->data + poff + 20);
    if (slot < 0 || slot >= (int)fc) return NXS_ERR_OUT_OF_BOUNDS;
    uint32_t rc = r->page_rec_count[pi];
    size_t body = poff + 24;
    for (int fi = 0; fi < slot; fi++) {
        size_t bm = null_bitmap_bytes(rc);
        body += bm + (size_t)rc * 8u;
    }
    size_t bm_len = null_bitmap_bytes(rc);
    if (body + bm_len + (size_t)rc * 8u > r->size) return NXS_ERR_OUT_OF_BOUNDS;
    if (!col_bit(r->data + body, li)) return NXS_ERR_FIELD_ABSENT;
    *vals = r->data + body + bm_len + (size_t)li * 8u;
    *val_len = 8;
    *local_idx = (uint32_t)li;
    return NXS_OK;
}

static nxs_err_t col_get_numeric(const nxs_object_t *obj, int slot, int is_f64,
                               int64_t *iout, double *fout) {
    const nxs_reader_t *r = obj->reader;
    const uint8_t *vals;
    size_t val_len;
    uint32_t local = 0;
    nxs_err_t err;
    if (r->layout == NXS_LAYOUT_PAX) {
        err = pax_field_values(r, obj->record_index, slot, &vals, &val_len, &local);
    } else {
        const uint8_t *bm;
        size_t bm_len;
        err = col_field_parts(r, slot, &bm, &bm_len, &vals, &val_len);
        if (err != NXS_OK) return err;
        uint32_t ri = obj->record_index;
        if (ri >= r->record_count) return NXS_ERR_OUT_OF_BOUNDS;
        if (!col_bit(bm, ri)) return NXS_ERR_FIELD_ABSENT;
        vals = vals + (size_t)ri * 8u;
        val_len = 8;
    }
    if (err != NXS_OK) return err;
    uint8_t sig = r->key_sigils[slot];
    if (sig == 0x22 || sig == 0x24 || sig == 0x3c)
        return NXS_ERR_UNSUPPORTED_TYPE;
    if (val_len < 8) return NXS_ERR_OUT_OF_BOUNDS;
    if (is_f64) {
        *fout = rd_f64(vals);
    } else {
        *iout = rd_i64(vals);
    }
    (void)local;
    return NXS_OK;
}

nxs_err_t nxs_get_i64_slot(nxs_object_t *obj, int slot, int64_t *out) {
    if (obj->reader->layout != NXS_LAYOUT_ROW)
        return col_get_numeric(obj, slot, 0, out, NULL);
    int64_t off = nxs_resolve_slot(obj, slot);
    if (off < 0) return NXS_ERR_FIELD_ABSENT;
    if ((size_t)off + 8 > obj->reader->size) return NXS_ERR_OUT_OF_BOUNDS;
    *out = rd_i64(obj->reader->data + off);
    return NXS_OK;
}

nxs_err_t nxs_get_f64_slot(nxs_object_t *obj, int slot, double *out) {
    if (obj->reader->layout != NXS_LAYOUT_ROW)
        return col_get_numeric(obj, slot, 1, NULL, out);
    int64_t off = nxs_resolve_slot(obj, slot);
    if (off < 0) return NXS_ERR_FIELD_ABSENT;
    if ((size_t)off + 8 > obj->reader->size) return NXS_ERR_OUT_OF_BOUNDS;
    *out = rd_f64(obj->reader->data + off);
    return NXS_OK;
}

nxs_err_t nxs_get_bool_slot(nxs_object_t *obj, int slot, int *out) {
    if (obj->reader->layout != NXS_LAYOUT_ROW) {
        int64_t v;
        nxs_err_t e = col_get_numeric(obj, slot, 0, &v, NULL);
        if (e != NXS_OK) return e;
        *out = v != 0;
        return NXS_OK;
    }
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

double nxs_col_sum_f64(const nxs_reader_t *r, const char *field) {
    int slot = nxs_slot(r, field);
    if (slot < 0) return 0.0;
    if (r->layout == NXS_LAYOUT_ROW) return nxs_sum_f64(r, field);
    if (r->layout == NXS_LAYOUT_PAX) return pax_col_sum_f64_slot(r, slot);
    const uint8_t *bm, *vals;
    size_t bm_len, val_len;
    if (col_field_parts(r, slot, &bm, &bm_len, &vals, &val_len) != NXS_OK) return 0.0;
    uint32_t n = r->record_count;
    size_t need = (size_t)n * 8u;
    if (null_bitmap_dense(bm, n) && val_len >= need) {
        return col_sum_f64_dense((const double *)vals, n);
    }
    double sum = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        if (!col_bit(bm, i)) continue;
        size_t off = (size_t)i * 8u;
        if (off + 8 <= val_len) sum += rd_f64(vals + off);
    }
    return sum;
}

double nxs_col_min_f64(const nxs_reader_t *r, const char *field) {
    double m, tmp;
    if (nxs_min_f64(r, field, &tmp) == NXS_OK) return tmp;
    (void)m;
    int slot = nxs_slot(r, field);
    if (slot < 0 || r->layout == NXS_LAYOUT_ROW) return 0.0;
    const uint8_t *bm, *vals;
    size_t bm_len, val_len;
    if (col_field_parts(r, slot, &bm, &bm_len, &vals, &val_len) != NXS_OK) return 0.0;
    int have = 0;
    m = 0.0;
    for (uint32_t i = 0; i < r->record_count; i++) {
        if (!col_bit(bm, i)) continue;
        double v = rd_f64(vals + (size_t)i * 8u);
        if (!have || v < m) { m = v; have = 1; }
    }
    return have ? m : 0.0;
}

double nxs_col_max_f64(const nxs_reader_t *r, const char *field) {
    double m = 0.0;
    if (nxs_max_f64(r, field, &m) == NXS_OK) return m;
    int slot = nxs_slot(r, field);
    if (slot < 0 || r->layout == NXS_LAYOUT_ROW) return 0.0;
    const uint8_t *bm, *vals;
    size_t bm_len, val_len;
    if (col_field_parts(r, slot, &bm, &bm_len, &vals, &val_len) != NXS_OK) return 0.0;
    int have = 0;
    for (uint32_t i = 0; i < r->record_count; i++) {
        if (!col_bit(bm, i)) continue;
        double v = rd_f64(vals + (size_t)i * 8u);
        if (!have || v > m) { m = v; have = 1; }
    }
    return have ? m : 0.0;
}

int64_t nxs_col_sum_i64(const nxs_reader_t *r, const char *field) {
    int slot = nxs_slot(r, field);
    if (slot < 0) return 0;
    if (r->layout == NXS_LAYOUT_ROW) return nxs_sum_i64(r, field);
    const uint8_t *bm, *vals;
    size_t bm_len, val_len;
    if (col_field_parts(r, slot, &bm, &bm_len, &vals, &val_len) != NXS_OK) return 0;
    int64_t sum = 0;
    for (uint32_t i = 0; i < r->record_count; i++) {
        if (!col_bit(bm, i)) continue;
        sum += rd_i64(vals + (size_t)i * 8u);
    }
    return sum;
}

const void* nxs_col_buffer(const nxs_reader_t *r, const char *field, size_t *out_len) {
    int slot = nxs_slot(r, field);
    if (slot < 0 || r->layout == NXS_LAYOUT_ROW) { *out_len = 0; return NULL; }
    const uint8_t *bm, *vals;
    size_t bm_len, val_len;
    if (col_field_parts(r, slot, &bm, &bm_len, &vals, &val_len) != NXS_OK) {
        *out_len = 0; return NULL;
    }
    *out_len = val_len;
    return vals;
}

const uint8_t* nxs_col_null_bitmap(const nxs_reader_t *r, const char *field, size_t *out_len) {
    int slot = nxs_slot(r, field);
    if (slot < 0 || r->layout == NXS_LAYOUT_ROW) { *out_len = 0; return NULL; }
    const uint8_t *bm, *vals;
    size_t bm_len, val_len;
    if (col_field_parts(r, slot, &bm, &bm_len, &vals, &val_len) != NXS_OK) {
        *out_len = 0; return NULL;
    }
    (void)vals;
    *out_len = bm_len;
    return bm;
}

nxs_page_t* nxs_page_first(const nxs_reader_t *r) {
    if (r->layout != NXS_LAYOUT_PAX || r->page_count == 0) return NULL;
    nxs_page_t *p = calloc(1, sizeof(nxs_page_t));
    if (!p) return NULL;
    p->reader = r;
    p->page_idx = 0;
    return p;
}

nxs_page_t* nxs_page_next(nxs_page_t *page) {
    if (!page) return NULL;
    if ((uint32_t)(page->page_idx + 1) >= page->reader->page_count) {
        nxs_page_free(page);
        return NULL;
    }
    page->page_idx++;
    return page;
}

void nxs_page_free(nxs_page_t *page) { free(page); }

const void* nxs_page_col_buffer(nxs_page_t *page, const char *field, size_t *out_len) {
    *out_len = 0;
    if (!page || page->reader->layout != NXS_LAYOUT_PAX) return NULL;
    int slot = nxs_slot(page->reader, field);
    if (slot < 0) return NULL;
    const nxs_reader_t *r = page->reader;
    int pi = page->page_idx;
    size_t poff = (size_t)r->page_offset[pi];
    if (poff + 24 > r->size) return NULL;
    if (rd_u32(r->data + poff) != MAGIC_PAGE) return NULL;
    uint16_t fc = rd_u16(r->data + poff + 20);
    if (slot < 0 || slot >= (int)fc) return NULL;
    uint32_t rc = r->page_rec_count[pi];
    size_t body = poff + 24;
    for (int fi = 0; fi < slot; fi++) {
        size_t bm = null_bitmap_bytes(rc);
        body += bm + (size_t)rc * 8u;
    }
    size_t bm = null_bitmap_bytes(rc);
    if (body + bm + (size_t)rc * 8u > r->size) return NULL;
    *out_len = (size_t)rc * 8u;
    return r->data + body + bm;
}

double nxs_sum_f64(const nxs_reader_t *r, const char *key) {
    if (r->layout != NXS_LAYOUT_ROW) return nxs_col_sum_f64(r, key);
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

// ── PAX stream reader ─────────────────────────────────────────────────────────

static void pax_stream_free_pages(nxs_pax_stream_reader_t *sr) {
    free(sr->page_index);
    free(sr->page_rec_start);
    free(sr->page_rec_count);
    free(sr->page_offset);
    free(sr->page_length);
    sr->page_index = NULL;
    sr->page_rec_start = NULL;
    sr->page_rec_count = NULL;
    sr->page_offset = NULL;
    sr->page_length = NULL;
    sr->page_count = 0;
    sr->records_available = 0;
}

static int pax_stream_grow_page(nxs_pax_stream_reader_t *sr, uint32_t page_index,
                                uint64_t rec_start, uint32_t rec_count,
                                uint64_t page_off, uint32_t page_len) {
    uint32_t n = sr->page_count + 1;
    size_t bytes = (size_t)n;
    uint32_t *pi = malloc(bytes * sizeof(uint32_t));
    uint64_t *rs = malloc(bytes * sizeof(uint64_t));
    uint32_t *rc = malloc(bytes * sizeof(uint32_t));
    uint64_t *po = malloc(bytes * sizeof(uint64_t));
    uint32_t *pl = malloc(bytes * sizeof(uint32_t));
    if (!pi || !rs || !rc || !po || !pl) {
        free(pi);
        free(rs);
        free(rc);
        free(po);
        free(pl);
        return 0;
    }
    if (sr->page_count > 0) {
        memcpy(pi, sr->page_index, (size_t)sr->page_count * sizeof(uint32_t));
        memcpy(rs, sr->page_rec_start, (size_t)sr->page_count * sizeof(uint64_t));
        memcpy(rc, sr->page_rec_count, (size_t)sr->page_count * sizeof(uint32_t));
        memcpy(po, sr->page_offset, (size_t)sr->page_count * sizeof(uint64_t));
        memcpy(pl, sr->page_length, (size_t)sr->page_count * sizeof(uint32_t));
    }
    free(sr->page_index);
    free(sr->page_rec_start);
    free(sr->page_rec_count);
    free(sr->page_offset);
    free(sr->page_length);
    sr->page_index = pi;
    sr->page_rec_start = rs;
    sr->page_rec_count = rc;
    sr->page_offset = po;
    sr->page_length = pl;
    uint32_t i = sr->page_count;
    sr->page_index[i] = page_index;
    sr->page_rec_start[i] = rec_start;
    sr->page_rec_count[i] = rec_count;
    sr->page_offset[i] = page_off;
    sr->page_length[i] = page_len;
    sr->page_count = n;
    sr->records_available += rec_count;
    return 1;
}

static int pax_stream_detect_sealed(const uint8_t *data, size_t size, uint64_t *tail_index_off) {
    if (size < FOOTER_PAX_BYTES) return 0;
    if (rd_u32(data + size - 4) != MAGIC_FOOTER) return 0;
    uint64_t tp = rd_u64(data + size - FOOTER_PAX_BYTES);
    if (tp == 0 || tp >= size || size - tp < FOOTER_PAX_BYTES) return 0;
    *tail_index_off = tp;
    return 1;
}

static nxs_err_t pax_stream_load_sealed_tail(nxs_pax_stream_reader_t *sr, uint64_t tail_off) {
    pax_stream_free_pages(sr);
    const uint8_t *data = sr->data;
    size_t size = sr->size;
    size_t fo = size - FOOTER_PAX_BYTES;
    uint32_t pc = rd_u32(data + fo + 16);
    sr->scan_cursor = size;
    if (pc == 0) {
        sr->sealed = 1;
        return NXS_OK;
    }
    sr->page_index = calloc(pc, sizeof(uint32_t));
    sr->page_rec_start = calloc(pc, sizeof(uint64_t));
    sr->page_rec_count = calloc(pc, sizeof(uint32_t));
    sr->page_offset = calloc(pc, sizeof(uint64_t));
    sr->page_length = calloc(pc, sizeof(uint32_t));
    if (!sr->page_index || !sr->page_rec_start || !sr->page_rec_count ||
        !sr->page_offset || !sr->page_length) {
        pax_stream_free_pages(sr);
        return NXS_ERR_ALLOC;
    }
    sr->page_count = pc;
    sr->records_available = 0;
    for (uint32_t i = 0; i < pc; i++) {
        size_t e = (size_t)tail_off + (size_t)i * PAX_TAIL_ENTRY_BYTES;
        if (e + PAX_TAIL_ENTRY_BYTES > size) return NXS_ERR_OUT_OF_BOUNDS;
        sr->page_index[i] = rd_u32(data + e);
        sr->page_rec_start[i] = rd_u64(data + e + 4);
        sr->page_rec_count[i] = rd_u32(data + e + 12);
        sr->page_offset[i] = rd_u64(data + e + 16);
        sr->page_length[i] = rd_u32(data + e + 24);
        sr->records_available += sr->page_rec_count[i];
    }
    sr->sealed = 1;
    return NXS_OK;
}

nxs_err_t nxs_pax_stream_open(nxs_pax_stream_reader_t *sr,
                              const uint8_t *data, size_t size) {
    if (!sr || !data || size < 32) return NXS_ERR_OUT_OF_BOUNDS;
    memset(sr, 0, sizeof(*sr));
    sr->data = data;
    sr->size = size;
    if (rd_u32(data) != MAGIC_FILE) return NXS_ERR_BAD_MAGIC;
    if ((rd_u16(data + 6) & FLAG_PAX) == 0) return NXS_ERR_INVALID_FLAGS;
    if (rd_u64(data + 16) != 0)
        return NXS_ERR_INCOMPATIBLE;
    sr->version = rd_u16(data + 4);
    sr->flags = rd_u16(data + 6);
    sr->dict_hash = rd_u64(data + 8);
    if (!(sr->flags & FLAG_SCHEMA)) return NXS_ERR_OUT_OF_BOUNDS;
    size_t off = 32;
    if (off + 2 > size) return NXS_ERR_OUT_OF_BOUNDS;
    uint16_t kc = rd_u16(data + off);
    off += 2;
    if (kc > NXS_MAX_KEYS) return NXS_ERR_OUT_OF_BOUNDS;
    if (off + kc > size) return NXS_ERR_OUT_OF_BOUNDS;
    memcpy(sr->key_sigils, data + off, kc);
    off += kc;
    sr->key_count = (int)kc;
    char *pool = sr->_pool;
    size_t pool_used = 0;
    for (int i = 0; i < sr->key_count; i++) {
        const uint8_t *start = data + off;
        while (off < size && data[off] != 0) off++;
        if (off >= size) return NXS_ERR_OUT_OF_BOUNDS;
        size_t len = (size_t)(data + off - start);
        if (pool_used + len + 1 > sizeof(sr->_pool)) return NXS_ERR_OUT_OF_BOUNDS;
        memcpy(pool + pool_used, start, len);
        pool[pool_used + len] = '\0';
        sr->keys[i] = pool + pool_used;
        pool_used += len + 1;
        off++;
    }
    size_t schema_end = (off + 7) & ~(size_t)7;
    if (murmur3_64(data + 32, schema_end - 32) != sr->dict_hash)
        return NXS_ERR_DICT_MISMATCH;
    sr->data_start = schema_end;
    sr->scan_cursor = schema_end;
    uint64_t tail_off = 0;
    if (pax_stream_detect_sealed(data, size, &tail_off)) {
        nxs_err_t err = pax_stream_load_sealed_tail(sr, tail_off);
        if (err != NXS_OK) return err;
    }
    return NXS_OK;
}

void nxs_pax_stream_close(nxs_pax_stream_reader_t *sr) {
    if (sr) pax_stream_free_pages(sr);
}

uint32_t nxs_pax_stream_poll(nxs_pax_stream_reader_t *sr) {
    if (!sr) return 0;
    uint32_t before = sr->page_count;
    const uint8_t *data = sr->data;
    size_t size = sr->size;
    uint64_t tail_off = 0;
    if (!sr->sealed && pax_stream_detect_sealed(data, size, &tail_off)) {
        if (pax_stream_load_sealed_tail(sr, tail_off) == NXS_OK)
            return sr->page_count - before;
    }
    if (sr->sealed) return 0;
    uint16_t fc = (uint16_t)sr->key_count;
    while (sr->scan_cursor + 28 <= size) {
        if (rd_u32(data + sr->scan_cursor) != MAGIC_PAGE) break;
        size_t plen = nxs_pax_complete_page_at(data, size, sr->scan_cursor, fc);
        if (plen == 0) break;
        uint32_t pidx = rd_u32(data + sr->scan_cursor + 4);
        uint64_t rstart = rd_u64(data + sr->scan_cursor + 8);
        uint32_t rc = rd_u32(data + sr->scan_cursor + 16);
        size_t bl = null_bitmap_bytes(rc);
        size_t field_stride = bl + (size_t)rc * 8u;
        size_t body = 24 + (size_t)fc * field_stride;
        if (body + 4 > size || body + 4 > 0xFFFFFFFFu) break;
        uint32_t page_len = (uint32_t)(body + 4);
        if (!pax_stream_grow_page(sr, pidx, rstart, rc, sr->scan_cursor, page_len))
            break;
        sr->scan_cursor += plen;
    }
    return sr->page_count - before;
}

int nxs_pax_stream_is_sealed(const nxs_pax_stream_reader_t *sr) {
    return sr && sr->sealed;
}

uint32_t nxs_pax_stream_page_count(const nxs_pax_stream_reader_t *sr) {
    return sr ? sr->page_count : 0;
}

uint64_t nxs_pax_stream_records_available(const nxs_pax_stream_reader_t *sr) {
    return sr ? sr->records_available : 0;
}

double nxs_pax_stream_col_sum_f64(const nxs_pax_stream_reader_t *sr, const char *field) {
    if (!sr) return 0.0;
    int slot = -1;
    for (int i = 0; i < sr->key_count; i++) {
        if (strcmp(sr->keys[i], field) == 0) { slot = i; break; }
    }
    if (slot < 0) return 0.0;
    double sum = 0.0;
    for (uint32_t pi = 0; pi < sr->page_count; pi++) {
        const uint8_t *bm, *vals;
        size_t bm_len, val_len;
        uint32_t rc;
        size_t poff = (size_t)sr->page_offset[pi];
        if (pax_page_field_parts_at(sr->data, sr->size, poff, slot,
                                    &bm, &bm_len, &vals, &val_len, &rc) != NXS_OK)
            continue;
        for (uint32_t i = 0; i < rc; i++) {
            if (!col_bit(bm, i)) continue;
            size_t off = (size_t)i * 8u;
            if (off + 8 <= val_len) sum += rd_f64(vals + off);
        }
    }
    return sum;
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
