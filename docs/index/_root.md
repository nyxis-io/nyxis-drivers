# LOI Index — nyxis-drivers

Generated: 2026-05-24
Source paths: c/, go/, js/, py/, ruby/, php/, kotlin/, csharp/, swift/, devtools-extension/

MIT language SDKs for the NXS binary format (`.nxb`): zero-copy readers, slot-indexed writers, adaptive prefetch, columnar/PAX layouts, and C extensions for Ruby/PHP/Python.

## TASK → LOAD

| Task | Load |
|------|------|
| Read or write `.nxb` from C | c/_root.md |
| Adaptive row prefetch / viewport warmup (C) | c/prefetch.md |
| Column-buffer prefetch on columnar `.nxb` (C) | c/prefetch.md |
| Read or write `.nxb` from Go | go/_root.md |
| Go columnar/PAX layouts and `ColSumF64` | go/prefetch_col.md |
| Go adaptive prefetch engine | go/prefetch_col.md |
| Read or write `.nxb` in JavaScript (Node/browser) | js/_root.md |
| JS prefetch, WASM compile, decode-to-`.nxs` | js/prefetch_compile.md |
| Read or write `.nxb` in pure Python | py/_root.md |
| Python C extension (`_nxs`) for throughput | py/c_ext.md |
| Python prefetch engine and pattern detector | py/prefetch.md |
| Read or write `.nxb` from Ruby | langs/_root.md → langs/ruby.md |
| Read or write `.nxb` from PHP | langs/_root.md → langs/php.md |
| Read or write `.nxb` from Kotlin/JVM | langs/kotlin.md |
| Read or write `.nxb` from C# (.NET) | langs/csharp.md |
| Read or write `.nxb` from Swift | langs/swift.md |
| Decode `.nxb` network responses in Chrome DevTools | devtools/_root.md |
| Profile selective-read latency (C workload A) | c/prefetch.md |

## PATTERN → LOAD

| Pattern | Load |
|---------|------|
| LEB128 bitmask + offset-table object header | c/reader.md, go/reader.md, js/reader.md |
| Tail-index O(1) random record access | c/reader.md, go/reader.md, js/reader.md |
| MurmurHash3-64 DictHash integrity | c/reader.md, go/reader.md, py/reader.md |
| Schema-once / slot-indexed emit | c/reader.md, go/reader.md, js/reader.md, py/reader.md |
| Adaptive prefetch (lazy / adaptive / eager) | c/prefetch.md, go/prefetch_col.md, js/prefetch_compile.md, py/prefetch.md, langs/ruby.md |
| Page-cache LRU + coalesced range fetch | c/prefetch.md, go/prefetch_col.md, js/prefetch_compile.md |
| Column-buffer warmup (`prefetch_column`) | c/prefetch.md, go/prefetch_col.md, langs/kotlin.md, langs/csharp.md, langs/swift.md |
| Columnar dense `ColSumF64` fast path | go/prefetch_col.md, langs/kotlin.md |
| PAX streaming incremental pages | go/prefetch_col.md, js/reader.md |
| CPython buffer-protocol C extension | py/c_ext.md, langs/ruby.md, langs/php.md |
| Freestanding WASM compile (no libc) | js/prefetch_compile.md |
| Chrome DevTools Network → `.nxs` decode | devtools/extension.md |

## GOVERNANCE WATCHLIST

| Room | Health | Security | Committee Note |
|------|--------|----------|----------------|
| c/reader.md | normal | sensitive | Raw pointer arithmetic; `nxs_writer` included by `py/_nxs.c` and `ruby/ext/nxs/nxs_ext.c` |
| py/c_ext.md | normal | sensitive | `_nxs.c` uses buffer-protocol pointers over caller memory |
| langs/ruby.md | normal | sensitive | `nxs_ext.c` inline LEB128 over Ruby string buffers |
| langs/php.md | normal | sensitive | `nxs_ext.c` Zend lifecycle over PHP string data |

## Buildings

| Subdomain | Description | Rooms |
|-----------|-------------|-------|
| c/ | C99 reader/writer, adaptive prefetch, column prefetch, benches, tests | reader.md, prefetch.md |
| go/ | Go reader/writer, fast reducers, query, columnar/PAX, prefetch | reader.md, prefetch_col.md |
| js/ | ESM reader/writer, prefetch, WASM compile/decode, tests | reader.md, prefetch_compile.md |
| py/ | Pure-Python reader/writer, C extension, prefetch | reader.md, c_ext.md, prefetch.md |
| langs/ | Ruby, PHP, Kotlin, C#, Swift SDKs | ruby.md, php.md, kotlin.md, csharp.md, swift.md |
| devtools/ | Chrome/Firefox Nyxis Inspector extension | extension.md, panel.md |
