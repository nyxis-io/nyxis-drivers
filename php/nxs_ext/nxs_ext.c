/*
 * nxs_ext.c — PHP 8 Zend extension for the NXS binary format reader.
 *
 * Exposes two classes (global namespace):
 *   NxsReader($bytes)  — parses preamble/schema/tail-index
 *   NxsObject          — returned by NxsReader::record(i); lazy field decode
 *
 * Algorithm mirrors py/_nxs.c:
 *   - Raw bytes held in a zend_string* (refcounted, no extra copy).
 *   - Schema pre-computed into a HashTable: key name → slot index.
 *   - LEB128 bitmask walk identical to the Python C extension.
 *   - sumF64/minF64/maxF64/sumI64 are tight C loops, zero per-record zval.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "ext/standard/info.h"
#include "zend_exceptions.h"

#include <stdint.h>
#include <string.h>

/* ── Format constants ──────────────────────────────────────────────────────── */

#define MAGIC_FILE    0x4E595842u   /* NYXB */
#define MAGIC_OBJ     0x4E59584Fu   /* NYXO */
#define MAGIC_FOOTER  0x2153584Eu   /* NXS! */
#define FLAG_SCHEMA_EMBEDDED 0x0002u

/* ── Unaligned LE readers ──────────────────────────────────────────────────── */

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

/* ── Class entries + handlers ──────────────────────────────────────────────── */

static zend_class_entry *nxs_reader_ce;
static zend_class_entry *nxs_object_ce;
static zend_object_handlers nxs_reader_handlers;
static zend_object_handlers nxs_object_handlers;

/* ── NxsReader internal struct ─────────────────────────────────────────────── */

typedef struct _nxs_reader_t {
    zend_string   *bytes_zs;      /* raw buffer; refcounted */
    const uint8_t *data;
    size_t         size;
    uint64_t       tail_ptr;
    uint32_t       record_count;
    size_t         tail_start;    /* offset of first tail-index entry */
    HashTable     *key_index;     /* zend_string key → ZEND_LONG slot */
    zval           keys_zv;       /* PHP array of key names */
    int            schema_embedded;
    zend_object    std;
} nxs_reader_t;

static inline nxs_reader_t *reader_from_obj(zend_object *obj) {
    return (nxs_reader_t *)((char *)obj - XtOffsetOf(nxs_reader_t, std));
}
#define Z_READER_P(zv)  reader_from_obj(Z_OBJ_P(zv))

/* ── NxsObject internal struct ─────────────────────────────────────────────── */

typedef struct _nxs_object_t {
    zval       reader_zv;         /* strong ref to NxsReader */
    size_t     offset;            /* byte offset of NYXO magic */
    int        parsed;
    size_t     offset_table_start;
    uint16_t   present_len;
    uint8_t   *present;           /* 1 byte per slot */
    uint16_t  *rank;              /* prefix-sum of present bits */
    zend_object std;
} nxs_object_t;

static inline nxs_object_t *nxsobj_from_obj(zend_object *obj) {
    return (nxs_object_t *)((char *)obj - XtOffsetOf(nxs_object_t, std));
}
#define Z_NYXOBJ_P(zv)  nxsobj_from_obj(Z_OBJ_P(zv))

/* ── NxsReader lifecycle ───────────────────────────────────────────────────── */

static zend_object *nxs_reader_create(zend_class_entry *ce)
{
    nxs_reader_t *r = ecalloc(1, sizeof(*r) + zend_object_properties_size(ce));
    zend_object_std_init(&r->std, ce);
    object_properties_init(&r->std, ce);
    r->std.handlers = &nxs_reader_handlers;
    ZVAL_UNDEF(&r->keys_zv);
    return &r->std;
}

static void nxs_reader_free(zend_object *obj)
{
    nxs_reader_t *r = reader_from_obj(obj);
    if (r->bytes_zs) { zend_string_release(r->bytes_zs); r->bytes_zs = NULL; }
    if (r->key_index) {
        zend_hash_destroy(r->key_index);
        FREE_HASHTABLE(r->key_index);
        r->key_index = NULL;
    }
    zval_ptr_dtor(&r->keys_zv);
    zend_object_std_dtor(obj);
}

/* ── NxsObject lifecycle ───────────────────────────────────────────────────── */

static zend_object *nxs_object_create(zend_class_entry *ce)
{
    nxs_object_t *o = ecalloc(1, sizeof(*o) + zend_object_properties_size(ce));
    zend_object_std_init(&o->std, ce);
    object_properties_init(&o->std, ce);
    o->std.handlers = &nxs_object_handlers;
    ZVAL_UNDEF(&o->reader_zv);
    return &o->std;
}

static void nxs_object_free(zend_object *obj)
{
    nxs_object_t *o = nxsobj_from_obj(obj);
    zval_ptr_dtor(&o->reader_zv);
    if (o->present) { efree(o->present); o->present = NULL; }
    if (o->rank)    { efree(o->rank);    o->rank    = NULL; }
    zend_object_std_dtor(obj);
}

/* ── Schema + tail-index parse ─────────────────────────────────────────────── */

static int reader_parse(nxs_reader_t *r)
{
    const uint8_t *data = r->data;
    size_t size = r->size;
    size_t p = 32;

    if (r->schema_embedded) {
        if (p + 2 > size) {
            zend_throw_exception(zend_ce_exception,
                "ERR_OUT_OF_BOUNDS: schema header", 0);
            return FAILURE;
        }
        uint16_t kc = rd_u16(data + p);
        p += 2 + kc; /* skip TypeManifest */

        array_init_size(&r->keys_zv, kc);
        ALLOC_HASHTABLE(r->key_index);
        zend_hash_init(r->key_index, kc, NULL, NULL, 0);

        for (uint16_t i = 0; i < kc; i++) {
            size_t start = p;
            while (p < size && data[p] != 0) p++;
            if (p >= size) {
                zend_throw_exception(zend_ce_exception,
                    "ERR_OUT_OF_BOUNDS: string pool", 0);
                return FAILURE;
            }
            zend_string *ks = zend_string_init((const char *)(data + start), p - start, 0);
            /* append to keys array */
            zval kv; ZVAL_STR_COPY(&kv, ks);
            zend_hash_next_index_insert(Z_ARRVAL(r->keys_zv), &kv);
            /* key_index: name → slot */
            zval sv; ZVAL_LONG(&sv, (zend_long)i);
            zend_hash_add(r->key_index, ks, &sv);
            zend_string_release(ks);
            p++; /* null terminator */
        }
    }

    if ((size_t)r->tail_ptr + 4 > size) {
        zend_throw_exception(zend_ce_exception,
            "ERR_OUT_OF_BOUNDS: tail index", 0);
        return FAILURE;
    }
    r->record_count = rd_u32(data + r->tail_ptr);
    r->tail_start   = (size_t)r->tail_ptr + 4;
    return SUCCESS;
}

/* ── Inline bitmask walk (no alloc) ───────────────────────────────────────── */

static inline size_t scan_field_offset(
    const uint8_t *data, size_t size,
    size_t obj_off, int slot)
{
    size_t p = obj_off + 8;
    if (p > size) return (size_t)-1;

    int cur  = 0, tidx = 0, found = 0;
    uint8_t byte = 0;

    do {
        if (p >= size) return (size_t)-1;
        byte = data[p++];
        uint8_t bits = byte & 0x7F;
        for (int b = 0; b < 7; b++) {
            if (cur == slot) {
                if ((bits >> b) & 1) found = 1;
                else return (size_t)-1;
                goto done;
            }
            if ((bits >> b) & 1) tidx++;
            cur++;
        }
        if (cur > slot && !found) return (size_t)-1;
    } while (byte & 0x80);

done:
    if (!found) return (size_t)-1;
    /* drain remaining bitmask bytes */
    while (byte & 0x80) {
        if (p >= size) return (size_t)-1;
        byte = data[p++];
    }
    size_t ofpos = p + (size_t)tidx * 2;
    if (ofpos + 2 > size) return (size_t)-1;
    return obj_off + rd_u16(data + ofpos);
}

/* ── NxsReader methods ─────────────────────────────────────────────────────── */

PHP_METHOD(NxsReader, __construct)
{
    zend_string *arg;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(arg)
    ZEND_PARSE_PARAMETERS_END();

    nxs_reader_t *r = Z_READER_P(ZEND_THIS);
    r->bytes_zs = zend_string_copy(arg);
    r->data     = (const uint8_t *)ZSTR_VAL(r->bytes_zs);
    r->size     = ZSTR_LEN(r->bytes_zs);

    if (r->size < 32) {
        zend_throw_exception(zend_ce_exception,
            "ERR_OUT_OF_BOUNDS: file too small", 0);
        return;
    }
    if (rd_u32(r->data) != MAGIC_FILE) {
        zend_throw_exception(zend_ce_exception, "ERR_BAD_MAGIC: preamble", 0);
        return;
    }
    uint16_t flags = rd_u16(r->data + 6);
    r->tail_ptr        = rd_u64(r->data + 16);
    r->schema_embedded = (flags & FLAG_SCHEMA_EMBEDDED) ? 1 : 0;

    if (rd_u32(r->data + r->size - 4) != MAGIC_FOOTER) {
        zend_throw_exception(zend_ce_exception, "ERR_BAD_MAGIC: footer", 0);
        return;
    }
    if (r->tail_ptr == 0) {
        if (r->size < 44) {
            zend_throw_exception(zend_ce_exception,
                "ERR_OUT_OF_BOUNDS: stream footer missing tail pointer", 0);
            return;
        }
        r->tail_ptr = rd_u64(r->data + r->size - 12);
    }
    reader_parse(r);
}

PHP_METHOD(NxsReader, recordCount)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_LONG((zend_long)Z_READER_P(ZEND_THIS)->record_count);
}

PHP_METHOD(NxsReader, keys)
{
    ZEND_PARSE_PARAMETERS_NONE();
    nxs_reader_t *r = Z_READER_P(ZEND_THIS);
    ZVAL_COPY(return_value, &r->keys_zv);
}

static zend_long reader_slot(nxs_reader_t *r, zend_string *key)
{
    if (!r->key_index) return -1;
    zval *z = zend_hash_find(r->key_index, key);
    return z ? Z_LVAL_P(z) : -1;
}

PHP_METHOD(NxsReader, record)
{
    zend_long idx;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(idx)
    ZEND_PARSE_PARAMETERS_END();

    nxs_reader_t *r = Z_READER_P(ZEND_THIS);
    if (idx < 0 || (uint64_t)idx >= r->record_count) {
        zend_throw_exception_ex(zend_ce_exception, 0,
            "ERR_OUT_OF_BOUNDS: record %ld out of [0, %u)",
            (long)idx, r->record_count);
        return;
    }
    size_t entry = r->tail_start + (size_t)idx * 10;
    uint64_t abs = rd_u64(r->data + entry + 2);

    object_init_ex(return_value, nxs_object_ce);
    nxs_object_t *o = nxsobj_from_obj(Z_OBJ_P(return_value));
    ZVAL_OBJ_COPY(&o->reader_zv, Z_OBJ_P(ZEND_THIS));
    o->offset = (size_t)abs;
}

PHP_METHOD(NxsReader, sumF64)
{
    zend_string *key;
    ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_STR(key) ZEND_PARSE_PARAMETERS_END();
    nxs_reader_t *r = Z_READER_P(ZEND_THIS);
    zend_long slot  = reader_slot(r, key);
    if (slot < 0) { zend_throw_exception(zend_ce_exception, "key not in schema", 0); return; }

    const uint8_t *d = r->data; size_t sz = r->size, tail = r->tail_start;
    uint32_t n = r->record_count; double sum = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t ab = rd_u64(d + tail + (size_t)i * 10 + 2);
        size_t of = scan_field_offset(d, sz, (size_t)ab, (int)slot);
        if (of != (size_t)-1 && of + 8 <= sz) sum += rd_f64(d + of);
    }
    RETURN_DOUBLE(sum);
}

PHP_METHOD(NxsReader, minF64)
{
    zend_string *key;
    ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_STR(key) ZEND_PARSE_PARAMETERS_END();
    nxs_reader_t *r = Z_READER_P(ZEND_THIS);
    zend_long slot  = reader_slot(r, key);
    if (slot < 0) { zend_throw_exception(zend_ce_exception, "key not in schema", 0); return; }

    const uint8_t *d = r->data; size_t sz = r->size, tail = r->tail_start;
    uint32_t n = r->record_count; double m = 0.0; int have = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t ab = rd_u64(d + tail + (size_t)i * 10 + 2);
        size_t of = scan_field_offset(d, sz, (size_t)ab, (int)slot);
        if (of == (size_t)-1 || of + 8 > sz) continue;
        double v = rd_f64(d + of);
        if (!have || v < m) { m = v; have = 1; }
    }
    if (!have) RETURN_NULL();
    RETURN_DOUBLE(m);
}

PHP_METHOD(NxsReader, maxF64)
{
    zend_string *key;
    ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_STR(key) ZEND_PARSE_PARAMETERS_END();
    nxs_reader_t *r = Z_READER_P(ZEND_THIS);
    zend_long slot  = reader_slot(r, key);
    if (slot < 0) { zend_throw_exception(zend_ce_exception, "key not in schema", 0); return; }

    const uint8_t *d = r->data; size_t sz = r->size, tail = r->tail_start;
    uint32_t n = r->record_count; double m = 0.0; int have = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t ab = rd_u64(d + tail + (size_t)i * 10 + 2);
        size_t of = scan_field_offset(d, sz, (size_t)ab, (int)slot);
        if (of == (size_t)-1 || of + 8 > sz) continue;
        double v = rd_f64(d + of);
        if (!have || v > m) { m = v; have = 1; }
    }
    if (!have) RETURN_NULL();
    RETURN_DOUBLE(m);
}

PHP_METHOD(NxsReader, sumI64)
{
    zend_string *key;
    ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_STR(key) ZEND_PARSE_PARAMETERS_END();
    nxs_reader_t *r = Z_READER_P(ZEND_THIS);
    zend_long slot  = reader_slot(r, key);
    if (slot < 0) { zend_throw_exception(zend_ce_exception, "key not in schema", 0); return; }

    const uint8_t *d = r->data; size_t sz = r->size, tail = r->tail_start;
    uint32_t n = r->record_count; int64_t sum = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t ab = rd_u64(d + tail + (size_t)i * 10 + 2);
        size_t of = scan_field_offset(d, sz, (size_t)ab, (int)slot);
        if (of != (size_t)-1 && of + 8 <= sz) sum += rd_i64(d + of);
    }
    RETURN_LONG((zend_long)sum);
}

/* ── NxsObject: lazy bitmask parse ────────────────────────────────────────── */

static int nxsobj_parse(nxs_object_t *o)
{
    if (o->parsed) return SUCCESS;
    nxs_reader_t *r = Z_READER_P(&o->reader_zv);
    const uint8_t *data = r->data;
    size_t size = r->size, p = o->offset;

    if (p + 8 > size) {
        zend_throw_exception(zend_ce_exception, "ERR_OUT_OF_BOUNDS: object header", 0);
        return FAILURE;
    }
    if (rd_u32(data + p) != MAGIC_OBJ) {
        zend_throw_exception(zend_ce_exception, "ERR_BAD_MAGIC: object", 0);
        return FAILURE;
    }
    p += 8;

    uint16_t kc = r->key_index ? (uint16_t)zend_hash_num_elements(r->key_index) : 0;
    o->present = ecalloc(kc + 8, 1);
    o->rank    = ecalloc(kc + 1, sizeof(uint16_t));

    uint16_t slot = 0; uint8_t byte;
    do {
        if (p >= size) {
            zend_throw_exception(zend_ce_exception, "ERR_OUT_OF_BOUNDS: bitmask", 0);
            return FAILURE;
        }
        byte = data[p++];
        uint8_t bits = byte & 0x7F;
        for (int b = 0; b < 7 && slot < kc; b++, slot++)
            o->present[slot] = (bits >> b) & 1;
    } while ((byte & 0x80) && slot < kc);

    uint16_t acc = 0;
    for (uint16_t s = 0; s < kc; s++) {
        o->rank[s] = acc;
        acc += o->present[s];
    }
    o->rank[kc] = acc;
    o->present_len = kc;
    o->offset_table_start = p;
    o->parsed = 1;
    return SUCCESS;
}

static size_t nxsobj_field_off(nxs_object_t *o, zend_long slot)
{
    if (!o->parsed && nxsobj_parse(o) == FAILURE) return (size_t)-1;
    if (slot < 0 || (uint16_t)slot >= o->present_len) return (size_t)-1;
    if (!o->present[slot]) return (size_t)-1;
    nxs_reader_t *r = Z_READER_P(&o->reader_zv);
    uint16_t ei = o->rank[slot];
    uint16_t rel = rd_u16(r->data + o->offset_table_start + (size_t)ei * 2);
    return o->offset + rel;
}

/* ── NxsObject methods ─────────────────────────────────────────────────────── */

PHP_METHOD(NxsObject, getStr)
{
    zend_string *key;
    ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_STR(key) ZEND_PARSE_PARAMETERS_END();
    nxs_object_t *o = Z_NYXOBJ_P(ZEND_THIS);
    nxs_reader_t *r = Z_READER_P(&o->reader_zv);
    zend_long slot = reader_slot(r, key);
    if (slot < 0) RETURN_NULL();
    size_t off = nxsobj_field_off(o, slot);
    if (off == (size_t)-1) RETURN_NULL();
    if (off + 4 > r->size) { zend_throw_exception(zend_ce_exception, "ERR_OUT_OF_BOUNDS: str len", 0); return; }
    uint32_t len = rd_u32(r->data + off);
    if (off + 4 + (size_t)len > r->size) { zend_throw_exception(zend_ce_exception, "ERR_OUT_OF_BOUNDS: str bytes", 0); return; }
    RETURN_STRINGL((const char *)(r->data + off + 4), len);
}

PHP_METHOD(NxsObject, getI64)
{
    zend_string *key;
    ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_STR(key) ZEND_PARSE_PARAMETERS_END();
    nxs_object_t *o = Z_NYXOBJ_P(ZEND_THIS);
    nxs_reader_t *r = Z_READER_P(&o->reader_zv);
    zend_long slot = reader_slot(r, key);
    if (slot < 0) RETURN_NULL();
    size_t off = nxsobj_field_off(o, slot);
    if (off == (size_t)-1) RETURN_NULL();
    if (off + 8 > r->size) { zend_throw_exception(zend_ce_exception, "ERR_OUT_OF_BOUNDS: i64", 0); return; }
    RETURN_LONG((zend_long)rd_i64(r->data + off));
}

PHP_METHOD(NxsObject, getF64)
{
    zend_string *key;
    ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_STR(key) ZEND_PARSE_PARAMETERS_END();
    nxs_object_t *o = Z_NYXOBJ_P(ZEND_THIS);
    nxs_reader_t *r = Z_READER_P(&o->reader_zv);
    zend_long slot = reader_slot(r, key);
    if (slot < 0) RETURN_NULL();
    size_t off = nxsobj_field_off(o, slot);
    if (off == (size_t)-1) RETURN_NULL();
    if (off + 8 > r->size) { zend_throw_exception(zend_ce_exception, "ERR_OUT_OF_BOUNDS: f64", 0); return; }
    RETURN_DOUBLE(rd_f64(r->data + off));
}

PHP_METHOD(NxsObject, getBool)
{
    zend_string *key;
    ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_STR(key) ZEND_PARSE_PARAMETERS_END();
    nxs_object_t *o = Z_NYXOBJ_P(ZEND_THIS);
    nxs_reader_t *r = Z_READER_P(&o->reader_zv);
    zend_long slot = reader_slot(r, key);
    if (slot < 0) RETURN_NULL();
    size_t off = nxsobj_field_off(o, slot);
    if (off == (size_t)-1) RETURN_NULL();
    if (off >= r->size) { zend_throw_exception(zend_ce_exception, "ERR_OUT_OF_BOUNDS: bool", 0); return; }
    RETURN_BOOL(r->data[off] != 0);
}

/* ── Arginfo ────────────────────────────────────────────────────────────────── */

/* NxsReader::__construct(string $bytes): void */
ZEND_BEGIN_ARG_INFO_EX(arginfo_NxsReader_construct, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, bytes, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* NxsReader::recordCount(): int */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_NxsReader_recordCount, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

/* NxsReader::keys(): array */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_NxsReader_keys, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

/* NxsReader::record(int $i): NxsObject */
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_NxsReader_record, 0, 1, NxsObject, 0)
    ZEND_ARG_TYPE_INFO(0, i, IS_LONG, 0)
ZEND_END_ARG_INFO()

/* NxsReader::sumF64(string $key): float */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_NxsReader_sumF64, 0, 1, IS_DOUBLE, 0)
    ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* NxsReader::minF64(string $key): ?float */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_NxsReader_minF64, 0, 1, IS_DOUBLE, 1)
    ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* NxsReader::maxF64(string $key): ?float */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_NxsReader_maxF64, 0, 1, IS_DOUBLE, 1)
    ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* NxsReader::sumI64(string $key): int */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_NxsReader_sumI64, 0, 1, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* NxsObject::getStr(string $key): ?string */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_NxsObject_getStr, 0, 1, IS_STRING, 1)
    ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* NxsObject::getI64(string $key): ?int */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_NxsObject_getI64, 0, 1, IS_LONG, 1)
    ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* NxsObject::getF64(string $key): ?float */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_NxsObject_getF64, 0, 1, IS_DOUBLE, 1)
    ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* NxsObject::getBool(string $key): ?bool */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_NxsObject_getBool, 0, 1, _IS_BOOL, 1)
    ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* ── Method tables ─────────────────────────────────────────────────────────── */

static const zend_function_entry nxs_reader_methods[] = {
    PHP_ME(NxsReader, __construct, arginfo_NxsReader_construct, ZEND_ACC_PUBLIC)
    PHP_ME(NxsReader, recordCount, arginfo_NxsReader_recordCount, ZEND_ACC_PUBLIC)
    PHP_ME(NxsReader, keys,        arginfo_NxsReader_keys,        ZEND_ACC_PUBLIC)
    PHP_ME(NxsReader, record,      arginfo_NxsReader_record,      ZEND_ACC_PUBLIC)
    PHP_ME(NxsReader, sumF64,      arginfo_NxsReader_sumF64,      ZEND_ACC_PUBLIC)
    PHP_ME(NxsReader, minF64,      arginfo_NxsReader_minF64,      ZEND_ACC_PUBLIC)
    PHP_ME(NxsReader, maxF64,      arginfo_NxsReader_maxF64,      ZEND_ACC_PUBLIC)
    PHP_ME(NxsReader, sumI64,      arginfo_NxsReader_sumI64,      ZEND_ACC_PUBLIC)
    PHP_FE_END
};

static const zend_function_entry nxs_object_methods[] = {
    PHP_ME(NxsObject, getStr,  arginfo_NxsObject_getStr,  ZEND_ACC_PUBLIC)
    PHP_ME(NxsObject, getI64,  arginfo_NxsObject_getI64,  ZEND_ACC_PUBLIC)
    PHP_ME(NxsObject, getF64,  arginfo_NxsObject_getF64,  ZEND_ACC_PUBLIC)
    PHP_ME(NxsObject, getBool, arginfo_NxsObject_getBool, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

/* ── Module init ───────────────────────────────────────────────────────────── */

PHP_MINIT_FUNCTION(nxs)
{
    /* NxsReader */
    {
        zend_class_entry ce;
        INIT_CLASS_ENTRY(ce, "NxsReader", nxs_reader_methods);
        nxs_reader_ce = zend_register_internal_class(&ce);
        nxs_reader_ce->create_object = nxs_reader_create;

        memcpy(&nxs_reader_handlers, zend_get_std_object_handlers(),
               sizeof(zend_object_handlers));
        nxs_reader_handlers.offset    = XtOffsetOf(nxs_reader_t, std);
        nxs_reader_handlers.free_obj  = nxs_reader_free;
        nxs_reader_handlers.clone_obj = NULL;
    }

    /* NxsObject */
    {
        zend_class_entry ce;
        INIT_CLASS_ENTRY(ce, "NxsObject", nxs_object_methods);
        nxs_object_ce = zend_register_internal_class(&ce);
        nxs_object_ce->create_object = nxs_object_create;

        memcpy(&nxs_object_handlers, zend_get_std_object_handlers(),
               sizeof(zend_object_handlers));
        nxs_object_handlers.offset    = XtOffsetOf(nxs_object_t, std);
        nxs_object_handlers.free_obj  = nxs_object_free;
        nxs_object_handlers.clone_obj = NULL;
    }

    return SUCCESS;
}

PHP_MINFO_FUNCTION(nxs)
{
    php_info_print_table_start();
    php_info_print_table_row(2, "NXS Reader extension", "enabled");
    php_info_print_table_row(2, "version", "1.0.0");
    php_info_print_table_end();
}

zend_module_entry nxs_module_entry = {
    STANDARD_MODULE_HEADER,
    "nxs",
    NULL,
    PHP_MINIT(nxs),
    NULL,
    NULL,
    NULL,
    PHP_MINFO(nxs),
    "1.0.0",
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_NXS
ZEND_GET_MODULE(nxs)
#endif
