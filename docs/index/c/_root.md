---
room: _root
subdomain: c
source_paths: c/
see_also: ["_root.md"]
architectural_health: normal
security_tier: sensitive
committee_notes: nxs_writer.h/nxs_writer.c are shared as an include by py/_nxs.c and ruby/ext/nxs/nxs_ext.c; changes here propagate to Python and Ruby C extensions.
---

# C — Building Router

Subdomain: c/
Source paths: c/

## TASK → LOAD

| Task | Load |
|------|------|
| Read or write .nxb files from C | reader.md |
| Include NXS in a C/C++ project | reader.md |
| Benchmark C NXS throughput | reader.md |
| Run C smoke tests | reader.md |
| Adaptive prefetch / column warmup | prefetch.md |
| Profile selective-read steps | prefetch.md |

## Rooms

| Room | Source paths | Files |
|------|-------------|-------|
| reader.md | c/nxs.c, nxs.h, nxs_writer.c, nxs_writer.h, bench.c, bench_wal.c, test.c, Makefile, CMakeLists.txt | 9 |
| prefetch.md | c/nxs_prefetch.c, nxs_prefetch.h, nxs_column_prefetch.c, nxs_column_prefetch.h, test_prefetch.c, profile_selective.c | 6 |
