// NXS Writer — direct-to-buffer .nxb emitter for C99.
//
// Usage:
//   nxs_writer_t w;
//   const char *keys[] = {"id", "username", "score", "active"};
//   nxs_writer_init(&w, keys, 4, 64 * 1024);
//
//   nxs_writer_begin_object(&w);
//   nxs_write_i64(&w, 0, 42);
//   nxs_write_str(&w, 1, "alice", 5);
//   nxs_write_f64(&w, 2, 9.5);
//   nxs_write_bool(&w, 3, 1);
//   nxs_writer_end_object(&w);
//
//   const uint8_t *bytes = nxs_writer_finish(&w);
//   size_t         size  = w.out_size;
//   // ... send or write bytes ...
//   nxs_writer_free(&w);
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXS_WRITER_MAX_KEYS   256
#define NXS_WRITER_MAX_DEPTH  8    /* max nested object depth */

// ── Writer ────────────────────────────────────────────────────────────────────

typedef struct {
    // Schema (set at init, immutable)
    const char *keys[NXS_WRITER_MAX_KEYS];
    int         key_count;
    int         bitmask_bytes;

    // Output buffer (growable)
    uint8_t    *buf;
    size_t      buf_len;   // allocated
    size_t      buf_pos;   // bytes written to buf (data sector)

    // Record offsets for the tail-index
    uint32_t   *record_offsets;
    int         record_count;
    int         record_cap;

    // Frame stack for nested begin_object / end_object
    struct {
        size_t   start;
        uint8_t  bitmask[NXS_WRITER_MAX_KEYS / 7 + 1];
        int      bitmask_bytes;
        uint16_t offset_table[NXS_WRITER_MAX_KEYS];
        int      slot_order[NXS_WRITER_MAX_KEYS]; // slot at each offset_table entry
        int      present_count;
        int      last_slot;
        int      needs_sort;
    } frames[NXS_WRITER_MAX_DEPTH];
    int frame_depth;

    // Assembled output (set by nxs_writer_finish)
    uint8_t *out;
    size_t   out_size;
} nxs_writer_t;

// Initialise a writer.  `keys` must live at least as long as the writer.
// `initial_cap` is the initial data-sector buffer capacity (bytes).
int  nxs_writer_init(nxs_writer_t *w, const char **keys, int key_count,
                     size_t initial_cap);

// Free all heap allocations.  Safe to call even if init failed.
void nxs_writer_free(nxs_writer_t *w);

// Object lifetime
int nxs_writer_begin_object(nxs_writer_t *w);
int nxs_writer_end_object(nxs_writer_t *w);

// Typed write methods (slot = 0-based index into keys[])
int nxs_write_i64  (nxs_writer_t *w, int slot, int64_t v);
int nxs_write_f64  (nxs_writer_t *w, int slot, double  v);
int nxs_write_bool (nxs_writer_t *w, int slot, int     v);
int nxs_write_time (nxs_writer_t *w, int slot, int64_t unix_ns);
int nxs_write_null (nxs_writer_t *w, int slot);
int nxs_write_str  (nxs_writer_t *w, int slot, const char *s, uint32_t len);
int nxs_write_bytes(nxs_writer_t *w, int slot, const uint8_t *data, uint32_t len);

// Finish: assemble preamble + schema + data + tail-index into w->out / w->out_size.
// Returns 0 on success.  Caller must call nxs_writer_free() when done.
int nxs_writer_finish(nxs_writer_t *w);

// Reset: clear all records and restart the data sector, keeping the schema and
// the allocated buffer.  Cheaper than free + init for reuse across many spans.
void nxs_writer_reset(nxs_writer_t *w);

#ifdef __cplusplus
}
#endif
