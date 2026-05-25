---
room: prefetch
subdomain: py
source_paths: py/
see_also: ["py/reader.md", "c/prefetch.md"]
hot_paths: nxs.py, pattern.py
architectural_health: normal
security_tier: normal
---

# py/ — Adaptive Prefetch

Subdomain: py/
Source paths: py/

## TASK → LOAD

| Task | Load |
|------|------|
| Enable prefetch on NxsReader | prefetch.md |
| Observe access patterns | prefetch.md |
| Run prefetch unit tests | prefetch.md |

---

# nxs.py

DOES: Pure-Python reader plus embedded `PrefetchEngine`, `PageCache`, `InFlightMap`, viewport prefetch, pause/resume, and query predicates; mirrors Go/JS prefetch semantics.
SYMBOLS:
- NxsReader.prefetch_viewport(start, end) None
- NxsReader.warmup() / pause_prefetch() / resume_prefetch() / cache_stats() dict
- PrefetchEngine.on_access(index) None
- initial_strategy(hint, file_size) str
- coalesce_page_indices(...) list
- Types: PageCache, InFlightMap, PrefetchEngine
DEPENDS: py/pattern.py
PATTERNS: LRU page cache, coalesced fetch, adaptive strategy
USE WHEN: Row-layout Python reads with IO-backed buffers; see py/reader.md for field accessors.

---

# pattern.py

DOES: `AccessPatternDetector` with sequential/random thresholds matching the cross-language prefetch spec.
SYMBOLS:
- AccessPatternDetector.observe(index) None
- AccessPatternDetector.pattern() str
- AccessPatternDetector.predict_next(depth, record_count) list[int]
DEPENDS: (none)
USE WHEN: Testing or extending prefetch classification in Python.

---

# test_prefetch.py

DOES: CLI test suite for coalescing, LRU, in-flight dedup, pattern detector, pause/resume, eager hint, and sequential upgrade using synthetic writers.
SYMBOLS:
- main() int
- build_records(n) bytes
- build_compact_records(n) bytes
- _test_*() helpers
DEPENDS: nxs, nxs_writer, pattern
PATTERNS: assertion harness
USE WHEN: `python3 test_prefetch.py`.
