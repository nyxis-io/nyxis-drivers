---
room: prefetch_compile
subdomain: js
source_paths: js/
see_also: ["js/reader.md", "devtools/extension.md"]
hot_paths: prefetch.js, nxs.js, nxs_decode.js
architectural_health: normal
security_tier: normal
---

# js/ — Prefetch, WASM Compile & Decode

Subdomain: js/
Source paths: js/

## TASK → LOAD

| Task | Load |
|------|------|
| Wire adaptive prefetch into NxsReader | prefetch_compile.md |
| Coalesce page indices for viewport warmup | prefetch_compile.md |
| Compile `.nxs` text to `.nxb` via WASM | prefetch_compile.md |
| Decode `.nxb` to `.nxs` text (DevTools) | prefetch_compile.md |
| Attach WASM reducers to NxsReader | prefetch_compile.md |

---

# build_compile_wasm.sh

DOES: Shell script building `nxs_compile_wasm.wasm` from Rust/wasm sources for in-browser `.nxs` → `.nxb` compilation.
SYMBOLS:
- (shell — no exported API)
PATTERNS: wasm build pipeline
USE WHEN: Refreshing WASM after compiler changes; run before `test_wasm.js` or DevTools sync.

---

# nxs_compile.js

DOES: Lazy-loads the compile WASM module and exposes `NyxisCompiler.compile` / `compileColumnar` for Node and browser bundlers.
SYMBOLS:
- ensureWasm() Promise<void>
- NyxisCompiler.compile(source: string) Uint8Array
- NyxisCompiler.compileColumnar(source: string) Uint8Array
DEPENDS: ./nxs_compile_wasm.js
PATTERNS: lazy wasm init
USE WHEN: Server-side or bundler path that ships the wasm glue separately.

---

# nxs_compile_wasm.js

DOES: Auto-generated wasm-bindgen glue: `compile_nxs`, `compile_nxs_columnar`, memory views, and sync init helpers.
SYMBOLS:
- compile_nxs(source: string) Uint8Array
- compile_nxs_columnar(source: string) Uint8Array
- initSync(module) void
DEPENDS: nxs_compile_wasm_bg.wasm (artifact)
PATTERNS: wasm-bindgen, no-libc compile
USE WHEN: Low-level WASM imports; prefer `nxs_compile.js` facade.

---

# nxs_decode.js

DOES: Decodes in-memory `.nxb` buffers to human-readable `.nxs`-style text; powers DevTools Inspector and `NyxisJsSDK.decodeToNxs`.
SYMBOLS:
- decodeToNxs(input, options) string
- isNxbBuffer(bytes) boolean
- NyxisJsSDK.decodeToNxs / isNxbBuffer
DEPENDS: (uses inline decode helpers; sync copy in devtools-extension/lib/)
PATTERNS: lazy field decode, sigil-aware formatting
USE WHEN: Inspecting binary payloads without round-tripping through Rust CLI.

---

# pattern.js

DOES: `AccessPatternDetector` tracking sequential runs vs random jumps; exports pattern name constants and upgrade thresholds shared with prefetch.js.
SYMBOLS:
- AccessPatternDetector.observe(index)
- AccessPatternDetector.pattern() string
- AccessPatternDetector.predictNext(depth, recordCount) number[]
DEPENDS: (none)
PATTERNS: sliding-window detector
USE WHEN: Debugging adaptive prefetch; imported by prefetch.js and NxsReader.

---

# prefetch.js

DOES: Page-cache LRU, in-flight dedup map, coalesced page ranges, initial strategy selection, and viewport/page-index helpers for JS adaptive prefetch.
SYMBOLS:
- initialStrategy(hint, fileSize) string
- coalescePageIndices(indices, gapPages, pageSize) PageRange[]
- PageCache.get/set/evictOne()
- InFlightMap.begin/wait/finish(pageIndex)
- rowDataSector(tailStart, fileSize) { start, length }
DEPENDS: ./pattern.js
PATTERNS: LRU, range coalescing, in-flight dedup
USE WHEN: Integrated from `nxs.js` reader options; mirror of C/Go prefetch modules.

---

# test/prefetch.test.js

DOES: Node test runner for coalescing, pattern detector, PageCache LRU, and viewport integration against synthetic writers.
SYMBOLS:
- test(name, fn)
- buildRecords(n) Uint8Array
DEPENDS: ../prefetch.js, ../nxs.js, ../nxs_writer.js
USE WHEN: `node test/prefetch.test.js`.

---

# test_wasm.js

DOES: Parity tests comparing pure JS `NxsReader` reducers against WASM-attached reader on fixture `records_1000.nxb`.
SYMBOLS:
- test(name, fn)
DEPENDS: ./nxs.js, ./wasm.js
USE WHEN: After rebuilding wasm reducers; `node test_wasm.js [fixtures_dir]`.

---

# wasm.js

DOES: Loads `nxs_reducers.wasm` (when present), exposes `NxsWasm` reducer attachment and `WasmSpanWriter` for span encoding benchmarks.
SYMBOLS:
- NxsWasm.attach(reader) void
- NxsWasm.sumF64(key) number
- WasmSpanWriter.encodeSpan(fields) Uint8Array
DEPENDS: wasm/nxs_reducers.wasm (optional artifact)
PATTERNS: wasm linear memory, zero-copy attach
USE WHEN: Benchmarking WASM vs JS reducers; optional — reducers wasm may be absent in minimal checkouts.
