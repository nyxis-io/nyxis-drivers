---
room: _root
subdomain: py
source_paths: py/
see_also: ["_root.md"]
architectural_health: normal
security_tier: normal
---

# Python — Building Router

Subdomain: py/
Source paths: py/

## TASK → LOAD

| Task | Load |
|------|------|
| Read .nxb files in pure Python | reader.md |
| Write .nxb output from Python | reader.md |
| Run or add Python reader/writer tests | reader.md |
| Benchmark pure-Python NXS vs JSON | reader.md |
| Use the C-accelerated reader/writer | c_ext.md |
| Benchmark C extension vs pure Python | c_ext.md |
| Measure WAL-append throughput | c_ext.md |
| Verify C extension parity | c_ext.md |
| Adaptive prefetch in pure Python | prefetch.md |

## Rooms

| Room | Source paths | Files |
|------|-------------|-------|
| reader.md | py/nxs.py, nxs_writer.py, pattern.py, test_nxs.py, bench.py, pyproject.toml, build_ext.sh | 7 |
| c_ext.md | py/_nxs.c, bench_c.py, bench_wal.py, test_c_ext.py | 4 |
| prefetch.md | py/pattern.py, test_prefetch.py, nxs.py (PrefetchEngine) | 3 |
