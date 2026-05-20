/*
 * nxs_ext.c — MRI Ruby C extension for the NXS binary format reader.
 *
 * Exposes two classes under the Nxs module:
 *   Nxs::CReader(bytes)   — parses preamble/schema/tail-index
 *   Nxs::CObject          — returned by CReader#record(i); decodes fields
 *
 * Design mirrors py/_nxs.c:
 *   - CReader holds the raw bytes string (frozen); no per-call unpack.
 *   - Schema is precomputed into a key→slot Hash (Ruby Hash).
 *   - sum_f64 / min_f64 / max_f64 / sum_i64 run entirely in C with a single
 *     rb_float_new / rb_ll2inum at the end — no VALUE allocation per record.
 *   - LEB128 bitmask walk is inline C (scan_field_offset helper).
 *   - Little-endian reads via memcpy into typed locals (rd_u16 / rd_u32 /
 *     rd_u64 / rd_f64).
 */

#include "ruby.h"
#include "ruby/encoding.h"
#include <stdint.h>
#include <string.h>

/* ── Format constants ─────────────────────────────────────────────────────── */

#define MAGIC_FILE    0x4E595842u   /* NYXB */
#define MAGIC_OBJ     0x4E59584Fu   /* NYXO */
#define MAGIC_FOOTER  0x2153584Eu   /* NXS! */

/* ── Unaligned little-endian readers ─────────────────────────────────────── */

static inline uint16_t rd_u16(const uint8_t *p) {
    uint16_t v; memcpy(&v, p, 2); return v;
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

/* ── Reader C struct ─────────────────────────────────────────────────────── */

typedef struct {
    const uint8_t *data;
    size_t         size;
    uint64_t       tail_ptr;
    uint32_t       record_count;
    size_t         tail_start;    /* offset of first 10-byte entry */
    VALUE          key_names;     /* Ruby Array of String */
    VALUE          key_index;     /* Ruby Hash  str -> Integer (slot) */
    int            schema_embedded;
} NxsReader;

static void
reader_mark(void *ptr)
{
    NxsReader *r = (NxsReader *)ptr;
    rb_gc_mark(r->key_names);
    rb_gc_mark(r->key_index);
}

static void
reader_free(void *ptr)
{
    ruby_xfree(ptr);
}

static size_t
reader_memsize(const void *ptr)
{
    return sizeof(NxsReader);
}

static const rb_data_type_t reader_type = {
    "Nxs::CReader",
    { reader_mark, reader_free, reader_memsize },
    0, 0,
    RUBY_TYPED_FREE_IMMEDIATELY
};

/* ── Object C struct ─────────────────────────────────────────────────────── */

typedef struct {
    VALUE       reader_obj;    /* strong ref to CReader instance */
    NxsReader  *reader;        /* pointer into reader_obj's data */
    size_t      offset;        /* byte offset of NYXO magic */
    /* Lazy parse results */
    uint8_t    *present;       /* 1 byte per slot: 0/1 */
    uint16_t   *rank;          /* prefix-sum of present[] */
    uint16_t    present_len;
    size_t      offset_table_start;
    int         parsed;
} NxsObject;

static void
object_mark(void *ptr)
{
    NxsObject *o = (NxsObject *)ptr;
    rb_gc_mark(o->reader_obj);
}

static void
object_free(void *ptr)
{
    NxsObject *o = (NxsObject *)ptr;
    if (o->present) ruby_xfree(o->present);
    if (o->rank)    ruby_xfree(o->rank);
    ruby_xfree(o);
}

static size_t
object_memsize(const void *ptr)
{
    const NxsObject *o = (const NxsObject *)ptr;
    size_t s = sizeof(NxsObject);
    if (o->present) s += o->present_len;
    if (o->rank)    s += (o->present_len + 1) * sizeof(uint16_t);
    return s;
}

static const rb_data_type_t object_type = {
    "Nxs::CObject",
    { object_mark, object_free, object_memsize },
    0, 0,
    RUBY_TYPED_FREE_IMMEDIATELY
};

static VALUE
cobject_alloc(VALUE klass)
{
    NxsObject *obj;
    VALUE v = TypedData_Make_Struct(klass, NxsObject, &object_type, obj);
    obj->reader_obj         = Qnil;
    obj->reader             = NULL;
    obj->offset             = 0;
    obj->present            = NULL;
    obj->rank               = NULL;
    obj->present_len        = 0;
    obj->offset_table_start = 0;
    obj->parsed             = 0;
    return v;
}

/* ── Bulk scan helper (no alloc, used by reducers) ───────────────────────── */

/*
 * Walk the LEB128 bitmask starting at obj_offset+8, find the offset-table
 * entry for `slot`, and return the absolute byte offset of the value.
 * Returns (size_t)-1 on absent or out-of-bounds.
 *
 * Matches scan_field_offset() in py/_nxs.c exactly.
 */
static inline size_t
scan_field_offset(const uint8_t *data, size_t size,
                  size_t obj_offset, int slot)
{
    size_t p = obj_offset + 8; /* skip NYXO magic(4) + length(4) */
    if (p > size) return (size_t)-1;

    int cur_slot  = 0;
    int table_idx = 0;
    int found     = 0;
    uint8_t byte  = 0;

    do {
        if (p >= size) return (size_t)-1;
        byte = data[p++];
        uint8_t data_bits = byte & 0x7F;
        for (int b = 0; b < 7; b++) {
            if (cur_slot == slot) {
                if ((data_bits >> b) & 1) found = 1;
                else return (size_t)-1;
            } else if (cur_slot < slot && ((data_bits >> b) & 1)) {
                table_idx++;
            }
            cur_slot++;
        }
        if (found && (byte & 0x80) == 0) break;
        if (cur_slot > slot && found) break;
    } while (byte & 0x80);

    if (!found) return (size_t)-1;

    /* Drain any remaining continuation bytes to reach offset_table_start */
    while (byte & 0x80) {
        if (p >= size) return (size_t)-1;
        byte = data[p++];
    }

    size_t ofpos = p + (size_t)table_idx * 2;
    if (ofpos + 2 > size) return (size_t)-1;
    uint16_t rel = rd_u16(data + ofpos);
    return obj_offset + rel;
}

/* ── Forward declarations ────────────────────────────────────────────────── */

static VALUE rb_cNxs;
static VALUE rb_cCReader;
static VALUE rb_cCObject;

/* ── CReader#initialize(bytes) ───────────────────────────────────────────── */

static VALUE
creader_alloc(VALUE klass)
{
    NxsReader *r;
    VALUE obj = TypedData_Make_Struct(klass, NxsReader, &reader_type, r);
    r->data           = NULL;
    r->size           = 0;
    r->tail_ptr       = 0;
    r->record_count   = 0;
    r->tail_start     = 0;
    r->key_names      = rb_ary_new();
    r->key_index      = rb_hash_new();
    r->schema_embedded = 0;
    return obj;
}

static VALUE
creader_initialize(VALUE self, VALUE bytes)
{
    NxsReader *r;
    TypedData_Get_Struct(self, NxsReader, &reader_type, r);

    /* Freeze the buffer and hold a strong ivar reference to prevent GC. */
    VALUE buf = rb_str_freeze(rb_str_dup(bytes));
    rb_ivar_set(self, rb_intern("@_buf"), buf);

    r->data = (const uint8_t *)RSTRING_PTR(buf);
    r->size = (size_t)RSTRING_LEN(buf);

    if (r->size < 32)
        rb_raise(rb_eArgError, "ERR_OUT_OF_BOUNDS: file too small");

    uint32_t magic = rd_u32(r->data);
    if (magic != MAGIC_FILE)
        rb_raise(rb_eArgError, "ERR_BAD_MAGIC: preamble (got 0x%08X)", magic);

    uint16_t flags = rd_u16(r->data + 6);
    r->tail_ptr        = rd_u64(r->data + 16);
    r->schema_embedded = (flags & 0x0002) ? 1 : 0;

    if (r->size < 4 || rd_u32(r->data + r->size - 4) != MAGIC_FOOTER)
        rb_raise(rb_eArgError, "ERR_BAD_MAGIC: footer");
    if (r->tail_ptr == 0) {
        if (r->size < 44)
            rb_raise(rb_eArgError, "ERR_OUT_OF_BOUNDS: stream footer missing tail pointer");
        r->tail_ptr = rd_u64(r->data + r->size - 12);
    }

    /* ── Parse schema ──────────────────────────────────────────────────────── */
    if (r->schema_embedded) {
        size_t p = 32;
        if (p + 2 > r->size)
            rb_raise(rb_eArgError, "ERR_OUT_OF_BOUNDS: schema header");

        uint16_t key_count = rd_u16(r->data + p);
        p += 2;
        p += key_count; /* skip TypeManifest */

        r->key_names = rb_ary_new_capa(key_count);
        r->key_index = rb_hash_new();

        for (uint16_t i = 0; i < key_count; i++) {
            size_t start = p;
            while (p < r->size && r->data[p] != 0) p++;
            if (p >= r->size)
                rb_raise(rb_eArgError, "ERR_OUT_OF_BOUNDS: string pool");

            VALUE s = rb_enc_str_new((const char *)(r->data + start),
                                     (long)(p - start), rb_utf8_encoding());
            rb_ary_push(r->key_names, s);
            rb_hash_aset(r->key_index, s, INT2FIX(i));
            p++; /* skip null terminator */
        }
    }

    /* ── Parse tail-index ──────────────────────────────────────────────────── */
    if (r->tail_ptr + 4 > r->size)
        rb_raise(rb_eArgError, "ERR_OUT_OF_BOUNDS: tail index");

    r->record_count = rd_u32(r->data + r->tail_ptr);
    r->tail_start   = (size_t)(r->tail_ptr + 4);

    return self;
}

/* ── CReader#record_count ────────────────────────────────────────────────── */

static VALUE
creader_record_count(VALUE self)
{
    NxsReader *r;
    TypedData_Get_Struct(self, NxsReader, &reader_type, r);
    return UINT2NUM(r->record_count);
}

/* ── CReader#keys ────────────────────────────────────────────────────────── */

static VALUE
creader_keys(VALUE self)
{
    NxsReader *r;
    TypedData_Get_Struct(self, NxsReader, &reader_type, r);
    return rb_ary_dup(r->key_names);
}

/* ── CReader#record(i) ───────────────────────────────────────────────────── */

static VALUE
creader_record(VALUE self, VALUE idx)
{
    NxsReader *r;
    TypedData_Get_Struct(self, NxsReader, &reader_type, r);

    long i = NUM2LONG(idx);
    if (i < 0 || (uint32_t)i >= r->record_count) {
        rb_raise(rb_eIndexError,
                 "ERR_OUT_OF_BOUNDS: record %ld out of [0, %u)", i, r->record_count);
    }

    size_t entry = r->tail_start + (size_t)i * 10;
    uint64_t abs_offset = rd_u64(r->data + entry + 2);

    /* Allocate CObject */
    NxsObject *obj;
    VALUE obj_val = TypedData_Make_Struct(rb_cCObject, NxsObject, &object_type, obj);
    obj->reader_obj         = self;
    obj->reader             = r;
    obj->offset             = (size_t)abs_offset;
    obj->present            = NULL;
    obj->rank               = NULL;
    obj->present_len        = 0;
    obj->offset_table_start = 0;
    obj->parsed             = 0;

    return obj_val;
}

/* ── Reducer helpers ─────────────────────────────────────────────────────── */

/* Resolve key string to slot index; returns -1 if absent, raises on error. */
static int
reader_slot(NxsReader *r, VALUE key)
{
    VALUE slot_v = rb_hash_lookup(r->key_index, key);
    if (NIL_P(slot_v)) return -1;
    return FIX2INT(slot_v);
}

/* ── CReader#sum_f64(key) ────────────────────────────────────────────────── */

static VALUE
creader_sum_f64(VALUE self, VALUE key)
{
    NxsReader *r;
    TypedData_Get_Struct(self, NxsReader, &reader_type, r);

    int slot = reader_slot(r, key);
    if (slot < 0)
        rb_raise(rb_eKeyError, "key not in schema");

    const uint8_t *data = r->data;
    size_t size  = r->size;
    size_t tail  = r->tail_start;
    uint32_t n   = r->record_count;
    double sum   = 0.0;

    for (uint32_t i = 0; i < n; i++) {
        uint64_t abs = rd_u64(data + tail + i * 10 + 2);
        size_t off   = scan_field_offset(data, size, (size_t)abs, slot);
        if (off == (size_t)-1 || off + 8 > size) continue;
        sum += rd_f64(data + off);
    }
    return rb_float_new(sum);
}

/* ── CReader#min_f64(key) ────────────────────────────────────────────────── */

static VALUE
creader_min_f64(VALUE self, VALUE key)
{
    NxsReader *r;
    TypedData_Get_Struct(self, NxsReader, &reader_type, r);

    int slot = reader_slot(r, key);
    if (slot < 0) rb_raise(rb_eKeyError, "key not in schema");

    const uint8_t *data = r->data;
    size_t size  = r->size;
    size_t tail  = r->tail_start;
    uint32_t n   = r->record_count;
    double m     = 0.0;
    int have     = 0;

    for (uint32_t i = 0; i < n; i++) {
        uint64_t abs = rd_u64(data + tail + i * 10 + 2);
        size_t off   = scan_field_offset(data, size, (size_t)abs, slot);
        if (off == (size_t)-1 || off + 8 > size) continue;
        double v = rd_f64(data + off);
        if (!have || v < m) { m = v; have = 1; }
    }
    return have ? rb_float_new(m) : Qnil;
}

/* ── CReader#max_f64(key) ────────────────────────────────────────────────── */

static VALUE
creader_max_f64(VALUE self, VALUE key)
{
    NxsReader *r;
    TypedData_Get_Struct(self, NxsReader, &reader_type, r);

    int slot = reader_slot(r, key);
    if (slot < 0) rb_raise(rb_eKeyError, "key not in schema");

    const uint8_t *data = r->data;
    size_t size  = r->size;
    size_t tail  = r->tail_start;
    uint32_t n   = r->record_count;
    double m     = 0.0;
    int have     = 0;

    for (uint32_t i = 0; i < n; i++) {
        uint64_t abs = rd_u64(data + tail + i * 10 + 2);
        size_t off   = scan_field_offset(data, size, (size_t)abs, slot);
        if (off == (size_t)-1 || off + 8 > size) continue;
        double v = rd_f64(data + off);
        if (!have || v > m) { m = v; have = 1; }
    }
    return have ? rb_float_new(m) : Qnil;
}

/* ── CReader#sum_i64(key) ────────────────────────────────────────────────── */

static VALUE
creader_sum_i64(VALUE self, VALUE key)
{
    NxsReader *r;
    TypedData_Get_Struct(self, NxsReader, &reader_type, r);

    int slot = reader_slot(r, key);
    if (slot < 0) rb_raise(rb_eKeyError, "key not in schema");

    const uint8_t *data = r->data;
    size_t size  = r->size;
    size_t tail  = r->tail_start;
    uint32_t n   = r->record_count;
    int64_t sum  = 0;

    for (uint32_t i = 0; i < n; i++) {
        uint64_t abs = rd_u64(data + tail + i * 10 + 2);
        size_t off   = scan_field_offset(data, size, (size_t)abs, slot);
        if (off == (size_t)-1 || off + 8 > size) continue;
        sum += rd_i64(data + off);
    }
    return LL2NUM(sum);
}

/* ── CObject lazy header parse ───────────────────────────────────────────── */

static int
object_parse_header(NxsObject *obj)
{
    if (obj->parsed) return 0;

    const uint8_t *data = obj->reader->data;
    size_t size = obj->reader->size;
    size_t p    = obj->offset;

    if (p + 8 > size)
        rb_raise(rb_eArgError, "ERR_OUT_OF_BOUNDS: object header");

    if (rd_u32(data + p) != MAGIC_OBJ)
        rb_raise(rb_eArgError, "ERR_BAD_MAGIC: object at %zu", p);

    p += 8; /* skip magic(4) + length(4) */

    long key_count = RARRAY_LEN(obj->reader->key_names);
    obj->present = (uint8_t *)ruby_xcalloc((size_t)(key_count + 8), 1);
    obj->rank    = (uint16_t *)ruby_xcalloc((size_t)(key_count + 1), sizeof(uint16_t));

    uint16_t slot = 0;
    uint8_t  byte;
    do {
        if (p >= size)
            rb_raise(rb_eArgError, "ERR_OUT_OF_BOUNDS: bitmask");
        byte = data[p++];
        uint8_t bits = byte & 0x7F;
        for (int b = 0; b < 7 && slot < (uint16_t)key_count; b++, slot++) {
            obj->present[slot] = (bits >> b) & 1;
        }
    } while ((byte & 0x80) && slot < (uint16_t)key_count);

    uint16_t acc = 0;
    for (uint16_t s = 0; s < (uint16_t)key_count; s++) {
        obj->rank[s] = acc;
        acc += obj->present[s];
    }
    obj->rank[key_count]     = acc;
    obj->present_len         = (uint16_t)key_count;
    obj->offset_table_start  = p;
    obj->parsed              = 1;
    return 0;
}

/* Returns absolute byte offset of value for slot, or (size_t)-1 if absent. */
static size_t
object_field_offset(NxsObject *obj, int slot)
{
    object_parse_header(obj);
    if (slot < 0 || slot >= obj->present_len) return (size_t)-1;
    if (!obj->present[slot]) return (size_t)-1;

    uint16_t entry_idx = obj->rank[slot];
    size_t ofpos = obj->offset_table_start + (size_t)entry_idx * 2;
    uint16_t rel = rd_u16(obj->reader->data + ofpos);
    return obj->offset + rel;
}

/* Resolve key string → slot; returns -1 if not in schema */
static int
object_slot(NxsObject *obj, VALUE key)
{
    VALUE slot_v = rb_hash_lookup(obj->reader->key_index, key);
    if (NIL_P(slot_v)) return -1;
    return FIX2INT(slot_v);
}

/* ── CObject#get_str(key) ────────────────────────────────────────────────── */

static VALUE
cobject_get_str(VALUE self, VALUE key)
{
    NxsObject *obj;
    TypedData_Get_Struct(self, NxsObject, &object_type, obj);

    int slot = object_slot(obj, key);
    if (slot < 0) return Qnil;

    size_t off = object_field_offset(obj, slot);
    if (off == (size_t)-1) return Qnil;

    const uint8_t *data = obj->reader->data;
    size_t size = obj->reader->size;

    if (off + 4 > size)
        rb_raise(rb_eArgError, "ERR_OUT_OF_BOUNDS: str len");

    uint32_t n = rd_u32(data + off);
    if (off + 4 + n > size)
        rb_raise(rb_eArgError, "ERR_OUT_OF_BOUNDS: str bytes");

    return rb_enc_str_new((const char *)(data + off + 4),
                          (long)n, rb_utf8_encoding());
}

/* ── CObject#get_i64(key) ────────────────────────────────────────────────── */

static VALUE
cobject_get_i64(VALUE self, VALUE key)
{
    NxsObject *obj;
    TypedData_Get_Struct(self, NxsObject, &object_type, obj);

    int slot = object_slot(obj, key);
    if (slot < 0) return Qnil;

    size_t off = object_field_offset(obj, slot);
    if (off == (size_t)-1) return Qnil;

    if (off + 8 > obj->reader->size)
        rb_raise(rb_eArgError, "ERR_OUT_OF_BOUNDS: i64");

    return LL2NUM(rd_i64(obj->reader->data + off));
}

/* ── CObject#get_f64(key) ────────────────────────────────────────────────── */

static VALUE
cobject_get_f64(VALUE self, VALUE key)
{
    NxsObject *obj;
    TypedData_Get_Struct(self, NxsObject, &object_type, obj);

    int slot = object_slot(obj, key);
    if (slot < 0) return Qnil;

    size_t off = object_field_offset(obj, slot);
    if (off == (size_t)-1) return Qnil;

    if (off + 8 > obj->reader->size)
        rb_raise(rb_eArgError, "ERR_OUT_OF_BOUNDS: f64");

    return rb_float_new(rd_f64(obj->reader->data + off));
}

/* ── CObject#get_bool(key) ───────────────────────────────────────────────── */

static VALUE
cobject_get_bool(VALUE self, VALUE key)
{
    NxsObject *obj;
    TypedData_Get_Struct(self, NxsObject, &object_type, obj);

    int slot = object_slot(obj, key);
    if (slot < 0) return Qnil;

    size_t off = object_field_offset(obj, slot);
    if (off == (size_t)-1) return Qnil;

    if (off >= obj->reader->size)
        rb_raise(rb_eArgError, "ERR_OUT_OF_BOUNDS: bool");

    return obj->reader->data[off] ? Qtrue : Qfalse;
}

/* ── Writer / Schema C types ─────────────────────────────────────────────── */

#include "../../../c/nxs_writer.h"
#include "../../../c/nxs_writer.c"

/* ── Schema type ─────────────────────────────────────────────────────────── */

typedef struct {
    char  *key_buf;   /* single heap block: all key strings */
    char **keys;
    int    key_count;
} CSchemaData;

static void
cschema_free(void *ptr)
{
    CSchemaData *d = (CSchemaData *)ptr;
    free(d->key_buf);
    free(d->keys);
    free(d);
}

static size_t
cschema_memsize(const void *ptr)
{
    (void)ptr; return sizeof(CSchemaData);
}

static const rb_data_type_t cschema_type = {
    "Nxs::CSchema",
    { NULL, cschema_free, cschema_memsize },
    NULL, NULL, RUBY_TYPED_FREE_IMMEDIATELY
};

static VALUE rb_cCSchema;

static VALUE
cschema_alloc(VALUE klass)
{
    CSchemaData *d = (CSchemaData *)calloc(1, sizeof(CSchemaData));
    return TypedData_Wrap_Struct(klass, &cschema_type, d);
}

static VALUE
cschema_initialize(VALUE self, VALUE keys_ary)
{
    Check_Type(keys_ary, T_ARRAY);
    long n = RARRAY_LEN(keys_ary);
    if (n <= 0 || n > NXS_WRITER_MAX_KEYS)
        rb_raise(rb_eArgError, "key list must have 1–256 entries");

    CSchemaData *d;
    TypedData_Get_Struct(self, CSchemaData, &cschema_type, d);

    /* Compute total key-string storage */
    size_t total = 0;
    for (long i = 0; i < n; i++) {
        VALUE k = rb_ary_entry(keys_ary, i);
        Check_Type(k, T_STRING);
        total += (size_t)RSTRING_LEN(k) + 1;
    }

    d->key_buf  = (char *)malloc(total);
    d->keys     = (char **)malloc((size_t)n * sizeof(char *));
    if (!d->key_buf || !d->keys) rb_raise(rb_eNoMemError, "out of memory");

    char *p = d->key_buf;
    for (long i = 0; i < n; i++) {
        VALUE k = rb_ary_entry(keys_ary, i);
        long  slen = RSTRING_LEN(k);
        memcpy(p, RSTRING_PTR(k), (size_t)slen);
        p[slen] = '\0';
        d->keys[i] = p;
        p += slen + 1;
    }
    d->key_count = (int)n;
    return self;
}

/* ── Writer type ─────────────────────────────────────────────────────────── */

static void
cwriter_free(void *ptr)
{
    nxs_writer_t *w = (nxs_writer_t *)ptr;
    nxs_writer_free(w);
    free(w);
}

static size_t
cwriter_memsize(const void *ptr)
{
    const nxs_writer_t *w = (const nxs_writer_t *)ptr;
    return sizeof(nxs_writer_t) + w->buf_len + (size_t)w->record_cap * sizeof(uint32_t);
}

static const rb_data_type_t cwriter_type = {
    "Nxs::CWriter",
    { NULL, cwriter_free, cwriter_memsize },
    NULL, NULL, RUBY_TYPED_FREE_IMMEDIATELY
};

static VALUE rb_cCWriter;

static VALUE
cwriter_alloc(VALUE klass)
{
    nxs_writer_t *w = (nxs_writer_t *)calloc(1, sizeof(nxs_writer_t));
    return TypedData_Wrap_Struct(klass, &cwriter_type, w);
}

static VALUE
cwriter_initialize(VALUE self, VALUE schema)
{
    CSchemaData *d;
    TypedData_Get_Struct(schema, CSchemaData, &cschema_type, d);

    nxs_writer_t *w;
    TypedData_Get_Struct(self, nxs_writer_t, &cwriter_type, w);

    if (nxs_writer_init(w, (const char **)d->keys, d->key_count, 4096) != 0)
        rb_raise(rb_eNoMemError, "nxs_writer_init failed");
    return self;
}

static VALUE
cwriter_begin_object(VALUE self)
{
    nxs_writer_t *w;
    TypedData_Get_Struct(self, nxs_writer_t, &cwriter_type, w);
    if (nxs_writer_begin_object(w) != 0)
        rb_raise(rb_eRuntimeError, "nxs_writer_begin_object failed");
    return self;
}

static VALUE
cwriter_end_object(VALUE self)
{
    nxs_writer_t *w;
    TypedData_Get_Struct(self, nxs_writer_t, &cwriter_type, w);
    if (nxs_writer_end_object(w) != 0)
        rb_raise(rb_eRuntimeError, "nxs_writer_end_object failed");
    return self;
}

static VALUE
cwriter_write_i64(VALUE self, VALUE slot, VALUE v)
{
    nxs_writer_t *w;
    TypedData_Get_Struct(self, nxs_writer_t, &cwriter_type, w);
    if (nxs_write_i64(w, NUM2INT(slot), (int64_t)NUM2LL(v)) != 0)
        rb_raise(rb_eRuntimeError, "nxs_write_i64 failed");
    return self;
}

static VALUE
cwriter_write_f64(VALUE self, VALUE slot, VALUE v)
{
    nxs_writer_t *w;
    TypedData_Get_Struct(self, nxs_writer_t, &cwriter_type, w);
    if (nxs_write_f64(w, NUM2INT(slot), NUM2DBL(v)) != 0)
        rb_raise(rb_eRuntimeError, "nxs_write_f64 failed");
    return self;
}

static VALUE
cwriter_write_bool(VALUE self, VALUE slot, VALUE v)
{
    nxs_writer_t *w;
    TypedData_Get_Struct(self, nxs_writer_t, &cwriter_type, w);
    if (nxs_write_bool(w, NUM2INT(slot), RTEST(v) ? 1 : 0) != 0)
        rb_raise(rb_eRuntimeError, "nxs_write_bool failed");
    return self;
}

static VALUE
cwriter_write_null(VALUE self, VALUE slot)
{
    nxs_writer_t *w;
    TypedData_Get_Struct(self, nxs_writer_t, &cwriter_type, w);
    if (nxs_write_null(w, NUM2INT(slot)) != 0)
        rb_raise(rb_eRuntimeError, "nxs_write_null failed");
    return self;
}

static VALUE
cwriter_write_str(VALUE self, VALUE slot, VALUE str)
{
    nxs_writer_t *w;
    TypedData_Get_Struct(self, nxs_writer_t, &cwriter_type, w);
    StringValue(str);
    if (nxs_write_str(w, NUM2INT(slot), RSTRING_PTR(str), (uint32_t)RSTRING_LEN(str)) != 0)
        rb_raise(rb_eRuntimeError, "nxs_write_str failed");
    return self;
}

static VALUE
cwriter_write_bytes(VALUE self, VALUE slot, VALUE data)
{
    nxs_writer_t *w;
    TypedData_Get_Struct(self, nxs_writer_t, &cwriter_type, w);
    StringValue(data);
    if (nxs_write_bytes(w, NUM2INT(slot),
                        (const uint8_t *)RSTRING_PTR(data),
                        (uint32_t)RSTRING_LEN(data)) != 0)
        rb_raise(rb_eRuntimeError, "nxs_write_bytes failed");
    return self;
}

static VALUE
cwriter_finish(VALUE self)
{
    nxs_writer_t *w;
    TypedData_Get_Struct(self, nxs_writer_t, &cwriter_type, w);
    if (nxs_writer_finish(w) != 0)
        rb_raise(rb_eRuntimeError, "nxs_writer_finish failed");
    return rb_str_new((const char *)w->out, (long)w->out_size);
}

/* Raw NYXO bytes (data sector only — WAL path, no preamble). */
static VALUE
cwriter_data_sector(VALUE self)
{
    nxs_writer_t *w;
    TypedData_Get_Struct(self, nxs_writer_t, &cwriter_type, w);
    return rb_str_new((const char *)w->buf, (long)w->buf_pos);
}

static VALUE
cwriter_reset(VALUE self)
{
    nxs_writer_t *w;
    TypedData_Get_Struct(self, nxs_writer_t, &cwriter_type, w);
    nxs_writer_reset(w);
    return self;
}

/* ── Extension init ──────────────────────────────────────────────────────── */

void
Init_nxs_ext(void)
{
    /* Get or create the Nxs module */
    rb_cNxs = rb_const_defined(rb_cObject, rb_intern("Nxs"))
        ? rb_const_get(rb_cObject, rb_intern("Nxs"))
        : rb_define_module("Nxs");

    /* Nxs::CReader */
    rb_cCReader = rb_define_class_under(rb_cNxs, "CReader", rb_cObject);
    rb_define_alloc_func(rb_cCReader, creader_alloc);
    rb_define_method(rb_cCReader, "initialize",   creader_initialize,   1);
    rb_define_method(rb_cCReader, "record_count", creader_record_count, 0);
    rb_define_method(rb_cCReader, "keys",         creader_keys,         0);
    rb_define_method(rb_cCReader, "record",       creader_record,       1);
    rb_define_method(rb_cCReader, "sum_f64",      creader_sum_f64,      1);
    rb_define_method(rb_cCReader, "min_f64",      creader_min_f64,      1);
    rb_define_method(rb_cCReader, "max_f64",      creader_max_f64,      1);
    rb_define_method(rb_cCReader, "sum_i64",      creader_sum_i64,      1);

    /* Nxs::CObject */
    rb_cCObject = rb_define_class_under(rb_cNxs, "CObject", rb_cObject);
    rb_define_alloc_func(rb_cCObject, cobject_alloc);
    rb_define_method(rb_cCObject, "get_str",  cobject_get_str,  1);
    rb_define_method(rb_cCObject, "get_i64",  cobject_get_i64,  1);
    rb_define_method(rb_cCObject, "get_f64",  cobject_get_f64,  1);
    rb_define_method(rb_cCObject, "get_bool", cobject_get_bool, 1);

    /* Nxs::CSchema */
    rb_cCSchema = rb_define_class_under(rb_cNxs, "CSchema", rb_cObject);
    rb_define_alloc_func(rb_cCSchema, cschema_alloc);
    rb_define_method(rb_cCSchema, "initialize", cschema_initialize, 1);

    /* Nxs::CWriter */
    rb_cCWriter = rb_define_class_under(rb_cNxs, "CWriter", rb_cObject);
    rb_define_alloc_func(rb_cCWriter, cwriter_alloc);
    rb_define_method(rb_cCWriter, "initialize",   cwriter_initialize,   1);
    rb_define_method(rb_cCWriter, "begin_object", cwriter_begin_object, 0);
    rb_define_method(rb_cCWriter, "end_object",   cwriter_end_object,   0);
    rb_define_method(rb_cCWriter, "write_i64",    cwriter_write_i64,    2);
    rb_define_method(rb_cCWriter, "write_f64",    cwriter_write_f64,    2);
    rb_define_method(rb_cCWriter, "write_bool",   cwriter_write_bool,   2);
    rb_define_method(rb_cCWriter, "write_null",   cwriter_write_null,   1);
    rb_define_method(rb_cCWriter, "write_str",    cwriter_write_str,    2);
    rb_define_method(rb_cCWriter, "write_bytes",  cwriter_write_bytes,  2);
    rb_define_method(rb_cCWriter, "finish",       cwriter_finish,       0);
    rb_define_method(rb_cCWriter, "data_sector",  cwriter_data_sector,  0);
    rb_define_method(rb_cCWriter, "reset",        cwriter_reset,        0);
}
