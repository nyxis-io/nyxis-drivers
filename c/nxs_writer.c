// NXS Writer — direct-to-buffer .nxb emitter for C99.
#include "nxs_writer.h"
#include <stdlib.h>
#include <string.h>

#define MAGIC_FILE    0x4E595842u
#define MAGIC_OBJ     0x4E59584Fu
#define MAGIC_LIST    0x4E59584Cu
#define MAGIC_FOOTER  0x2153584Eu
#define VERSION       0x0101u
#define FLAG_SCHEMA   0x0002u

// ── MurmurHash3-64 ────────────────────────────────────────────────────────────

static uint64_t murmur3_64(const uint8_t *data, size_t len)
{
    const uint64_t C1   = 0xFF51AFD7ED558CCDull;
    const uint64_t C2   = 0xC4CEB9FE1A85EC53ull;
    uint64_t h = 0x93681D6255313A99ull;
    for (size_t i = 0; i < len; i += 8) {
        uint64_t k = 0;
        for (int b = 0; b < 8 && i + (size_t)b < len; b++)
            k |= (uint64_t)data[i + b] << (b * 8);
        k  = k * C1;
        k ^= k >> 33;
        h ^= k;
        h  = h * C2;
        h ^= h >> 33;
    }
    h ^= (uint64_t)len;
    h ^= h >> 33;
    h  = h * C1;
    h ^= h >> 33;
    return h;
}

// ── Little-endian write helpers ───────────────────────────────────────────────

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

static void put_u64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) { p[i] = (uint8_t)(v >> (i * 8)); }
}

static void put_i64(uint8_t *p, int64_t v) { put_u64(p, (uint64_t)v); }

static void put_f64(uint8_t *p, double v)
{
    uint64_t bits;
    memcpy(&bits, &v, 8);
    put_u64(p, bits);
}

// ── Buffer helpers ────────────────────────────────────────────────────────────

static int buf_grow(nxs_writer_t *w, size_t need)
{
    if (w->buf_pos + need <= w->buf_len) return 0;
    size_t newlen = w->buf_len * 2;
    while (newlen < w->buf_pos + need) newlen *= 2;
    uint8_t *nb = (uint8_t *)realloc(w->buf, newlen);
    if (!nb) return -1;
    w->buf     = nb;
    w->buf_len = newlen;
    return 0;
}

static int buf_append(nxs_writer_t *w, const uint8_t *data, size_t n)
{
    if (buf_grow(w, n) != 0) return -1;
    memcpy(w->buf + w->buf_pos, data, n);
    w->buf_pos += n;
    return 0;
}

static int buf_append_zero(nxs_writer_t *w, size_t n)
{
    if (buf_grow(w, n) != 0) return -1;
    memset(w->buf + w->buf_pos, 0, n);
    w->buf_pos += n;
    return 0;
}

// ── Init / free ───────────────────────────────────────────────────────────────

int nxs_writer_init(nxs_writer_t *w, const char **keys, int key_count,
                    size_t initial_cap)
{
    memset(w, 0, sizeof(*w));
    if (key_count > NXS_WRITER_MAX_KEYS) return -1;

    w->key_count    = key_count;
    w->bitmask_bytes = (key_count + 6) / 7;
    if (w->bitmask_bytes < 1) w->bitmask_bytes = 1;
    for (int i = 0; i < key_count; i++) w->keys[i] = keys[i];

    if (initial_cap < 64) initial_cap = 64;
    w->buf = (uint8_t *)malloc(initial_cap);
    if (!w->buf) return -1;
    w->buf_len = initial_cap;

    w->record_cap    = 256;
    w->record_offsets = (uint32_t *)malloc(w->record_cap * sizeof(uint32_t));
    if (!w->record_offsets) { free(w->buf); w->buf = NULL; return -1; }

    return 0;
}

void nxs_writer_free(nxs_writer_t *w)
{
    free(w->buf);            w->buf            = NULL;
    free(w->record_offsets); w->record_offsets = NULL;
    free(w->out);            w->out            = NULL;
    w->buf_len = w->buf_pos = w->out_size = 0;
}

void nxs_writer_reset(nxs_writer_t *w)
{
    w->buf_pos      = 0;
    w->record_count = 0;
    w->frame_depth  = 0;
    free(w->out); w->out = NULL; w->out_size = 0;
}

// ── Object lifetime ───────────────────────────────────────────────────────────

int nxs_writer_begin_object(nxs_writer_t *w)
{
    if (w->frame_depth >= NXS_WRITER_MAX_DEPTH) return -1;

    if (w->frame_depth == 0) {
        // Grow record_offsets if needed
        if (w->record_count >= w->record_cap) {
            int newcap = w->record_cap * 2;
            uint32_t *nr = (uint32_t *)realloc(w->record_offsets,
                                                newcap * sizeof(uint32_t));
            if (!nr) return -1;
            w->record_offsets = nr;
            w->record_cap     = newcap;
        }
        w->record_offsets[w->record_count++] = (uint32_t)w->buf_pos;
    }

    int d = w->frame_depth++;
    memset(&w->frames[d], 0, sizeof(w->frames[d]));
    w->frames[d].start        = w->buf_pos;
    w->frames[d].bitmask_bytes = w->bitmask_bytes;
    w->frames[d].last_slot    = -1;

    // Set LEB128 continuation bits
    for (int i = 0; i < w->bitmask_bytes - 1; i++)
        w->frames[d].bitmask[i] = 0x80;

    // Magic (4) + length placeholder (4)
    uint8_t header[8] = {0};
    put_u32(header, MAGIC_OBJ);
    if (buf_append(w, header, 8) != 0) return -1;

    // Bitmask placeholder
    if (buf_append(w, w->frames[d].bitmask, (size_t)w->bitmask_bytes) != 0) return -1;

    // Offset table placeholder (u16 per key)
    if (buf_append_zero(w, (size_t)w->key_count * 2) != 0) return -1;

    // Align data area to 8 bytes from object start
    while ((w->buf_pos - w->frames[d].start) % 8 != 0) {
        uint8_t z = 0;
        if (buf_append(w, &z, 1) != 0) return -1;
    }
    return 0;
}


int nxs_writer_end_object(nxs_writer_t *w)
{
    if (w->frame_depth <= 0) return -1;
    int d = --w->frame_depth;
    size_t total_len = w->buf_pos - w->frames[d].start;

    // Back-patch Length
    put_u32(w->buf + w->frames[d].start + 4, (uint32_t)total_len);

    // Back-patch bitmask
    size_t bm_off = w->frames[d].start + 8;
    memcpy(w->buf + bm_off, w->frames[d].bitmask, (size_t)w->bitmask_bytes);

    // Back-patch offset table
    size_t ot_start = bm_off + (size_t)w->bitmask_bytes;
    int    present  = w->frames[d].present_count;

    if (!w->frames[d].needs_sort) {
        for (int i = 0; i < present; i++) {
            put_u16(w->buf + ot_start + i * 2,
                    w->frames[d].offset_table[i]);
        }
    } else {
        // Sort by slot_order, then write offsets in slot order
        // slot_order[i] stores the slot index for offset_table[i]
        // Simple insertion sort (small N)
        for (int i = 1; i < present; i++) {
            int ks = w->frames[d].slot_order[i];
            uint16_t kv = w->frames[d].offset_table[i];
            int j = i - 1;
            while (j >= 0 && w->frames[d].slot_order[j] > ks) {
                w->frames[d].slot_order[j + 1]   = w->frames[d].slot_order[j];
                w->frames[d].offset_table[j + 1] = w->frames[d].offset_table[j];
                j--;
            }
            w->frames[d].slot_order[j + 1]   = ks;
            w->frames[d].offset_table[j + 1] = kv;
        }
        for (int i = 0; i < present; i++) {
            put_u16(w->buf + ot_start + i * 2,
                    w->frames[d].offset_table[i]);
        }
    }

    // Zero unused offset table slots
    for (int i = present * 2; i < w->key_count * 2; i++) {
        w->buf[ot_start + i] = 0;
    }
    return 0;
}

// ── Slot marking ─────────────────────────────────────────────────────────────

static int mark_slot(nxs_writer_t *w, int slot)
{
    if (w->frame_depth <= 0) return -1;
    int d = w->frame_depth - 1;

    int byte_idx = slot / 7;
    int bit_idx  = slot % 7;
    w->frames[d].bitmask[byte_idx] |= (uint8_t)(1 << bit_idx);

    uint16_t rel = (uint16_t)(w->buf_pos - w->frames[d].start);
    int      pc  = w->frames[d].present_count;

    if (slot < w->frames[d].last_slot) w->frames[d].needs_sort = 1;
    w->frames[d].last_slot = slot;

    w->frames[d].offset_table[pc] = rel;
    w->frames[d].slot_order[pc]   = slot;
    w->frames[d].present_count++;
    return 0;
}

// ── Typed write methods ───────────────────────────────────────────────────────

int nxs_write_i64(nxs_writer_t *w, int slot, int64_t v)
{
    if (mark_slot(w, slot) != 0) return -1;
    uint8_t b[8]; put_i64(b, v);
    return buf_append(w, b, 8);
}

int nxs_write_f64(nxs_writer_t *w, int slot, double v)
{
    if (mark_slot(w, slot) != 0) return -1;
    uint8_t b[8]; put_f64(b, v);
    return buf_append(w, b, 8);
}

int nxs_write_bool(nxs_writer_t *w, int slot, int v)
{
    if (mark_slot(w, slot) != 0) return -1;
    uint8_t b[8]; memset(b, 0, 8); b[0] = v ? 0x01 : 0x00;
    return buf_append(w, b, 8);
}

int nxs_write_time(nxs_writer_t *w, int slot, int64_t unix_ns)
{
    return nxs_write_i64(w, slot, unix_ns);
}

int nxs_write_null(nxs_writer_t *w, int slot)
{
    if (mark_slot(w, slot) != 0) return -1;
    return buf_append_zero(w, 8);
}

int nxs_write_str(nxs_writer_t *w, int slot, const char *s, uint32_t len)
{
    if (mark_slot(w, slot) != 0) return -1;
    uint8_t lbuf[4]; put_u32(lbuf, len);
    if (buf_append(w, lbuf, 4) != 0) return -1;
    if (buf_append(w, (const uint8_t *)s, len) != 0) return -1;
    size_t used = (4 + len) % 8;
    if (used != 0) return buf_append_zero(w, 8 - used);
    return 0;
}

int nxs_write_bytes(nxs_writer_t *w, int slot, const uint8_t *data, uint32_t len)
{
    if (mark_slot(w, slot) != 0) return -1;
    uint8_t lbuf[4]; put_u32(lbuf, len);
    if (buf_append(w, lbuf, 4) != 0) return -1;
    if (buf_append(w, data, len) != 0) return -1;
    size_t used = (4 + len) % 8;
    if (used != 0) return buf_append_zero(w, 8 - used);
    return 0;
}

// ── Finish ────────────────────────────────────────────────────────────────────

int nxs_writer_finish(nxs_writer_t *w)
{
    if (w->frame_depth != 0) return -1;

    // Build schema header
    int    n    = w->key_count;
    size_t size = (size_t)(2 + n);
    for (int i = 0; i < n; i++) size += strlen(w->keys[i]) + 1;
    size_t padded = size + (((8 - size % 8)) % 8);

    uint8_t *schema = (uint8_t *)calloc(padded, 1);
    if (!schema) return -1;

    size_t p = 0;
    schema[p++] = (uint8_t)(n & 0xFF);
    schema[p++] = (uint8_t)((n >> 8) & 0xFF);
    for (int i = 0; i < n; i++) schema[p++] = 0x22; // '"' sigil
    for (int i = 0; i < n; i++) {
        size_t klen = strlen(w->keys[i]);
        memcpy(schema + p, w->keys[i], klen); p += klen;
        schema[p++] = 0x00;
    }

    uint64_t dict_hash      = murmur3_64(schema, padded);
    uint64_t data_start_abs = 32 + padded;
    uint64_t tail_ptr       = data_start_abs + w->buf_pos;

    // Build tail-index
    int      nr    = w->record_count;
    size_t   tsz   = (size_t)(4 + nr * 10 + 12);
    uint8_t *tail  = (uint8_t *)calloc(tsz, 1);
    if (!tail) { free(schema); return -1; }

    size_t tp = 0;
    put_u32(tail + tp, (uint32_t)nr); tp += 4;
    for (int i = 0; i < nr; i++) {
        put_u16(tail + tp, (uint16_t)i); tp += 2;
        put_u64(tail + tp, data_start_abs + w->record_offsets[i]); tp += 8;
    }
    put_u64(tail + tp, tail_ptr); tp += 8;
    put_u32(tail + tp, MAGIC_FOOTER);

    // Assemble output
    size_t total = 32 + padded + w->buf_pos + tsz;
    w->out = (uint8_t *)malloc(total);
    if (!w->out) { free(schema); free(tail); return -1; }
    w->out_size = total;

    uint8_t *op = w->out;

    // Preamble
    put_u32(op, MAGIC_FILE);  op += 4;
    put_u16(op, VERSION);     op += 2;
    put_u16(op, FLAG_SCHEMA); op += 2;
    put_u64(op, dict_hash);   op += 8;
    put_u64(op, 0);           op += 8;
    memset(op, 0, 8);         op += 8; // reserved

    memcpy(op, schema, padded); op += padded;
    memcpy(op, w->buf, w->buf_pos); op += w->buf_pos;
    memcpy(op, tail, tsz);

    free(schema);
    free(tail);
    return 0;
}
