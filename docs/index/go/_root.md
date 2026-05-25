---
room: _root
subdomain: go
source_paths: go/
see_also: ["_root.md"]
architectural_health: normal
security_tier: normal
---

# Go — Building Router

Subdomain: go/
Source paths: go/

## TASK → LOAD

| Task | Load |
|------|------|
| Read .nxb files from Go | reader.md |
| Write .nxb output from Go | reader.md |
| Use fast unsafe aggregate reducers | reader.md |
| Benchmark Go NXS vs JSON/CSV | reader.md |
| Run Go tests | reader.md |
| Columnar/PAX layout or prefetch | prefetch_col.md |
| Predicate queries over records | reader.md |

## Rooms

| Room | Source paths | Files |
|------|-------------|-------|
| reader.md | go/nxs.go, fast.go, writer.go, query.go, nxs_test.go, query_test.go, bench_wal_test.go, cmd/bench/main.go | 8 |
| prefetch_col.md | go/prefetch.go, pattern.go, column_prefetch.go, col.go, pax_stream.go, *_test.go | 9 |
