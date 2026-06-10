// NXS v1.3 compact row decode implementation (C99).
#include "nxs.h"
#include "compact.h"
#include <stdlib.h>
#include <string.h>

static inline uint16_t rd_u16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}
static inline uint32_t rd_u32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static inline uint64_t rd_u64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}
static inline float rd_f32(const uint8_t *p) {
    float v;
    memcpy(&v, p, 4);
    return v;
}

size_t nxs_align_to(size_t pos, size_t align) {
    if (!align) return pos;
    return (pos + align - 1u) & ~(align - 1u);
}

static int is_var_sigil(uint8_t sig) {
    return sig == NXS_SIGIL_STR || sig == 0x3cu;
}

static int is_promoted(const nxs_ext_schema_t *ext, int slot) {
    return (ext->field_attrs[slot] & NXS_FIELD_ATTR_PROMOTED) != 0;
}

static int is_u16_len(const nxs_ext_schema_t *ext, int slot) {
    return (ext->field_attrs[slot] & NXS_FIELD_ATTR_U16_LEN) != 0;
}

static int str_len_prefix(const nxs_ext_schema_t *ext, int slot) {
    if (is_promoted(ext, slot)) return 0;
    if (is_u16_len(ext, slot)) return 2;
    return 4;
}

static uint8_t cell_width(const struct nxs_reader_t *r, const nxs_ext_schema_t *ext, int slot) {
    if (is_promoted(ext, slot) || r->key_sigils[slot] == NXS_SIGIL_KEYWORD) return 2;
    uint8_t w = ext->field_widths[slot];
    return w == 0 ? 8 : w;
}

uint8_t nxs_field_cell_width(const struct nxs_reader_t *r, const nxs_ext_schema_t *ext,
                             int slot) {
    return cell_width(r, ext, slot);
}

static int bool_slot_index(const nxs_cell_plan_t *plan, int slot) {
    for (int i = 0; i < plan->bool_slot_count; i++) {
        if (plan->bool_slots[i] == slot) return i;
    }
    return -1;
}

static int plan_bool_word_bytes(const nxs_cell_plan_t *plan) {
    if (!plan->packed_bools || plan->bool_slot_count == 0) return 0;
    int n = (plan->bool_slot_count + 7) / 8;
    return n < 1 ? 1 : n;
}

static size_t read_str_cell_len(const uint8_t *data, size_t size, size_t off,
                                int prefix_len) {
    if (prefix_len == 2) {
        if (off > size || size - off < 2) return (size_t)-1;
        return rd_u16(data + off);
    }
    if (prefix_len == 4) {
        if (off > size || size - off < 4) return (size_t)-1;
        return rd_u32(data + off);
    }
    return (size_t)-1;
}

static size_t advance_past_str_cell(size_t pos, size_t payload_len, int prefix_len) {
    size_t cell_bytes = (size_t)prefix_len + payload_len;
    size_t pad = (8u - (cell_bytes % 8u)) % 8u;
    return pos + cell_bytes + pad;
}

static size_t advance_past_bool_word(size_t pos, const nxs_cell_plan_t *plan) {
    size_t bw = (size_t)plan_bool_word_bytes(plan);
    return nxs_align_to(pos, bw) + bw;
}

static size_t dense_cell_align_width(const struct nxs_reader_t *r, const nxs_ext_schema_t *ext,
                                     int fi, const nxs_cell_plan_t *plan) {
    if (plan->packed_bools && bool_slot_index(plan, fi) >= 0)
        return (size_t)plan_bool_word_bytes(plan);
    uint8_t sig = r->key_sigils[fi];
    if (is_var_sigil(sig)) return 0;
    if (is_promoted(ext, fi) || sig == NXS_SIGIL_KEYWORD) return 2;
    if (sig == NXS_SIGIL_INT || sig == NXS_SIGIL_FLOAT)
        return plan->narrow ? cell_width(r, ext, fi) : 8;
    if (sig == 0x40u) return 8;
    if (sig == NXS_SIGIL_BOOL && !plan->packed_bools) return 8;
    if (sig == 0x5eu) return 1;
    return 8;
}

typedef struct { size_t width; int fi; } wire_entry_t;

static int wire_entry_cmp(const void *a, const void *b) {
    const wire_entry_t *ea = (const wire_entry_t *)a;
    const wire_entry_t *eb = (const wire_entry_t *)b;
    if (ea->width != eb->width)
        return ea->width > eb->width ? -1 : 1;
    return ea->fi < eb->fi ? -1 : ea->fi > eb->fi ? 1 : 0;
}

static void dense_wire_order_fill(const struct nxs_reader_t *r, const nxs_ext_schema_t *ext,
                                  const nxs_cell_plan_t *plan,
                                  int *out, int *out_count) {
    int n = r->key_count;
    *out_count = 0;
    if (!plan->dense_wire_reorder) {
        for (int i = 0; i < n; i++) out[(*out_count)++] = i;
        return;
    }
    wire_entry_t fixed[NXS_MAX_KEYS];
    int vars[NXS_MAX_KEYS];
    int fc = 0, vc = 0;
    for (int fi = 0; fi < n; fi++) {
        uint8_t sig = r->key_sigils[fi];
        if (is_var_sigil(sig)) {
            vars[vc++] = fi;
            continue;
        }
        if (plan->packed_bools && bool_slot_index(plan, fi) >= 0) {
            if (fi == plan->first_bool)
                fixed[fc++] = (wire_entry_t){ dense_cell_align_width(r, ext, fi, plan), fi };
            continue;
        }
        fixed[fc++] = (wire_entry_t){ dense_cell_align_width(r, ext, fi, plan), fi };
    }
    qsort(fixed, (size_t)fc, sizeof(fixed[0]), wire_entry_cmp);
    for (int i = 0; i < fc; i++) out[(*out_count)++] = fixed[i].fi;
    for (int i = 0; i < vc; i++) out[(*out_count)++] = vars[i];
}

static size_t advance_dense_past_cell_fixed(size_t pos, int fi,
                                            const struct nxs_reader_t *r,
                                            const nxs_ext_schema_t *ext,
                                            const nxs_cell_plan_t *plan) {
    uint8_t sig = r->key_sigils[fi];
    size_t w = plan->narrow ? cell_width(r, ext, fi) : 8;
    if (plan->packed_bools && bool_slot_index(plan, fi) >= 0)
        return advance_past_bool_word(pos, plan);
    if ((sig == NXS_SIGIL_INT || sig == NXS_SIGIL_FLOAT || sig == NXS_SIGIL_BOOL) &&
        (!plan->packed_bools || sig != NXS_SIGIL_BOOL))
        return nxs_align_to(pos, w) + w;
    if (sig == NXS_SIGIL_KEYWORD) return nxs_align_to(pos, 2) + 2;
    return nxs_align_to(pos, 8) + 8;
}

static void precompute_dense_fixed_offsets(const struct nxs_reader_t *r,
                                           const nxs_ext_schema_t *ext,
                                           nxs_cell_plan_t *plan) {
    int n = r->key_count;
    for (int i = 0; i < n; i++) plan->dense_fixed_body_offsets[i] = -1;
    plan->dense_var_body_start = -1;
    plan->has_dense_fixed = 0;

    size_t pos = 0;
    for (int wi = 0; wi < plan->wire_order_count; wi++) {
        int fi = plan->wire_order[wi];
        if (plan->packed_bools && bool_slot_index(plan, fi) >= 0) {
            if (fi == plan->first_bool) {
                size_t bw = (size_t)plan_bool_word_bytes(plan);
                size_t base = nxs_align_to(pos, bw);
                for (int idx = 0; idx < plan->bool_slot_count; idx++) {
                    int bs = plan->bool_slots[idx];
                    plan->dense_fixed_body_offsets[bs] = (int32_t)(base + (size_t)(idx / 8));
                }
                pos = base + bw;
            }
            continue;
        }
        uint8_t sig = r->key_sigils[fi];
        if (is_var_sigil(sig)) {
            plan->dense_var_body_start = (int)pos;
            plan->has_dense_fixed = 1;
            return;
        }
        size_t w = plan->narrow ? cell_width(r, ext, fi) : 8;
        size_t off = (is_promoted(ext, fi) || sig == NXS_SIGIL_KEYWORD)
                         ? nxs_align_to(pos, 2)
                         : nxs_align_to(pos, w);
        plan->dense_fixed_body_offsets[fi] = (int32_t)off;
        pos = advance_dense_past_cell_fixed(pos, fi, r, ext, plan);
    }
    plan->has_dense_fixed = 1;
}

void nxs_cell_plan_build(const struct nxs_reader_t *r, const nxs_ext_schema_t *ext,
                         uint16_t flags, nxs_cell_plan_t *plan) {
    memset(plan, 0, sizeof(*plan));
    plan->packed_bools = (flags & NXS_FLAG_PACKED_BOOLS) != 0;
    plan->narrow = (flags & NXS_FLAG_NARROW_CELLS) != 0;
    plan->dense_allowed = (flags & NXS_FLAG_DENSE_FRAMES) != 0;
    plan->dense_wire_reorder = (flags & NXS_FLAG_DENSE_WIRE_REORDER) != 0;
    plan->first_bool = -1;
    plan->dense_var_body_start = -1;

    for (int i = 0; i < r->key_count; i++) {
        if (r->key_sigils[i] == NXS_SIGIL_BOOL) {
            plan->bool_slots[plan->bool_slot_count++] = i;
        }
    }
    if (plan->bool_slot_count > 0) plan->first_bool = plan->bool_slots[0];

    dense_wire_order_fill(r, ext, plan, plan->wire_order, &plan->wire_order_count);

    if (plan->dense_wire_reorder && plan->dense_allowed)
        precompute_dense_fixed_offsets(r, ext, plan);
}

void nxs_ext_schema_free(nxs_ext_schema_t *ext) {
    if (!ext) return;
    if (ext->value_pool) {
        for (int i = 0; i < ext->value_pool_count; i++)
            free(ext->value_pool[i]);
        free(ext->value_pool);
        ext->value_pool = NULL;
    }
    ext->value_pool_count = 0;
}

int nxs_parse_extended_schema(const uint8_t *data, size_t size, size_t pos,
                                    uint16_t flags, struct nxs_reader_t *r,
                                    nxs_ext_schema_t *out, size_t *end_out) {
    if (!data || !r || !out || !end_out || pos + 2 > size) return NXS_ERR_OUT_OF_BOUNDS;
    memset(out, 0, sizeof(*out));

    uint16_t kc = rd_u16(data + pos);
    pos += 2;
    if (kc > NXS_MAX_KEYS) return NXS_ERR_OUT_OF_BOUNDS;
    if (pos + kc > size) return NXS_ERR_OUT_OF_BOUNDS;

    memcpy(r->key_sigils, data + pos, kc);
    pos += kc;
    r->key_count = (int)kc;

    char *pool = r->_pool;
    size_t pool_used = 0;
    for (int i = 0; i < r->key_count; i++) {
        const uint8_t *start = data + pos;
        while (pos < size && data[pos] != 0) pos++;
        if (pos >= size) return NXS_ERR_OUT_OF_BOUNDS;
        size_t len = (size_t)(data + pos - start);
        if (pool_used + len + 1 > sizeof(r->_pool)) return NXS_ERR_OUT_OF_BOUNDS;
        memcpy(pool + pool_used, start, len);
        pool[pool_used + len] = '\0';
        r->keys[i] = pool + pool_used;
        pool_used += len + 1;
        pos++;
    }
    if (pos % 8 != 0) pos = nxs_align_to(pos, 8);

    memset(out->field_widths, 0, sizeof(out->field_widths));
    if (flags & NXS_FLAG_NARROW_CELLS) {
        if (pos + (size_t)kc > size) return NXS_ERR_OUT_OF_BOUNDS;
        memcpy(out->field_widths, data + pos, kc);
        pos += kc;
    }

    memset(out->field_attrs, 0, sizeof(out->field_attrs));
    if (flags & NXS_FLAG_V13_COMPACT_MASK) {
        if (pos + (size_t)kc > size) return NXS_ERR_OUT_OF_BOUNDS;
        memcpy(out->field_attrs, data + pos, kc);
        pos += kc;
    }

    if (pos + 2 <= size) {
        uint16_t value_count = rd_u16(data + pos);
        pos += 2;
        out->value_pool_count = (int)value_count;
        if (value_count > 0) {
            out->value_pool = calloc((size_t)value_count, sizeof(char *));
            if (!out->value_pool) return NXS_ERR_ALLOC;
            for (int i = 0; i < (int)value_count; i++) {
                const uint8_t *start = data + pos;
                while (pos < size && data[pos] != 0) pos++;
                if (pos >= size) {
                    nxs_ext_schema_free(out);
                    return NXS_ERR_OUT_OF_BOUNDS;
                }
                size_t len = (size_t)(data + pos - start);
                char *s = malloc(len + 1);
                if (!s) {
                    nxs_ext_schema_free(out);
                    return NXS_ERR_ALLOC;
                }
                memcpy(s, start, len);
                s[len] = '\0';
                out->value_pool[i] = s;
                pos++;
            }
            if (pos % 8 != 0) pos = nxs_align_to(pos, 8);
        }
    }

    if (pos > size) return NXS_ERR_OUT_OF_BOUNDS;
    out->schema_end = pos;
    *end_out = pos;
    return NXS_OK;
}

int nxs_parse_delta_tail_layout(const uint8_t *data, size_t size,
                                       size_t tail_ptr, nxs_delta_tail_t *out) {
    if (!data || !out || tail_ptr + 12 > size) return NXS_ERR_OUT_OF_BOUNDS;
    out->tail_ptr = tail_ptr;
    out->record_count = rd_u32(data + tail_ptr);
    out->block_size = rd_u32(data + tail_ptr + 4);
    uint16_t ti_flags = rd_u16(data + tail_ptr + 8);
    uint16_t anchor_count = rd_u16(data + tail_ptr + 10);
    out->anchors_off = tail_ptr + nxs_align_to(12, 8);
    out->deltas_off = out->anchors_off + (size_t)anchor_count * 8u;
    out->single_key_id = (ti_flags & 0x0001u) != 0;
    if (out->deltas_off + (size_t)out->record_count * 4u > size)
        return NXS_ERR_OUT_OF_BOUNDS;
    return NXS_OK;
}

int64_t nxs_delta_record_offset(const uint8_t *data, size_t size,
                                const nxs_delta_tail_t *layout, uint32_t index) {
    if (!data || !layout || index >= layout->record_count) return -1;
    uint32_t a = layout->block_size;
    if (a == 0) a = 1;
    uint32_t anchor_idx = index / a;
    size_t anchor_off = layout->anchors_off + (size_t)anchor_idx * 8u;
    if (anchor_off + 8 > size) return -1;
    uint64_t anchor = rd_u64(data + anchor_off);
    size_t delta_off = layout->deltas_off + (size_t)index * 4u;
    if (delta_off + 4 > size) return -1;
    uint32_t delta = rd_u32(data + delta_off);
    uint64_t abs = anchor + (uint64_t)delta;
    if (abs > (uint64_t)SIZE_MAX) return -1;
    return (int64_t)abs;
}

int64_t nxs_decode_int_cell(const uint8_t *data, size_t size, size_t offset,
                            uint8_t width) {
    if (!data) return 0;
    switch (width) {
    case 1:
        if (offset >= size) return 0;
        return (int64_t)(int8_t)data[offset];
    case 2: {
        if (offset + 2 > size) return 0;
        return (int64_t)(int16_t)rd_u16(data + offset);
    }
    case 4: {
        if (offset + 4 > size) return 0;
        return (int64_t)(int32_t)rd_u32(data + offset);
    }
    case 8: {
        if (offset + 8 > size) return 0;
        int64_t v;
        memcpy(&v, data + offset, 8);
        return v;
    }
    default:
        return 0;
    }
}

double nxs_decode_f64_cell(const uint8_t *data, size_t size, size_t offset,
                           uint8_t width) {
    if (!data) return 0.0;
    if (width == 4) {
        if (offset + 4 > size) return 0.0;
        return (double)rd_f32(data + offset);
    }
    if (width == 8) {
        if (offset + 8 > size) return 0.0;
        double v;
        memcpy(&v, data + offset, 8);
        return v;
    }
    return 0.0;
}

static size_t advance_dense_past_cell(const uint8_t *data, size_t size,
                                      size_t body_base, size_t pos, int fi,
                                      const struct nxs_reader_t *r,
                                      const nxs_ext_schema_t *ext,
                                      const nxs_cell_plan_t *plan) {
    uint8_t sig = r->key_sigils[fi];
    size_t w = plan->narrow ? cell_width(r, ext, fi) : 8;
    if (plan->packed_bools && bool_slot_index(plan, fi) >= 0)
        return advance_past_bool_word(pos, plan);
    if ((sig == NXS_SIGIL_INT || sig == NXS_SIGIL_FLOAT || sig == NXS_SIGIL_BOOL) &&
        (!plan->packed_bools || sig != NXS_SIGIL_BOOL))
        return nxs_align_to(pos, w) + w;
    if (sig == NXS_SIGIL_STR || sig == 0x3cu) {
        if (is_promoted(ext, fi)) return nxs_align_to(pos, 2) + 2;
        int prefix = str_len_prefix(ext, fi);
        size_t abs = body_base + pos;
        size_t len = read_str_cell_len(data, size, abs, prefix);
        if (len == (size_t)-1) return pos;
        return advance_past_str_cell(pos, len, prefix);
    }
    if (sig == NXS_SIGIL_KEYWORD) return nxs_align_to(pos, 2) + 2;
    return nxs_align_to(pos, 8) + 8;
}

static int64_t dense_field_offset(const uint8_t *data, size_t size,
                                  size_t obj_offset, int slot,
                                  const struct nxs_reader_t *r,
                                  const nxs_ext_schema_t *ext,
                                  const nxs_cell_plan_t *plan) {
    size_t body_base = obj_offset + 9;
    if (plan->has_dense_fixed) {
        if (slot >= 0 && slot < r->key_count &&
            plan->dense_fixed_body_offsets[slot] >= 0) {
            return (int64_t)(body_base + (size_t)plan->dense_fixed_body_offsets[slot]);
        }
        if (plan->dense_var_body_start >= 0) {
            size_t pos = (size_t)plan->dense_var_body_start;
            for (int wi = 0; wi < plan->wire_order_count; wi++) {
                int fi = plan->wire_order[wi];
                if (!is_var_sigil(r->key_sigils[fi])) continue;
                if (fi == slot) return (int64_t)(body_base + pos);
                pos = advance_dense_past_cell(data, size, body_base, pos, fi, r, ext, plan);
            }
            return -1;
        }
    }
    size_t pos = 0;
    for (int wi = 0; wi < plan->wire_order_count; wi++) {
        int fi = plan->wire_order[wi];
        if (plan->packed_bools && bool_slot_index(plan, fi) >= 0) {
            if (bool_slot_index(plan, slot) >= 0 && fi == plan->first_bool) {
                int bit_idx = bool_slot_index(plan, slot);
                if (bit_idx < 0) return -1;
                size_t bw = (size_t)plan_bool_word_bytes(plan);
                return (int64_t)(body_base + nxs_align_to(pos, bw) + (size_t)(bit_idx / 8));
            }
            if (fi == plan->first_bool) pos = advance_past_bool_word(pos, plan);
            continue;
        }
        uint8_t sig = r->key_sigils[fi];
        size_t w = plan->narrow ? cell_width(r, ext, fi) : 8;
        if (fi == slot) {
            size_t off = (is_promoted(ext, fi) || sig == NXS_SIGIL_KEYWORD)
                             ? nxs_align_to(pos, 2)
                             : is_var_sigil(sig) ? pos : nxs_align_to(pos, w);
            return (int64_t)(body_base + off);
        }
        pos = advance_dense_past_cell(data, size, body_base, pos, fi, r, ext, plan);
    }
    return -1;
}

static int sparse_offset_table_slots(const int *present_slots, int present_count,
                                     const nxs_cell_plan_t *plan,
                                     int *out, int *out_count) {
    *out_count = 0;
    int bool_word_added = 0;
    for (int i = 0; i < present_count; i++) {
        int fi = present_slots[i];
        if (bool_slot_index(plan, fi) >= 0) {
            if (!bool_word_added) {
                out[(*out_count)++] = fi;
                bool_word_added = 1;
            }
        } else {
            out[(*out_count)++] = fi;
        }
    }
    return 1;
}

static int64_t resolve_slot_v13_sparse(const uint8_t *data, size_t size,
                                         size_t obj_offset, int slot,
                                         const struct nxs_reader_t *r,
                                         const nxs_cell_plan_t *plan) {
    size_t p = obj_offset + 9;
    int cur = 0;
    int slot_present = 0;
    int present_slots[NXS_MAX_KEYS];
    int present_count = 0;
    int field_count = r->key_count;
    int done = 0;

    while (!done) {
        if (p >= size) return -1;
        uint8_t b = data[p++];
        uint8_t bits = b & 0x7fu;
        for (int bit = 0; bit < 7; bit++) {
            if ((bits >> bit) & 1) {
                present_slots[present_count++] = cur;
                if (cur == slot) slot_present = 1;
            }
            cur++;
            if (cur >= field_count) {
                done = 1;
                break;
            }
        }
        if ((b & 0x80) == 0) break;
    }
    if (!slot_present) return -1;

    int ot_slots[NXS_MAX_KEYS];
    int ot_count = 0;
    sparse_offset_table_slots(present_slots, present_count, plan, ot_slots, &ot_count);
    size_t table_base = p;

    if (plan->packed_bools && bool_slot_index(plan, slot) >= 0) {
        int bool_table_fi = -1;
        for (int i = 0; i < ot_count; i++) {
            if (bool_slot_index(plan, ot_slots[i]) >= 0) {
                bool_table_fi = ot_slots[i];
                break;
            }
        }
        if (bool_table_fi < 0) return -1;
        int table_idx = -1;
        for (int i = 0; i < ot_count; i++) {
            if (ot_slots[i] == bool_table_fi) {
                table_idx = i;
                break;
            }
        }
        if (table_idx < 0 || table_base + (size_t)table_idx * 2u + 2u > size) return -1;
        uint16_t rel = rd_u16(data + table_base + (size_t)table_idx * 2u);
        size_t base = obj_offset + rel;
        int bit_in_word = bool_slot_index(plan, slot);
        if (bit_in_word < 0) return -1;
        return (int64_t)(base + (size_t)(bit_in_word / 8));
    }

    int table_idx = -1;
    for (int i = 0; i < ot_count; i++) {
        if (ot_slots[i] == slot) {
            table_idx = i;
            break;
        }
    }
    if (table_idx < 0 || table_base + (size_t)table_idx * 2u + 2u > size) return -1;
    uint16_t rel = rd_u16(data + table_base + (size_t)table_idx * 2u);
    return (int64_t)(obj_offset + rel);
}

static int64_t resolve_slot_v12(const uint8_t *data, size_t size,
                                size_t obj_offset, int slot) {
    size_t p = obj_offset + 8;
    int cur = 0, table_idx = 0;
    uint8_t b = 0;
    int found = 0;
    for (;;) {
        if (p >= size) return -1;
        b = data[p++];
        uint8_t bits = b & 0x7fu;
        for (int bit = 0; bit < 7; bit++) {
            if (cur == slot) {
                if (((bits >> bit) & 1) == 0) return -1;
                found = 1;
            } else if (cur < slot && ((bits >> bit) & 1)) {
                table_idx++;
            }
            cur++;
        }
        if (found && (b & 0x80) == 0) break;
        if (cur > slot && found) break;
        if ((b & 0x80) == 0) return -1;
    }
    while (b & 0x80) {
        if (p >= size) return -1;
        b = data[p++];
    }
    if (p + (size_t)table_idx * 2u + 2u > size) return -1;
    uint16_t rel = rd_u16(data + p + (size_t)table_idx * 2u);
    return (int64_t)(obj_offset + rel);
}

int64_t nxs_resolve_field_offset(const uint8_t *data, size_t size,
                                 size_t obj_offset, int slot,
                                 const struct nxs_reader_t *r,
                                 const nxs_ext_schema_t *ext,
                                 const nxs_cell_plan_t *plan) {
    if (slot < 0 || slot >= r->key_count) return -1;
    if (plan->dense_allowed) {
        if (obj_offset + 9 > size) return -1;
        uint8_t hdr = data[obj_offset + 8];
        if (hdr & NXS_RECORD_HDR_DENSE)
            return dense_field_offset(data, size, obj_offset, slot, r, ext, plan);
        return resolve_slot_v13_sparse(data, size, obj_offset, slot, r, plan);
    }
    return resolve_slot_v12(data, size, obj_offset, slot);
}

int nxs_read_packed_bool(const uint8_t *data, size_t size, size_t obj_offset,
                         int slot, const struct nxs_reader_t *r,
                         const nxs_ext_schema_t *ext, const nxs_cell_plan_t *plan) {
    int64_t off = nxs_resolve_field_offset(data, size, obj_offset, slot, r, ext, plan);
    if (off < 0 || (size_t)off >= size) return 0;
    int bit_pos = bool_slot_index(plan, slot);
    if (bit_pos < 0) return 0;
    uint8_t b = data[(size_t)off];
    return ((b >> (bit_pos % 8)) & 1) == 1;
}

static const char *value_pool_at(const nxs_ext_schema_t *ext, uint16_t idx) {
    if (!ext->value_pool || (int)idx >= ext->value_pool_count) return NULL;
    return ext->value_pool[idx];
}

int nxs_materialise_str_at(const uint8_t *data, size_t size, size_t off,
                                 int slot, const struct nxs_reader_t *r,
                                 const nxs_ext_schema_t *ext,
                                 char *buf, size_t buf_len) {
    if (!buf || buf_len == 0) return NXS_ERR_OUT_OF_BOUNDS;
    uint8_t sig = r->key_sigils[slot];
    if (is_promoted(ext, slot) || sig == NXS_SIGIL_KEYWORD) {
        if (off > size || size - off < 2) return NXS_ERR_OUT_OF_BOUNDS;
        uint16_t idx = rd_u16(data + off);
        const char *s = value_pool_at(ext, idx);
        if (!s) return NXS_ERR_OUT_OF_BOUNDS;
        size_t len = strlen(s);
        size_t copy = len < buf_len - 1 ? len : buf_len - 1;
        memcpy(buf, s, copy);
        buf[copy] = '\0';
        return NXS_OK;
    }
    int prefix = str_len_prefix(ext, slot);
    size_t len = read_str_cell_len(data, size, off, prefix);
    if (len == (size_t)-1 || (size_t)prefix > size - off ||
        len > size - off - (size_t)prefix)
        return NXS_ERR_OUT_OF_BOUNDS;
    size_t copy = len < buf_len - 1 ? len : buf_len - 1;
    memcpy(buf, data + off + (size_t)prefix, copy);
    buf[copy] = '\0';
    return NXS_OK;
}
