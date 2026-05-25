---
room: prefetch
subdomain: c
source_paths: c/
see_also: ["c/reader.md", "go/prefetch_col.md"]
hot_paths: nxs_prefetch.c, nxs_column_prefetch.c
architectural_health: normal
security_tier: normal
---

# c/ — Adaptive Prefetch & Profiling

Subdomain: c/
Source paths: c/

## TASK → LOAD

| Task | Load |
|------|------|
| Enable adaptive row prefetch on open | prefetch.md |
| Warm a record viewport before scan | prefetch.md |
| Prefetch one columnar field buffer | prefetch.md |
| Inspect cache stats / strategy upgrades | prefetch.md |
| Run prefetch unit tests | prefetch.md |
| Profile selective-read step costs | prefetch.md |

---

# nxs_column_prefetch.c

DOES: Warms a single columnar field sector by touching 4 KiB pages once; marks `col_warmed[slot]` and increments `col_fetches` on the reader.
SYMBOLS:
- nxs_prefetch_column(r, field) nxs_err_t
- col_sector_end(r, slot, out_off, out_len) int
- touch_column_pages(sector, len) void
DEPENDS: nxs_column_prefetch.h, nxs.h
PATTERNS: page-touch warmup, idempotent column warm flag
USE WHEN: Columnar `.nxb` with `NXS_LAYOUT_COLUMNAR`; no-op on row layout (returns `NXS_ERR_UNSUPPORTED`).

---

# nxs_column_prefetch.h

DOES: Public C API declaring `nxs_prefetch_column` for columnar layout only (Adaptive-prefetch-spec §7.4).
SYMBOLS:
- nxs_prefetch_column(nxs_reader_t *r, const char *field) nxs_err_t
DEPENDS: nxs.h
USE WHEN: Including column prefetch alongside the core reader header.

---

# nxs_prefetch.c

DOES: Phase-2 adaptive prefetch engine: access-pattern detector, LRU page cache with byte cap, coalesced range fetch, lazy/adaptive/eager strategies, optional pthread eager background worker, viewport pin/unpin, and cache statistics export.
SYMBOLS:
- nxs_open_options_init(opts) void
- nxs_initial_strategy(hint, file_size) nxs_prefetch_strategy_t
- nxs_pattern_detector_init/observe/pattern/predict_next(...)
- nxs_prefetch_init(r, opts) nxs_err_t
- nxs_prefetch_destroy(r) void
- nxs_prefetch_on_access(r, index) void
- nxs_pause_prefetch / nxs_resume_prefetch(r) void
- nxs_prefetch_set_cache_limit(r, max_bytes) void
- nxs_warmup(r) void
- nxs_prefetch_viewport(r, start_index, end_index) nxs_err_t
- nxs_cache_stats(r, stats) void
- nxs_coalesce_page_indices(indices, count, gap_pages, page_size, out, out_cap) size_t
TYPE: nxs_open_options_t, nxs_cache_stats_t, nxs_page_range_t, nxs_access_pattern_detector_t
DEPENDS: nxs_prefetch.h, nxs.h, pthread.h
PATTERNS: LRU eviction, page coalescing, adaptive-to-eager upgrade, pthread background prefetch
USE WHEN: Row-layout `.nxb` with repeated sequential or viewport scans; pair with `nxs_open_ex` from header.

---

# nxs_prefetch.h

DOES: Declares prefetch constants, hint/strategy/pattern enums, open options, cache stats, pattern detector, and all prefetch lifecycle APIs.
SYMBOLS:
- nxs_open_ex(r, data, size, opts) nxs_err_t
- nxs_prefetch_init / destroy / on_access / viewport / cache_stats (+ helpers listed in nxs_prefetch.c)
TYPE: nxs_access_hint_t, nxs_prefetch_strategy_t, nxs_access_pattern_t, nxs_open_options_t, nxs_cache_stats_t
DEPENDS: nxs.h
USE WHEN: Any translation unit enabling adaptive prefetch on `nxs_reader_t`.

---

# profile_selective.c

DOES: Workload-A selective-read profiler: times tail lookup, object staging, slot resolution, and per-field cell reads over five keys for median latency reporting.
SYMBOLS:
- main(int argc, char **argv) int
- median_ns(v, n) int64_t
TYPE: sample_t { tail_ns, stage_ns, slot_ns, resolve_ns, cell_ns, full_ns, full_cached_slots_ns }
DEPENDS: nxs.h
PATTERNS: micro-step profiler, median aggregation
USE WHEN: Diagnosing selective-read hotspots on a fixture; build via `make profile-selective`.

---

# test_prefetch.c

DOES: Unit tests for coalescing, pattern detector, LRU eviction, viewport prefetch, pause/resume, eager hint, and column prefetch integration using in-memory writers.
SYMBOLS:
- main(int argc, char **argv) int
- build_records(n, out_size) uint8_t*
- build_compact_records(n, out_size) uint8_t*
DEPENDS: nxs.h, nxs_prefetch.h, nxs_writer.h
PATTERNS: CHECK macro harness, synthetic NXB builder
USE WHEN: `make test-prefetch && ./test_prefetch`.
