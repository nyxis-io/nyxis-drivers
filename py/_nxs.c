/*
 * _nxs.c — CPython C extension for the NXS binary format (reader + writer).
 *
 * Exposes four types:
 *   _nxs.Reader(buffer)       — parses preamble/schema/tail-index
 *   _nxs.Object               — returned by Reader.record(i); decodes fields
 *   _nxs.Schema(keys)         — precompiled schema for Writer
 *   _nxs.Writer(schema)       — direct-to-buffer .nxb emitter
 *
 * Design:
 *   - Reader holds the raw bytes pointer; no per-call struct.unpack.
 *   - Schema is precomputed into a key→slot HashMap (via PyDict).
 *   - Bitmask bits are eagerly counted into an offset-table index on object
 *     construction — so get_str/get_i64/etc. are O(1) after first access.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../c/nxs.h"

/* ── Format constants (must match writer.rs) ─────────────────────────────── */

#define MAGIC_FILE    0x4E595842u   /* NYXB */
#define MAGIC_OBJ     0x4E59584Fu   /* NYXO */
#define MAGIC_LIST    0x4E59584Cu   /* NYXL */
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

/* ── Reader type ─────────────────────────────────────────────────────────── */

typedef struct {
    PyObject_HEAD
    PyObject *buffer_obj;            /* Holds a reference keeping bytes alive */
    Py_buffer view;                  /* Actual byte pointer */
    const uint8_t *data;             /* view.buf */
    Py_ssize_t size;                 /* view.len */
    uint64_t tail_ptr;
    uint32_t record_count;
    Py_ssize_t tail_start;           /* Offset of first record entry */
    PyObject *key_index;             /* dict: str → int (slot) */
    PyObject *keys;                  /* list of str, for introspection */
    int schema_embedded;
    nxs_reader_t nxs;                /* Full layout-aware reader (c/nxs.c) */
    int has_nxs;
} ReaderObject;

static const char *
nxs_err_msg(nxs_err_t err)
{
    switch (err) {
    case NXS_ERR_BAD_MAGIC: return "ERR_BAD_MAGIC";
    case NXS_ERR_DICT_MISMATCH: return "ERR_DICT_MISMATCH";
    case NXS_ERR_OUT_OF_BOUNDS: return "ERR_OUT_OF_BOUNDS";
    case NXS_ERR_KEY_NOT_FOUND: return "ERR_KEY_NOT_FOUND";
    case NXS_ERR_INVALID_FLAGS: return "ERR_INVALID_FLAGS";
    case NXS_ERR_INCOMPATIBLE: return "ERR_INCOMPATIBLE_FLAGS";
    case NXS_ERR_BAD_PAGE_MAGIC: return "ERR_INVALID_PAGE_MAGIC";
    case NXS_ERR_UNSUPPORTED: return "ERR_UNSUPPORTED";
    case NXS_ERR_UNSUPPORTED_TYPE: return "ERR_UNSUPPORTED_FIELD_TYPE";
    default: return "ERR_UNKNOWN";
    }
}

static PyTypeObject ReaderType;
static PyTypeObject ObjectType;

static int
Reader_init(ReaderObject *self, PyObject *args, PyObject *kwds)
{
    PyObject *buf_obj;
    (void)kwds;
    if (!PyArg_ParseTuple(args, "O", &buf_obj)) return -1;

    if (PyObject_GetBuffer(buf_obj, &self->view, PyBUF_SIMPLE) < 0) return -1;

    self->buffer_obj = buf_obj;
    Py_INCREF(buf_obj);
    self->data = (const uint8_t *)self->view.buf;
    self->size = self->view.len;

    if (self->size < 32) {
        PyErr_SetString(PyExc_ValueError, "ERR_OUT_OF_BOUNDS: file too small");
        return -1;
    }

    uint32_t magic = rd_u32(self->data);
    if (magic != MAGIC_FILE) {
        PyErr_SetString(PyExc_ValueError, "ERR_BAD_MAGIC: preamble");
        return -1;
    }

    uint16_t flags = rd_u16(self->data + 6);
    self->tail_ptr = rd_u64(self->data + 16);
    self->schema_embedded = (flags & 0x0002) ? 1 : 0;

    if (rd_u32(self->data + self->size - 4) != MAGIC_FOOTER) {
        PyErr_SetString(PyExc_ValueError, "ERR_BAD_MAGIC: footer");
        return -1;
    }
    if (self->tail_ptr == 0) {
        if (self->size < 44) {
            PyErr_SetString(PyExc_ValueError, "ERR_OUT_OF_BOUNDS: stream footer missing tail pointer");
            return -1;
        }
        self->tail_ptr = rd_u64(self->data + self->size - 12);
    }

    memset(&self->nxs, 0, sizeof(self->nxs));
    nxs_err_t err = nxs_open(&self->nxs, self->data, (size_t)self->size);
    if (err != NXS_OK) {
        PyErr_SetString(PyExc_ValueError, nxs_err_msg(err));
        return -1;
    }
    self->has_nxs = 1;
    self->record_count = self->nxs.record_count;
    self->tail_start = (Py_ssize_t)self->nxs.tail_start;

    int kc = self->nxs.key_count;
    self->keys = PyList_New(kc);
    self->key_index = PyDict_New();
    if (!self->keys || !self->key_index) return -1;
    for (int i = 0; i < kc; i++) {
        PyObject *s = PyUnicode_FromString(self->nxs.keys[i]);
        if (!s) return -1;
        PyList_SET_ITEM(self->keys, i, s);
        PyObject *idx = PyLong_FromLong(i);
        if (!idx) return -1;
        if (PyDict_SetItem(self->key_index, s, idx) < 0) {
            Py_DECREF(idx);
            return -1;
        }
        Py_DECREF(idx);
    }
    return 0;
}

static void
Reader_dealloc(ReaderObject *self)
{
    if (self->has_nxs) nxs_close(&self->nxs);
    if (self->buffer_obj) {
        PyBuffer_Release(&self->view);
        Py_DECREF(self->buffer_obj);
    }
    Py_XDECREF(self->keys);
    Py_XDECREF(self->key_index);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

/* ── Object type ─────────────────────────────────────────────────────────── */

typedef struct {
    PyObject_HEAD
    ReaderObject *reader;            /* strong ref */
    Py_ssize_t offset;               /* row: NYXO offset; columnar/PAX: record index */
    Py_ssize_t offset_table_start;
    uint32_t record_index;           /* logical record index (columnar/PAX) */
    int layout_record;               /* 1 => use nxs_get_* via record_index */
    /* Expanded bitmask: 1 byte per slot (0 or 1). NULL if unparsed. */
    uint8_t *present;
    uint16_t present_len;
    /* Precomputed prefix sum: present_count[s] = count of set bits in slots [0, s) */
    uint16_t *rank;
    int parsed;
} ObjectView;

static ObjectView *
make_object(ReaderObject *reader, Py_ssize_t offset)
{
    ObjectView *obj = PyObject_New(ObjectView, &ObjectType);
    if (!obj) return NULL;
    Py_INCREF(reader);
    obj->reader = reader;
    obj->offset = offset;
    obj->record_index = 0;
    obj->layout_record = 0;
    obj->parsed = 0;
    obj->present = NULL;
    obj->rank = NULL;
    obj->present_len = 0;
    obj->offset_table_start = 0;
    return obj;
}

static int
object_obj_at_nyxo(const ObjectView *self)
{
    if (self->offset + 4 > self->reader->size)
        return 0;
    return rd_u32(self->reader->data + self->offset) == MAGIC_OBJ;
}

/* Columnar/PAX top-level records use record index; nested NYXO blobs use row paths. */
static int
object_layout_access(const ObjectView *self)
{
    if (!self->reader->has_nxs || self->reader->nxs.layout == NXS_LAYOUT_ROW)
        return 0;
    return !object_obj_at_nyxo(self);
}

static int
object_nxs_err_to_py(nxs_err_t err, const char *ctx)
{
    if (err == NXS_ERR_FIELD_ABSENT || err == NXS_ERR_KEY_NOT_FOUND)
        return 0;
    const char *code = nxs_err_msg(err);
    if (ctx) {
        PyErr_Format(PyExc_ValueError, "%s: %s", code, ctx);
    } else {
        PyErr_SetString(PyExc_ValueError, code);
    }
    return -1;
}

static void
Object_dealloc(ObjectView *self)
{
    if (self->present) PyMem_Free(self->present);
    if (self->rank) PyMem_Free(self->rank);
    Py_XDECREF(self->reader);
    PyObject_Free(self);
}

static int
Object_parse_header(ObjectView *self)
{
    if (self->parsed) return 0;
    const uint8_t *data = self->reader->data;
    Py_ssize_t size = self->reader->size;
    Py_ssize_t p = self->offset;

    if (p + 8 > size) {
        PyErr_SetString(PyExc_ValueError, "ERR_OUT_OF_BOUNDS: object header");
        return -1;
    }
    if (rd_u32(data + p) != MAGIC_OBJ) {
        PyErr_SetString(PyExc_ValueError, "ERR_BAD_MAGIC: object");
        return -1;
    }
    p += 8; /* skip magic + length */

    /* Read LEB128 bitmask into a sized array of single-bit values */
    uint16_t key_count = (uint16_t)PyList_GET_SIZE(self->reader->keys);
    self->present = (uint8_t *)PyMem_Calloc(key_count + 8, 1);
    if (!self->present) { PyErr_NoMemory(); return -1; }
    self->rank = (uint16_t *)PyMem_Calloc(key_count + 1, sizeof(uint16_t));
    if (!self->rank) { PyErr_NoMemory(); return -1; }

    uint16_t slot = 0;
    uint8_t byte;
    do {
        if (p >= size) {
            PyErr_SetString(PyExc_ValueError, "ERR_OUT_OF_BOUNDS: bitmask");
            return -1;
        }
        byte = data[p++];
        uint8_t data_bits = byte & 0x7F;
        for (int b = 0; b < 7 && slot < key_count; b++, slot++) {
            self->present[slot] = (data_bits >> b) & 1;
        }
    } while ((byte & 0x80) && slot < key_count);

    /* Build rank prefix-sum so slot-index → table-index is O(1) */
    uint16_t acc = 0;
    for (uint16_t s = 0; s < key_count; s++) {
        self->rank[s] = acc;
        acc += self->present[s];
    }
    self->rank[key_count] = acc;
    self->present_len = key_count;

    self->offset_table_start = p;
    self->parsed = 1;
    return 0;
}

/* Returns absolute byte offset of the value for `slot`, or -1 if absent. */
static Py_ssize_t
Object_field_offset(ObjectView *self, int slot)
{
    if (!self->parsed && Object_parse_header(self) < 0) return -1;
    if (slot < 0 || slot >= self->present_len) return -1;
    if (!self->present[slot]) return -1;

    uint16_t entry_idx = self->rank[slot];
    Py_ssize_t ofpos = self->offset_table_start + entry_idx * 2;
    uint16_t rel = rd_u16(self->reader->data + ofpos);
    return self->offset + rel;
}

/* ── Per-type accessors ──────────────────────────────────────────────────── */

/* Look up the slot index for a given key. Returns -1 if not found. */
static int
resolve_slot(ObjectView *self, PyObject *key)
{
    PyObject *idx_obj = PyDict_GetItemWithError(self->reader->key_index, key);
    if (!idx_obj) {
        if (PyErr_Occurred()) return -2;
        return -1;
    }
    return (int)PyLong_AsLong(idx_obj);
}

static PyObject *
Object_get_i64(ObjectView *self, PyObject *key)
{
    if (object_layout_access(self)) {
        const char *k = PyUnicode_AsUTF8(key);
        if (!k) return NULL;
        nxs_object_t obj;
        if (nxs_record(&self->reader->nxs, self->record_index, &obj) != NXS_OK) {
            PyErr_SetString(PyExc_ValueError, "ERR_OUT_OF_BOUNDS: record");
            return NULL;
        }
        int64_t v;
        nxs_err_t err = nxs_get_i64(&obj, k, &v);
        if (err == NXS_ERR_FIELD_ABSENT || err == NXS_ERR_KEY_NOT_FOUND)
            Py_RETURN_NONE;
        if (err != NXS_OK) {
            object_nxs_err_to_py(err, "i64");
            return NULL;
        }
        return PyLong_FromLongLong(v);
    }

    int slot = resolve_slot(self, key);
    if (slot == -2) return NULL;
    if (slot == -1) Py_RETURN_NONE;

    Py_ssize_t off = Object_field_offset(self, slot);
    if (off < 0) {
        if (PyErr_Occurred()) return NULL;
        Py_RETURN_NONE;
    }
    if (off + 8 > self->reader->size) {
        PyErr_SetString(PyExc_ValueError, "ERR_OUT_OF_BOUNDS: i64"); return NULL;
    }
    return PyLong_FromLongLong(rd_i64(self->reader->data + off));
}

static PyObject *
Object_get_f64(ObjectView *self, PyObject *key)
{
    if (object_layout_access(self)) {
        const char *k = PyUnicode_AsUTF8(key);
        if (!k) return NULL;
        nxs_object_t obj;
        if (nxs_record(&self->reader->nxs, self->record_index, &obj) != NXS_OK) {
            PyErr_SetString(PyExc_ValueError, "ERR_OUT_OF_BOUNDS: record");
            return NULL;
        }
        double v;
        nxs_err_t err = nxs_get_f64(&obj, k, &v);
        if (err == NXS_ERR_FIELD_ABSENT || err == NXS_ERR_KEY_NOT_FOUND)
            Py_RETURN_NONE;
        if (err != NXS_OK) {
            object_nxs_err_to_py(err, "f64");
            return NULL;
        }
        return PyFloat_FromDouble(v);
    }

    int slot = resolve_slot(self, key);
    if (slot == -2) return NULL;
    if (slot == -1) Py_RETURN_NONE;

    Py_ssize_t off = Object_field_offset(self, slot);
    if (off < 0) {
        if (PyErr_Occurred()) return NULL;
        Py_RETURN_NONE;
    }
    if (off + 8 > self->reader->size) {
        PyErr_SetString(PyExc_ValueError, "ERR_OUT_OF_BOUNDS: f64"); return NULL;
    }
    return PyFloat_FromDouble(rd_f64(self->reader->data + off));
}

static PyObject *
Object_get_bool(ObjectView *self, PyObject *key)
{
    if (object_layout_access(self)) {
        const char *k = PyUnicode_AsUTF8(key);
        if (!k) return NULL;
        nxs_object_t obj;
        if (nxs_record(&self->reader->nxs, self->record_index, &obj) != NXS_OK) {
            PyErr_SetString(PyExc_ValueError, "ERR_OUT_OF_BOUNDS: record");
            return NULL;
        }
        int v;
        nxs_err_t err = nxs_get_bool(&obj, k, &v);
        if (err == NXS_ERR_FIELD_ABSENT || err == NXS_ERR_KEY_NOT_FOUND)
            Py_RETURN_NONE;
        if (err != NXS_OK) {
            object_nxs_err_to_py(err, "bool");
            return NULL;
        }
        if (v) Py_RETURN_TRUE;
        Py_RETURN_FALSE;
    }

    int slot = resolve_slot(self, key);
    if (slot == -2) return NULL;
    if (slot == -1) Py_RETURN_NONE;

    Py_ssize_t off = Object_field_offset(self, slot);
    if (off < 0) {
        if (PyErr_Occurred()) return NULL;
        Py_RETURN_NONE;
    }
    if (off >= self->reader->size) {
        PyErr_SetString(PyExc_ValueError, "ERR_OUT_OF_BOUNDS: bool"); return NULL;
    }
    if (self->reader->data[off]) Py_RETURN_TRUE; else Py_RETURN_FALSE;
}

static PyObject *
py_unicode_from_nxs_str(nxs_object_t *obj, const char *key)
{
    char stack[4096];
    nxs_err_t err = nxs_get_str(obj, key, stack, sizeof(stack));
    if (err == NXS_ERR_FIELD_ABSENT || err == NXS_ERR_KEY_NOT_FOUND)
        return NULL;
    if (err != NXS_OK) {
        object_nxs_err_to_py(err, "str");
        return NULL;
    }
    size_t n = strlen(stack);
    if (n < sizeof(stack) - 1)
        return PyUnicode_FromString(stack);

    for (size_t cap = 8192; cap <= (size_t)32 * 1024 * 1024; cap *= 2) {
        char *heap = (char *)malloc(cap);
        if (!heap) {
            PyErr_NoMemory();
            return NULL;
        }
        err = nxs_get_str(obj, key, heap, cap);
        if (err == NXS_ERR_FIELD_ABSENT || err == NXS_ERR_KEY_NOT_FOUND) {
            free(heap);
            return NULL;
        }
        if (err != NXS_OK) {
            free(heap);
            object_nxs_err_to_py(err, "str");
            return NULL;
        }
        n = strlen(heap);
        if (n < cap - 1) {
            PyObject *out = PyUnicode_FromString(heap);
            free(heap);
            return out;
        }
        free(heap);
    }
    PyErr_SetString(PyExc_ValueError, "ERR_OUT_OF_BOUNDS: str too large");
    return NULL;
}

static PyObject *
Object_get_str(ObjectView *self, PyObject *key)
{
    if (object_layout_access(self)) {
        const char *k = PyUnicode_AsUTF8(key);
        if (!k) return NULL;
        nxs_object_t obj;
        if (nxs_record(&self->reader->nxs, self->record_index, &obj) != NXS_OK) {
            PyErr_SetString(PyExc_ValueError, "ERR_OUT_OF_BOUNDS: record");
            return NULL;
        }
        PyObject *s = py_unicode_from_nxs_str(&obj, k);
        if (!s) Py_RETURN_NONE;
        return s;
    }

    int slot = resolve_slot(self, key);
    if (slot == -2) return NULL;
    if (slot == -1) Py_RETURN_NONE;

    Py_ssize_t off = Object_field_offset(self, slot);
    if (off < 0) {
        if (PyErr_Occurred()) return NULL;
        Py_RETURN_NONE;
    }
    if (off + 4 > self->reader->size) {
        PyErr_SetString(PyExc_ValueError, "ERR_OUT_OF_BOUNDS: str len"); return NULL;
    }
    uint32_t n = rd_u32(self->reader->data + off);
    if ((size_t)off + 4u + (size_t)n > (size_t)self->reader->size) {
        PyErr_SetString(PyExc_ValueError, "ERR_OUT_OF_BOUNDS: str bytes"); return NULL;
    }
    return PyUnicode_DecodeUTF8((const char *)(self->reader->data + off + 4),
                                n, "strict");
}

static PyObject *
Object_field_offset_py(ObjectView *self, PyObject *key)
{
    if (object_layout_access(self)) {
        Py_RETURN_NONE;
    }
    int slot = resolve_slot(self, key);
    if (slot == -2) return NULL;
    if (slot == -1) Py_RETURN_NONE;
    Py_ssize_t off = Object_field_offset(self, slot);
    if (off < 0) {
        if (PyErr_Occurred()) return NULL;
        Py_RETURN_NONE;
    }
    return PyLong_FromSsize_t(off);
}

static PyMethodDef Object_methods[] = {
    {"get_i64",  (PyCFunction)Object_get_i64,  METH_O, "Read i64 field."},
    {"get_f64",  (PyCFunction)Object_get_f64,  METH_O, "Read f64 field."},
    {"get_bool", (PyCFunction)Object_get_bool, METH_O, "Read bool field."},
    {"get_str",  (PyCFunction)Object_get_str,  METH_O, "Read UTF-8 string field."},
    {"get_time", (PyCFunction)Object_get_i64,  METH_O, "Read time field (unix ns)."},
    {"field_offset", (PyCFunction)Object_field_offset_py, METH_O,
     "Absolute byte offset of field, or None if absent (row layout)."},
    {NULL, NULL, 0, NULL}
};

static PyTypeObject ObjectType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_nxs.Object",
    .tp_basicsize = sizeof(ObjectView),
    .tp_dealloc = (destructor)Object_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "NXS object view (lazy).",
    .tp_methods = Object_methods,
};

/* ── Bulk scan helpers ───────────────────────────────────────────────────── */

/*
 * Locate the absolute byte offset of a given slot's value in an object at
 * `obj_offset`. Returns -1 on absent/out-of-bounds. No allocation.
 *
 * This is the same algorithm as Object_field_offset but inlined and without
 * caching — optimised for a hot linear scan where we call it once per record.
 */
static inline Py_ssize_t
scan_field_offset(const uint8_t *data, Py_ssize_t size,
                  Py_ssize_t obj_offset, int slot)
{
    Py_ssize_t p = obj_offset + 8; /* skip NYXO magic + length */
    if (p > size) return -1;

    /* Walk LEB128 bitmask while:
     *   - checking whether target slot bit is set
     *   - counting present bits before it to get the offset-table index */
    int cur_slot = 0;
    int table_idx = 0;
    int found = 0;
    uint8_t byte;
    do {
        if (p >= size) return -1;
        byte = data[p++];
        uint8_t data_bits = byte & 0x7F;
        for (int b = 0; b < 7; b++) {
            if (cur_slot == slot) {
                if ((data_bits >> b) & 1) found = 1;
                else return -1;
            } else if (cur_slot < slot && ((data_bits >> b) & 1)) {
                table_idx++;
            }
            cur_slot++;
        }
        if (found && (byte & 0x80) == 0) break;
        if (cur_slot > slot && found) break;
    } while (byte & 0x80);

    if (!found) return -1;

    /* Skip the rest of the bitmask if we stopped mid-way — we need p at
     * offset_table_start */
    while (byte & 0x80) {
        if (p >= size) return -1;
        byte = data[p++];
    }

    Py_ssize_t ofpos = p + table_idx * 2;
    if (ofpos + 2 > size) return -1;
    uint16_t rel = rd_u16(data + ofpos);
    return obj_offset + rel;
}

/* Resolve a key name → slot index or return error with -2 / absent with -1 */
static int
reader_resolve_slot(ReaderObject *self, PyObject *key)
{
    PyObject *idx_obj = PyDict_GetItemWithError(self->key_index, key);
    if (!idx_obj) {
        if (PyErr_Occurred()) return -2;
        return -1;
    }
    return (int)PyLong_AsLong(idx_obj);
}

/* ── Reader methods ──────────────────────────────────────────────────────── */

static PyObject *
Reader_record(ReaderObject *self, PyObject *arg)
{
    long i = PyLong_AsLong(arg);
    if (i == -1 && PyErr_Occurred()) return NULL;
    if (i < 0 || (uint32_t)i >= self->record_count) {
        PyErr_Format(PyExc_IndexError, "record %ld out of [0, %u)",
                     i, self->record_count);
        return NULL;
    }
    if (self->has_nxs && self->nxs.layout != NXS_LAYOUT_ROW) {
        ObjectView *obj = make_object(self, i);
        if (!obj) return NULL;
        obj->record_index = (uint32_t)i;
        obj->layout_record = 1;
        return (PyObject *)obj;
    }
    Py_ssize_t entry = self->tail_start + i * 10;
    uint64_t abs_offset = rd_u64(self->data + entry + 2);
    return (PyObject *)make_object(self, (Py_ssize_t)abs_offset);
}

/*
 * Columnar scan: return a list of all values for `key` across all top-level
 * records. The key name and slot are resolved once; the LEB128 walk runs in C
 * with no Python overhead per record.
 */
static PyObject *
Reader_scan_f64(ReaderObject *self, PyObject *key)
{
    int slot = reader_resolve_slot(self, key);
    if (slot == -2) return NULL;
    if (slot == -1) {
        PyErr_SetString(PyExc_KeyError, "key not in schema");
        return NULL;
    }

    uint32_t n = self->record_count;
    PyObject *list = PyList_New(n);
    if (!list) return NULL;

    const uint8_t *data = self->data;
    Py_ssize_t size = self->size;
    Py_ssize_t tail = self->tail_start;

    for (uint32_t i = 0; i < n; i++) {
        uint64_t abs = rd_u64(data + tail + i * 10 + 2);
        Py_ssize_t off = scan_field_offset(data, size, (Py_ssize_t)abs, slot);
        PyObject *v;
        if (off < 0 || off + 8 > size) {
            Py_INCREF(Py_None);
            v = Py_None;
        } else {
            v = PyFloat_FromDouble(rd_f64(data + off));
            if (!v) { Py_DECREF(list); return NULL; }
        }
        PyList_SET_ITEM(list, i, v);
    }
    return list;
}

static PyObject *
Reader_scan_i64(ReaderObject *self, PyObject *key)
{
    int slot = reader_resolve_slot(self, key);
    if (slot == -2) return NULL;
    if (slot == -1) {
        PyErr_SetString(PyExc_KeyError, "key not in schema");
        return NULL;
    }

    uint32_t n = self->record_count;
    PyObject *list = PyList_New(n);
    if (!list) return NULL;

    const uint8_t *data = self->data;
    Py_ssize_t size = self->size;
    Py_ssize_t tail = self->tail_start;

    for (uint32_t i = 0; i < n; i++) {
        uint64_t abs = rd_u64(data + tail + i * 10 + 2);
        Py_ssize_t off = scan_field_offset(data, size, (Py_ssize_t)abs, slot);
        PyObject *v;
        if (off < 0 || off + 8 > size) {
            Py_INCREF(Py_None);
            v = Py_None;
        } else {
            v = PyLong_FromLongLong(rd_i64(data + off));
            if (!v) { Py_DECREF(list); return NULL; }
        }
        PyList_SET_ITEM(list, i, v);
    }
    return list;
}

static const char *
reader_key_cstr(PyObject *key, PyObject **tmp)
{
    if (PyUnicode_Check(key)) {
        return PyUnicode_AsUTF8(key);
    }
    *tmp = PyObject_Str(key);
    if (!*tmp) return NULL;
    return PyUnicode_AsUTF8(*tmp);
}

static PyObject *
Reader_col_buffer_dict(ReaderObject *self, const char *field)
{
    if (!self->has_nxs || self->nxs.layout == NXS_LAYOUT_ROW) {
        PyErr_SetString(PyExc_RuntimeError, "ERR_LAYOUT: col_buffer requires columnar or PAX layout");
        return NULL;
    }
    size_t val_len = 0;
    const void *vals = nxs_col_buffer(&self->nxs, field, &val_len);
    if (!vals) {
        PyErr_SetString(PyExc_KeyError, "col_buffer unavailable for field");
        return NULL;
    }
    size_t bm_len = 0;
    const uint8_t *bm = nxs_col_null_bitmap(&self->nxs, field, &bm_len);
    if (!bm) {
        PyErr_SetString(PyExc_KeyError, "col_null_bitmap unavailable for field");
        return NULL;
    }
    PyObject *dict = PyDict_New();
    PyObject *values_mv = PyMemoryView_FromMemory((char *)vals, (Py_ssize_t)val_len, PyBUF_READ);
    PyObject *bitmap_mv = PyMemoryView_FromMemory((char *)bm, (Py_ssize_t)bm_len, PyBUF_READ);
    PyObject *count = PyLong_FromUnsignedLong(self->record_count);
    if (!dict || !values_mv || !bitmap_mv || !count) {
        Py_XDECREF(dict);
        Py_XDECREF(values_mv);
        Py_XDECREF(bitmap_mv);
        Py_XDECREF(count);
        return NULL;
    }
    if (PyDict_SetItemString(dict, "values", values_mv) < 0 ||
        PyDict_SetItemString(dict, "bitmap", bitmap_mv) < 0 ||
        PyDict_SetItemString(dict, "count", count) < 0) {
        Py_DECREF(dict);
        Py_DECREF(values_mv);
        Py_DECREF(bitmap_mv);
        Py_DECREF(count);
        return NULL;
    }
    Py_DECREF(values_mv);
    Py_DECREF(bitmap_mv);
    Py_DECREF(count);
    return dict;
}

static PyObject *
Reader_col_var_buffer(ReaderObject *self, PyObject *key)
{
    PyObject *tmp = NULL;
    const char *field = reader_key_cstr(key, &tmp);
    if (!field) {
        Py_XDECREF(tmp);
        return NULL;
    }
    if (!self->has_nxs || self->nxs.layout != NXS_LAYOUT_COLUMNAR) {
        Py_XDECREF(tmp);
        PyErr_SetString(PyExc_RuntimeError,
                         "ERR_LAYOUT: col_var_buffer is columnar-only (use record get_str on PAX)");
        return NULL;
    }
    const uint8_t *bitmap = NULL, *offsets = NULL, *values = NULL;
    size_t bm_len = 0, off_len = 0, val_len = 0;
    nxs_err_t err = nxs_col_var_buffer(&self->nxs, field, &bitmap, &bm_len,
                                       &offsets, &off_len, &values, &val_len);
    Py_XDECREF(tmp);
    if (err != NXS_OK) {
        PyErr_SetString(PyExc_ValueError, nxs_err_msg(err));
        return NULL;
    }
    PyObject *dict = PyDict_New();
    PyObject *bitmap_mv = PyMemoryView_FromMemory((char *)bitmap, (Py_ssize_t)bm_len, PyBUF_READ);
    PyObject *offsets_mv = PyMemoryView_FromMemory((char *)offsets, (Py_ssize_t)off_len, PyBUF_READ);
    PyObject *values_mv = PyMemoryView_FromMemory((char *)values, (Py_ssize_t)val_len, PyBUF_READ);
    PyObject *count = PyLong_FromUnsignedLong(self->record_count);
    if (!dict || !bitmap_mv || !offsets_mv || !values_mv || !count) {
        Py_XDECREF(dict);
        Py_XDECREF(bitmap_mv);
        Py_XDECREF(offsets_mv);
        Py_XDECREF(values_mv);
        Py_XDECREF(count);
        return NULL;
    }
    if (PyDict_SetItemString(dict, "bitmap", bitmap_mv) < 0 ||
        PyDict_SetItemString(dict, "offsets", offsets_mv) < 0 ||
        PyDict_SetItemString(dict, "values", values_mv) < 0 ||
        PyDict_SetItemString(dict, "count", count) < 0) {
        Py_DECREF(dict);
        Py_DECREF(bitmap_mv);
        Py_DECREF(offsets_mv);
        Py_DECREF(values_mv);
        Py_DECREF(count);
        return NULL;
    }
    Py_DECREF(bitmap_mv);
    Py_DECREF(offsets_mv);
    Py_DECREF(values_mv);
    Py_DECREF(count);
    return dict;
}

static PyObject *
Reader_col_buffer(ReaderObject *self, PyObject *key)
{
    PyObject *tmp = NULL;
    const char *field = reader_key_cstr(key, &tmp);
    if (!field) {
        Py_XDECREF(tmp);
        return NULL;
    }
    PyObject *out = Reader_col_buffer_dict(self, field);
    Py_XDECREF(tmp);
    return out;
}

static PyObject *
Reader_col_numpy_f64(ReaderObject *self, PyObject *key)
{
    PyObject *tmp = NULL;
    const char *field = reader_key_cstr(key, &tmp);
    if (!field) {
        Py_XDECREF(tmp);
        return NULL;
    }
    PyObject *buf = Reader_col_buffer_dict(self, field);
    Py_XDECREF(tmp);
    if (!buf) return NULL;

    PyObject *numpy = PyImport_ImportModule("numpy");
    if (!numpy) {
        Py_DECREF(buf);
        return NULL;
    }
    PyObject *values = PyDict_GetItemString(buf, "values");
    PyObject *count_obj = PyDict_GetItemString(buf, "count");
    if (!values || !count_obj) {
        Py_DECREF(buf);
        Py_DECREF(numpy);
        PyErr_SetString(PyExc_RuntimeError, "col_buffer dict missing keys");
        return NULL;
    }
    PyObject *dtype = PyObject_GetAttrString(numpy, "float64");
    if (!dtype) {
        Py_DECREF(buf);
        Py_DECREF(numpy);
        return NULL;
    }
    long count = PyLong_AsLong(count_obj);
    if (count < 0 && PyErr_Occurred()) {
        Py_DECREF(dtype);
        Py_DECREF(buf);
        Py_DECREF(numpy);
        return NULL;
    }
    PyObject *arr = PyObject_CallMethod(numpy, "frombuffer", "Osl", values, dtype, count);
    Py_DECREF(dtype);
    Py_DECREF(buf);
    Py_DECREF(numpy);
    return arr;
}

static PyObject *
Reader_prefetch_column(ReaderObject *self, PyObject *key)
{
    if (!self->has_nxs) {
        PyErr_SetString(PyExc_RuntimeError, "prefetch_column requires C reader open");
        return NULL;
    }
    if (!PyUnicode_Check(key)) {
        PyErr_SetString(PyExc_TypeError, "key must be str");
        return NULL;
    }
    const char *field = PyUnicode_AsUTF8(key);
    if (!field) return NULL;
    nxs_err_t err = nxs_prefetch_column(&self->nxs, field);
    if (err != NXS_OK) {
        PyErr_SetString(PyExc_RuntimeError, nxs_err_msg(err));
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
Reader_col_sum_f64(ReaderObject *self, PyObject *key)
{
    PyObject *tmp = NULL;
    const char *field = reader_key_cstr(key, &tmp);
    if (!field) {
        Py_XDECREF(tmp);
        return NULL;
    }
    if (!self->has_nxs) {
        Py_XDECREF(tmp);
        PyErr_SetString(PyExc_RuntimeError, "reader not initialized");
        return NULL;
    }
    double sum = nxs_col_sum_f64(&self->nxs, field);
    Py_XDECREF(tmp);
    return PyFloat_FromDouble(sum);
}

static PyObject *
Reader_get_layout(ReaderObject *self, void *closure)
{
    (void)closure;
    const char *name = "row";
    if (self->has_nxs) {
        switch (self->nxs.layout) {
        case NXS_LAYOUT_COLUMNAR: name = "columnar"; break;
        case NXS_LAYOUT_PAX: name = "pax"; break;
        default: break;
        }
    }
    return PyUnicode_FromString(name);
}

/* In-native reducers: sum / min / max / count over an f64 field. */
static PyObject *
Reader_sum_f64(ReaderObject *self, PyObject *key)
{
    if (self->has_nxs && self->nxs.layout != NXS_LAYOUT_ROW) {
        return Reader_col_sum_f64(self, key);
    }
    int slot = reader_resolve_slot(self, key);
    if (slot == -2) return NULL;
    if (slot == -1) {
        PyErr_SetString(PyExc_KeyError, "key not in schema"); return NULL;
    }
    const uint8_t *data = self->data;
    Py_ssize_t size = self->size;
    Py_ssize_t tail = self->tail_start;
    uint32_t n = self->record_count;
    double sum = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t abs = rd_u64(data + tail + i * 10 + 2);
        Py_ssize_t off = scan_field_offset(data, size, (Py_ssize_t)abs, slot);
        if (off < 0 || off + 8 > size) continue;
        sum += rd_f64(data + off);
    }
    return PyFloat_FromDouble(sum);
}

static PyObject *
Reader_min_f64(ReaderObject *self, PyObject *key)
{
    int slot = reader_resolve_slot(self, key);
    if (slot == -2) return NULL;
    if (slot == -1) {
        PyErr_SetString(PyExc_KeyError, "key not in schema"); return NULL;
    }
    const uint8_t *data = self->data;
    Py_ssize_t size = self->size;
    Py_ssize_t tail = self->tail_start;
    uint32_t n = self->record_count;
    double m = 0; int have = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t abs = rd_u64(data + tail + i * 10 + 2);
        Py_ssize_t off = scan_field_offset(data, size, (Py_ssize_t)abs, slot);
        if (off < 0 || off + 8 > size) continue;
        double v = rd_f64(data + off);
        if (!have || v < m) { m = v; have = 1; }
    }
    if (!have) Py_RETURN_NONE;
    return PyFloat_FromDouble(m);
}

static PyObject *
Reader_max_f64(ReaderObject *self, PyObject *key)
{
    int slot = reader_resolve_slot(self, key);
    if (slot == -2) return NULL;
    if (slot == -1) {
        PyErr_SetString(PyExc_KeyError, "key not in schema"); return NULL;
    }
    const uint8_t *data = self->data;
    Py_ssize_t size = self->size;
    Py_ssize_t tail = self->tail_start;
    uint32_t n = self->record_count;
    double m = 0; int have = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t abs = rd_u64(data + tail + i * 10 + 2);
        Py_ssize_t off = scan_field_offset(data, size, (Py_ssize_t)abs, slot);
        if (off < 0 || off + 8 > size) continue;
        double v = rd_f64(data + off);
        if (!have || v > m) { m = v; have = 1; }
    }
    if (!have) Py_RETURN_NONE;
    return PyFloat_FromDouble(m);
}

static PyObject *
Reader_sum_i64(ReaderObject *self, PyObject *key)
{
    int slot = reader_resolve_slot(self, key);
    if (slot == -2) return NULL;
    if (slot == -1) {
        PyErr_SetString(PyExc_KeyError, "key not in schema"); return NULL;
    }
    const uint8_t *data = self->data;
    Py_ssize_t size = self->size;
    Py_ssize_t tail = self->tail_start;
    uint32_t n = self->record_count;
    int64_t sum = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t abs = rd_u64(data + tail + i * 10 + 2);
        Py_ssize_t off = scan_field_offset(data, size, (Py_ssize_t)abs, slot);
        if (off < 0 || off + 8 > size) continue;
        sum += rd_i64(data + off);
    }
    return PyLong_FromLongLong(sum);
}

static PyObject *
Reader_get_record_count(ReaderObject *self, void *closure)
{
    (void)closure;
    return PyLong_FromUnsignedLong(self->record_count);
}

static PyObject *
Reader_get_keys(ReaderObject *self, void *closure)
{
    (void)closure;
    Py_INCREF(self->keys);
    return self->keys;
}

static PyObject *
Reader_get_key_index(ReaderObject *self, void *closure)
{
    (void)closure;
    Py_INCREF(self->key_index);
    return self->key_index;
}

static PyObject *
Reader_get_key_sigils(ReaderObject *self, void *closure)
{
    (void)closure;
    int kc = self->has_nxs ? self->nxs.key_count : (int)PyList_GET_SIZE(self->keys);
    PyObject *tup = PyTuple_New(kc);
    if (!tup) return NULL;
    for (int i = 0; i < kc; i++) {
        int sig = self->has_nxs ? (int)self->nxs.key_sigils[i] : 0x22;
        PyObject *v = PyLong_FromLong(sig);
        if (!v) {
            Py_DECREF(tup);
            return NULL;
        }
        PyTuple_SET_ITEM(tup, i, v);
    }
    return tup;
}

static PyObject *
Reader_get_buffer(ReaderObject *self, void *closure)
{
    (void)closure;
    return PyMemoryView_FromMemory((char *)self->data, self->size, PyBUF_READ);
}

static PyMethodDef Reader_methods[] = {
    {"record",   (PyCFunction)Reader_record,   METH_O, "Get the object at index i."},
    {"scan_f64", (PyCFunction)Reader_scan_f64, METH_O, "List of all f64 values for key."},
    {"scan_i64", (PyCFunction)Reader_scan_i64, METH_O, "List of all i64 values for key."},
    {"sum_f64",  (PyCFunction)Reader_sum_f64,  METH_O, "Sum of an f64 field across all records."},
    {"prefetch_column", (PyCFunction)Reader_prefetch_column, METH_O,
     "Prefetch one columnar column buffer (§7.4)."},
    {"col_sum_f64", (PyCFunction)Reader_col_sum_f64, METH_O, "Columnar/PAX sum of f64 field."},
    {"col_buffer", (PyCFunction)Reader_col_buffer, METH_O,
     "dict(values, bitmap, count) memoryviews for columnar/PAX numeric field."},
    {"col_var_buffer", (PyCFunction)Reader_col_var_buffer, METH_O,
     "dict(bitmap, offsets, values, count) for string/binary columns."},
    {"col_numpy_f64", (PyCFunction)Reader_col_numpy_f64, METH_O,
     "numpy.ndarray view of f64 column (requires numpy)."},
    {"min_f64",  (PyCFunction)Reader_min_f64,  METH_O, "Min of an f64 field across all records."},
    {"max_f64",  (PyCFunction)Reader_max_f64,  METH_O, "Max of an f64 field across all records."},
    {"sum_i64",  (PyCFunction)Reader_sum_i64,  METH_O, "Sum of an i64 field across all records."},
    {NULL, NULL, 0, NULL}
};

static PyGetSetDef Reader_getset[] = {
    {"record_count", (getter)Reader_get_record_count, NULL, "Total top-level records.", NULL},
    {"keys",         (getter)Reader_get_keys,         NULL, "Schema keys.", NULL},
    {"key_index",    (getter)Reader_get_key_index,    NULL, "dict key → slot.", NULL},
    {"key_sigils",   (getter)Reader_get_key_sigils,   NULL, "tuple of schema sigil bytes.", NULL},
    {"buffer",       (getter)Reader_get_buffer,       NULL, "memoryview over file bytes.", NULL},
    {"layout",       (getter)Reader_get_layout,       NULL, "row | columnar | pax", NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyTypeObject ReaderType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_nxs.Reader",
    .tp_basicsize = sizeof(ReaderObject),
    .tp_dealloc = (destructor)Reader_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "NXS binary file reader (C accelerated).",
    .tp_init = (initproc)Reader_init,
    .tp_new = PyType_GenericNew,
    .tp_methods = Reader_methods,
    .tp_getset = Reader_getset,
};

/* ── Writer / Schema C types ─────────────────────────────────────────────── */
/*
 * Embed the C writer implementation directly — same source used by c/bench_wal.
 * We rename the public symbols via #define to avoid clashes with the reader's
 * own static helpers (put_u32 etc.).  The writer source is self-contained.
 */
#define NXS_WRITER_IMPL_ONLY
#include "../c/nxs_writer.h"
#include "../c/nxs_writer.c"

/* ── Schema type ─────────────────────────────────────────────────────────── */

typedef struct {
    PyObject_HEAD
    char   *key_buf;           /* single heap block: all key strings */
    char  **keys;              /* pointers into key_buf              */
    int     key_count;
} SchemaObject;

static void
Schema_dealloc(SchemaObject *self)
{
    free(self->key_buf);
    free(self->keys);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int
Schema_init(SchemaObject *self, PyObject *args, PyObject *kwds)
{
    (void)kwds;
    PyObject *list;
    if (!PyArg_ParseTuple(args, "O!", &PyList_Type, &list))
        return -1;

    Py_ssize_t n = PyList_GET_SIZE(list);
    if (n <= 0 || n > NXS_WRITER_MAX_KEYS) {
        PyErr_SetString(PyExc_ValueError, "key list must have 1–256 entries");
        return -1;
    }

    /* Compute total byte size for all key strings (null-terminated) */
    size_t total = 0;
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *k = PyList_GET_ITEM(list, i);
        if (!PyUnicode_Check(k)) { PyErr_SetString(PyExc_TypeError, "keys must be str"); return -1; }
        total += PyUnicode_GET_LENGTH(k) + 1;
    }

    self->key_buf   = (char *)malloc(total);
    self->keys      = (char **)malloc((size_t)n * sizeof(char *));
    if (!self->key_buf || !self->keys) { free(self->key_buf); free(self->keys); PyErr_NoMemory(); return -1; }

    char *p = self->key_buf;
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *k = PyList_GET_ITEM(list, i);
        const char *s = PyUnicode_AsUTF8(k);
        if (!s) return -1;
        size_t slen = strlen(s);
        memcpy(p, s, slen + 1);
        self->keys[i] = p;
        p += slen + 1;
    }
    self->key_count = (int)n;
    return 0;
}

static PyObject *
Schema_get_keys(SchemaObject *self, void *closure)
{
    (void)closure;
    PyObject *lst = PyList_New(self->key_count);
    if (!lst) return NULL;
    for (int i = 0; i < self->key_count; i++)
        PyList_SET_ITEM(lst, i, PyUnicode_FromString(self->keys[i]));
    return lst;
}

static PyGetSetDef Schema_getset[] = {
    {"keys", (getter)Schema_get_keys, NULL, "Schema key list.", NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyTypeObject SchemaType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "_nxs.Schema",
    .tp_basicsize = sizeof(SchemaObject),
    .tp_dealloc   = (destructor)Schema_dealloc,
    .tp_flags     = Py_TPFLAGS_DEFAULT,
    .tp_doc       = "NXS schema: precompiled key list for Writer.",
    .tp_init      = (initproc)Schema_init,
    .tp_new       = PyType_GenericNew,
    .tp_getset    = Schema_getset,
};

/* ── Writer type ─────────────────────────────────────────────────────────── */

typedef struct {
    PyObject_HEAD
    nxs_writer_t w;
    int          initialised;
} WriterObject;

static void
Writer_dealloc(WriterObject *self)
{
    if (self->initialised)
        nxs_writer_free(&self->w);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int
Writer_init(WriterObject *self, PyObject *args, PyObject *kwds)
{
    (void)kwds;
    SchemaObject *schema;
    if (!PyArg_ParseTuple(args, "O!", &SchemaType, &schema))
        return -1;

    if (nxs_writer_init(&self->w,
                        (const char **)schema->keys,
                        schema->key_count,
                        4096) != 0) {
        PyErr_NoMemory();
        return -1;
    }
    self->initialised = 1;
    return 0;
}

static PyObject *
Writer_begin_object(WriterObject *self, PyObject *Py_UNUSED(ignored))
{
    if (nxs_writer_begin_object(&self->w) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "nxs_writer_begin_object failed");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
Writer_end_object(WriterObject *self, PyObject *Py_UNUSED(ignored))
{
    if (nxs_writer_end_object(&self->w) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "nxs_writer_end_object failed");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
Writer_write_i64(WriterObject *self, PyObject *args)
{
    int slot; long long v;
    if (!PyArg_ParseTuple(args, "iL", &slot, &v)) return NULL;
    if (nxs_write_i64(&self->w, slot, (int64_t)v) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "nxs_write_i64 failed");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
Writer_write_f64(WriterObject *self, PyObject *args)
{
    int slot; double v;
    if (!PyArg_ParseTuple(args, "id", &slot, &v)) return NULL;
    if (nxs_write_f64(&self->w, slot, v) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "nxs_write_f64 failed");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
Writer_write_bool(WriterObject *self, PyObject *args)
{
    int slot, v;
    if (!PyArg_ParseTuple(args, "ip", &slot, &v)) return NULL;
    if (nxs_write_bool(&self->w, slot, v) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "nxs_write_bool failed");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
Writer_write_null(WriterObject *self, PyObject *args)
{
    int slot;
    if (!PyArg_ParseTuple(args, "i", &slot)) return NULL;
    if (nxs_write_null(&self->w, slot) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "nxs_write_null failed");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
Writer_write_str(WriterObject *self, PyObject *args)
{
    int slot;
    const char *s;
    Py_ssize_t slen;
    if (!PyArg_ParseTuple(args, "is#", &slot, &s, &slen)) return NULL;
    if (nxs_write_str(&self->w, slot, s, (uint32_t)slen) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "nxs_write_str failed");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
Writer_write_bytes(WriterObject *self, PyObject *args)
{
    int slot;
    const uint8_t *data;
    Py_ssize_t dlen;
    if (!PyArg_ParseTuple(args, "iy#", &slot, &data, &dlen)) return NULL;
    if (nxs_write_bytes(&self->w, slot, data, (uint32_t)dlen) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "nxs_write_bytes failed");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
Writer_finish(WriterObject *self, PyObject *Py_UNUSED(ignored))
{
    if (nxs_writer_finish(&self->w) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "nxs_writer_finish failed");
        return NULL;
    }
    PyObject *result = PyBytes_FromStringAndSize(
        (const char *)self->w.out, (Py_ssize_t)self->w.out_size);
    return result;
}

/* Return raw NYXO bytes of all records (data sector only, no preamble). */
static PyObject *
Writer_data_sector(WriterObject *self, PyObject *Py_UNUSED(ignored))
{
    return PyBytes_FromStringAndSize(
        (const char *)self->w.buf, (Py_ssize_t)self->w.buf_pos);
}

static PyObject *
Writer_reset(WriterObject *self, PyObject *Py_UNUSED(ignored))
{
    nxs_writer_reset(&self->w);
    Py_RETURN_NONE;
}

static PyMethodDef Writer_methods[] = {
    {"begin_object",  (PyCFunction)Writer_begin_object,  METH_NOARGS,  "Open an object."},
    {"end_object",    (PyCFunction)Writer_end_object,    METH_NOARGS,  "Close the current object."},
    {"write_i64",     (PyCFunction)Writer_write_i64,     METH_VARARGS, "write_i64(slot, v)"},
    {"write_f64",     (PyCFunction)Writer_write_f64,     METH_VARARGS, "write_f64(slot, v)"},
    {"write_bool",    (PyCFunction)Writer_write_bool,    METH_VARARGS, "write_bool(slot, v)"},
    {"write_null",    (PyCFunction)Writer_write_null,    METH_VARARGS, "write_null(slot)"},
    {"write_str",     (PyCFunction)Writer_write_str,     METH_VARARGS, "write_str(slot, s)"},
    {"write_bytes",   (PyCFunction)Writer_write_bytes,   METH_VARARGS, "write_bytes(slot, data)"},
    {"finish",        (PyCFunction)Writer_finish,        METH_NOARGS,  "Assemble and return complete .nxb bytes."},
    {"data_sector",   (PyCFunction)Writer_data_sector,   METH_NOARGS,  "Return raw NYXO bytes (no preamble, WAL path)."},
    {"reset",         (PyCFunction)Writer_reset,         METH_NOARGS,  "Reset writer state, keeping buffer allocation."},
    {NULL, NULL, 0, NULL}
};

static PyTypeObject WriterType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "_nxs.Writer",
    .tp_basicsize = sizeof(WriterObject),
    .tp_dealloc   = (destructor)Writer_dealloc,
    .tp_flags     = Py_TPFLAGS_DEFAULT,
    .tp_doc       = "NXS Writer: direct-to-buffer .nxb emitter (C accelerated).",
    .tp_init      = (initproc)Writer_init,
    .tp_new       = PyType_GenericNew,
    .tp_methods   = Writer_methods,
};

/* ── Module setup ────────────────────────────────────────────────────────── */

static PyModuleDef nxs_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "_nxs",
    .m_doc  = "NXS binary format reader + writer (C extension).",
    .m_size = -1,
};

PyMODINIT_FUNC
PyInit__nxs(void)
{
    if (PyType_Ready(&ReaderType) < 0) return NULL;
    if (PyType_Ready(&ObjectType) < 0) return NULL;
    if (PyType_Ready(&SchemaType) < 0) return NULL;
    if (PyType_Ready(&WriterType) < 0) return NULL;

    PyObject *m = PyModule_Create(&nxs_module);
    if (!m) return NULL;

#define ADD_TYPE(name, type) \
    Py_INCREF(&(type)); \
    if (PyModule_AddObject(m, (name), (PyObject *)&(type)) < 0) { \
        Py_DECREF(&(type)); Py_DECREF(m); return NULL; \
    }

    ADD_TYPE("Reader", ReaderType)
    ADD_TYPE("Schema", SchemaType)
    ADD_TYPE("Writer", WriterType)
#undef ADD_TYPE

    return m;
}
