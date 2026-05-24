#ifndef NXS_COLUMN_PREFETCH_H
#define NXS_COLUMN_PREFETCH_H

#include "nxs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Prefetch one column buffer (columnar layout only; Adaptive-prefetch-spec §7.4). */
nxs_err_t nxs_prefetch_column(nxs_reader_t *r, const char *field);

#ifdef __cplusplus
}
#endif

#endif /* NXS_COLUMN_PREFETCH_H */
