---
room: prefetch_col
subdomain: go
source_paths: go/
see_also: ["go/reader.md", "c/prefetch.md"]
hot_paths: prefetch.go, col.go, column_prefetch.go
architectural_health: normal
security_tier: normal
---

# go/ — Prefetch, Columnar & PAX

Subdomain: go/
Source paths: go/

## TASK → LOAD

| Task | Load |
|------|------|
| Configure reader prefetch options | prefetch_col.md |
| Prefetch a record viewport | prefetch_col.md |
| Warm one columnar column buffer | prefetch_col.md |
| Sum F64 on columnar dense layout | prefetch_col.md |
| Stream sealed PAX pages incrementally | prefetch_col.md |
| Run prefetch/col conformance tests | prefetch_col.md |

---

# col.go

DOES: Parses columnar and PAX footers, exposes `LayoutKind`, dense column reducers (`ColSumF64`, `ColBuffer`, `ColVarBuffer`), and PAX per-page field sectors.
SYMBOLS:
- (r *Reader) parseLayoutTail() error
- (r *Reader) PrefetchColumn(key string) error
- (r *Reader) ColSumF64(key string) float64
- (r *Reader) ColBuffer(key string) ([]byte, bool)
- (r *Reader) ColVarBuffer(key string) (ColVarBuffer, error)
- (r *Reader) LayoutKind() Layout
- Types: Layout, ColVarBuffer
DEPENDS: go/nxs.go
PATTERNS: columnar dense scan, PAX page index, null bitmap
USE WHEN: `.nxb` with `FLAG_COLUMNAR` or PAX footer; use `SumF64` in reader.md for row layout.

---

# column_prefetch.go

DOES: Columnar column-buffer warmup: resolves slot, fetches sector once via optional `FetchRange`, sets `colWarmed[slot]`.
SYMBOLS:
- (r *Reader) PrefetchColumn(key string) error
- (r *Reader) initColumnPrefetch(cfg readerConfig)
- (r *Reader) columnSector(slot int) ([]byte, error)
DEPENDS: go/col.go, go/nxs.go
PATTERNS: idempotent column warm
USE WHEN: Before hot `ColSumF64` loops on large columnar files.

---

# column_prefetch_test.go

DOES: Tests single-fetch column prefetch and readable records after `PrefetchColumn`.
SYMBOLS:
- TestPrefetchColumnSingleFetch(t *testing.T)
DEPENDS: testing, go/nxs.go, go/writer.go
USE WHEN: `go test -run PrefetchColumn`.

---

# col_test.go

DOES: Conformance and benchmark tests for columnar dense, PAX dense/strings, invalid flags, and `PaxStreamReader` incremental poll.
SYMBOLS:
- TestColumnarDenseConformance(t *testing.T)
- TestPAXDenseConformance(t *testing.T)
- TestPAXStreamIncremental(t *testing.T)
- BenchmarkColumnarSumF64_* / BenchmarkColumnarColSumF64_*(b *testing.B)
DEPENDS: testing, go/col.go, go/pax_stream.go
USE WHEN: Validating columnar/PAX parity against conformance vectors.

---

# pattern.go

DOES: Access-pattern detector counting sequential runs vs random jumps; classifies pattern and predicts next record indices for speculative prefetch.
SYMBOLS:
- NewAccessPatternDetector() *AccessPatternDetector
- (d *AccessPatternDetector) Observe(index int)
- (d *AccessPatternDetector) Pattern() AccessPattern
- (d *AccessPatternDetector) PredictNext(depth, recordCount int) []int
DEPENDS: (none)
PATTERNS: sliding-window access history
USE WHEN: Understanding adaptive prefetch decisions; used by prefetch.go.

---

# pax_stream.go

DOES: Incremental PAX stream reader: polls complete pages (`Poll`), tracks sealed tail, exposes `ColSumF64` over buffered pages without full-file mmap assumptions.
SYMBOLS:
- OpenPaxStream(data []byte) (*PaxStreamReader, error)
- (sr *PaxStreamReader) Poll() uint32
- (sr *PaxStreamReader) ColSumF64(key string) float64
- PaxCompletePageAt(data, off, fieldCount) int
TYPE: PaxStreamReader
DEPENDS: go/col.go
PATTERNS: incremental page ingest, sealed-tail detection
USE WHEN: Streaming WAL or growing PAX files; not for static mmap-only readers.

---

# prefetch.go

DOES: Go adaptive prefetch engine: `OpenOptions`, page LRU cache, in-flight dedup, coalesced fetch, eager goroutine background load, viewport prefetch, and `Reader` integration via functional options.
SYMBOLS:
- DefaultOpenOptions() OpenOptions
- InitialStrategy(hint AccessHint, fileSize int) PrefetchStrategy
- CoalescePageIndices(indices, gapPages, pageSize) []PageRange
- (r *Reader) PrefetchViewport(ctx, startIndex, endIndex) error
- (r *Reader) Warmup() / PausePrefetch() / ResumePrefetch() / CacheStats()
- newPrefetchEngine(...) *prefetchEngine
TYPE: OpenOptions, CacheStats, PageRange, prefetchEngine
DEPENDS: go/pattern.go, go/nxs.go
PATTERNS: LRU cache, coalesced IO, adaptive-to-eager upgrade, context-aware viewport
USE WHEN: Row-layout scans with non-trivial IO latency; pass `WithOpenOptions` to `NewReader`.

---

# prefetch_test.go

DOES: Tests coalescing, viewport fetch counts, LRU eviction, pattern classification, sequential upgrade, pause stopping speculative prefetch, and hint-full eager mode.
SYMBOLS:
- TestCoalesce(t *testing.T)
- TestPrefetchViewportCoalesce(t *testing.T)
- TestLRUEviction(t *testing.T)
- TestPatternSequential(t *testing.T)
- TestSequentialUpgrade(t *testing.T)
- (+6 more)
DEPENDS: testing, go/prefetch.go, go/writer.go
USE WHEN: `go test -run Prefetch`.

---

# producer_roundtrip_test.go

DOES: Validates Go producer schema sigils, stream writer manifest rules, and Rust conformance roundtrip via subprocess.
SYMBOLS:
- TestGoProducerMatchesRustConformance(t *testing.T)
- TestStreamWriterTypeManifestSigils(t *testing.T)
DEPENDS: testing, go/writer.go, os/exec
USE WHEN: Cross-language producer compatibility checks.
