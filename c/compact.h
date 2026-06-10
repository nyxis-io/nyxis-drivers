// NXS v1.3 compact row decode — mirrors nyxis/rust/src/compact.rs (read path).
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifndef NXS_MAX_KEYS
#define NXS_MAX_KEYS 256
#endif

struct nxs_reader_t;

#ifdef __cplusplus
extern "C" {
#endif

#define NXS_FLAG_DENSE_FRAMES       0x0010u
#define NXS_FLAG_PACKED_BOOLS       0x0020u
#define NXS_FLAG_NARROW_CELLS       0x0040u
#define NXS_FLAG_DELTA_TAIL         0x0080u
#define NXS_FLAG_DENSE_WIRE_REORDER 0x0100u

#define NXS_RECORD_HDR_DENSE        0x01u
#define NXS_FIELD_ATTR_PROMOTED     0x01u
#define NXS_FIELD_ATTR_U16_LEN      0x02u

#define NXS_SIGIL_INT               0x3du
#define NXS_SIGIL_FLOAT             0x7eu
#define NXS_SIGIL_BOOL              0x3fu
#define NXS_SIGIL_KEYWORD           0x24u
#define NXS_SIGIL_STR               0x22u

typedef struct {
    int            value_pool_count;
    char         **value_pool;
    uint8_t        field_widths[NXS_MAX_KEYS];
    uint8_t        field_attrs[NXS_MAX_KEYS];
    size_t         schema_end;
} nxs_ext_schema_t;

typedef struct {
    int            bool_slots[NXS_MAX_KEYS];
    int            bool_slot_count;
    int            first_bool;
    int            wire_order[NXS_MAX_KEYS];
    int            wire_order_count;
    int32_t        dense_fixed_body_offsets[NXS_MAX_KEYS];
    int            dense_var_body_start;
    uint8_t        packed_bools;
    uint8_t        narrow;
    uint8_t        dense_allowed;
    uint8_t        dense_wire_reorder;
    uint8_t        has_dense_fixed;
} nxs_cell_plan_t;

typedef struct {
    size_t         tail_ptr;
    uint32_t       record_count;
    uint32_t       block_size;
    uint8_t        single_key_id;
    size_t         anchors_off;
    size_t         deltas_off;
} nxs_delta_tail_t;

size_t nxs_align_to(size_t pos, size_t align);

int nxs_parse_extended_schema(const uint8_t *data, size_t size, size_t pos,
                                            uint16_t flags, struct nxs_reader_t *r,
                                            nxs_ext_schema_t *out, size_t *end_out);

void nxs_cell_plan_build(const struct nxs_reader_t *r, const nxs_ext_schema_t *ext,
                         uint16_t flags, nxs_cell_plan_t *plan);

int nxs_parse_delta_tail_layout(const uint8_t *data, size_t size,
                                              size_t tail_ptr, nxs_delta_tail_t *out);

int64_t nxs_delta_record_offset(const uint8_t *data, size_t size,
                                const nxs_delta_tail_t *layout, uint32_t index);

int64_t nxs_resolve_field_offset(const uint8_t *data, size_t size,
                                 size_t obj_offset, int slot,
                                 const struct nxs_reader_t *r,
                                 const nxs_ext_schema_t *ext,
                                 const nxs_cell_plan_t *plan);

int64_t nxs_decode_int_cell(const uint8_t *data, size_t size, size_t offset,
                            uint8_t width);
double nxs_decode_f64_cell(const uint8_t *data, size_t size, size_t offset,
                           uint8_t width);

uint8_t nxs_field_cell_width(const struct nxs_reader_t *r, const nxs_ext_schema_t *ext,
                             int slot);

int nxs_read_packed_bool(const uint8_t *data, size_t size, size_t obj_offset,
                         int slot, const struct nxs_reader_t *r,
                         const nxs_ext_schema_t *ext, const nxs_cell_plan_t *plan);

int nxs_materialise_str_at(const uint8_t *data, size_t size, size_t off,
                                         int slot, const struct nxs_reader_t *r,
                                         const nxs_ext_schema_t *ext,
                                         char *buf, size_t buf_len);

void nxs_ext_schema_free(nxs_ext_schema_t *ext);

#ifdef __cplusplus
}
#endif
