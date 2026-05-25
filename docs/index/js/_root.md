---
room: _root
subdomain: js
source_paths: js/
see_also: ["_root.md"]
architectural_health: normal
security_tier: normal
---

# JavaScript — Building Router

Subdomain: js/
Source paths: js/

## TASK → LOAD

| Task | Load |
|------|------|
| Read .nxb files in Node or browser | reader.md |
| Write / emit .nxb from JavaScript | reader.md |
| Run or add JS reader/writer tests | reader.md |
| Adaptive prefetch / page cache | prefetch_compile.md |
| Compile `.nxs` to `.nxb` via WASM | prefetch_compile.md |
| Decode `.nxb` to `.nxs` text | prefetch_compile.md |
| Attach optional WASM reducers | prefetch_compile.md |

## Rooms

| Room | Source paths | Files |
|------|-------------|-------|
| reader.md | js/nxs.js, nxs_writer.js, test.js, eslint.config.js, package.json | 5 |
| prefetch_compile.md | js/prefetch.js, pattern.js, nxs_compile.js, nxs_compile_wasm.js, nxs_decode.js, wasm.js, test/prefetch.test.js, test_wasm.js | 9 |
