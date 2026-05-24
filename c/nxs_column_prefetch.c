#include "nxs_column_prefetch.h"

#include <string.h>

#define NXS_COL_PAGE 4096u

static int col_sector_end(const nxs_reader_t *r, int slot, size_t *out_off, size_t *out_len) {
    if (slot < 0 || slot >= r->key_count) return 0;
    uint64_t off = r->col_buf_off[slot];
    uint64_t len = r->col_buf_len[slot];
    if (off > r->size) return 0;
    if (len > r->size - off) return 0;
    *out_off = (size_t)off;
    *out_len = (size_t)len;
    return 1;
}

static void touch_column_pages(const uint8_t *sector, size_t len) {
    for (size_t i = 0; i < len; i += NXS_COL_PAGE) {
        (void)sector[i];
    }
}

nxs_err_t nxs_prefetch_column(nxs_reader_t *r, const char *field) {
    if (!r || !field) return NXS_ERR_OUT_OF_BOUNDS;
    if (r->layout != NXS_LAYOUT_COLUMNAR) return NXS_ERR_UNSUPPORTED;
    int slot = nxs_slot(r, field);
    if (slot < 0) return NXS_ERR_KEY_NOT_FOUND;
    if (r->col_warmed[slot]) return NXS_OK;
    size_t off, len;
    if (!col_sector_end(r, slot, &off, &len)) return NXS_ERR_OUT_OF_BOUNDS;
    r->col_warmed[slot] = 1;
    r->col_fetches++;
    touch_column_pages(r->data + off, len);
    return NXS_OK;
}
