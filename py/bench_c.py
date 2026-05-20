"""NXS (C ext) vs NXS (pure Python) vs JSON benchmark.

Run: python3 bench_c.py [fixtures_dir]
"""
from __future__ import annotations

import json
import random
import sys
import time
from pathlib import Path

import _nxs
from nxs import NxsReader as PyNxsReader


def bench(iters, fn):
    for _ in range(max(3, iters // 10)): fn()
    start = time.perf_counter()
    for _ in range(iters): fn()
    return (time.perf_counter() - start) / iters


def fmt_time(s):
    if s < 1e-6:   return f"{s * 1e9:.0f} ns"
    if s < 1e-3:   return f"{s * 1e6:.1f} µs"
    if s < 1:      return f"{s * 1e3:.2f} ms"
    return f"{s:.2f} s"


def fmt_bytes(n):
    if n < 1024:            return f"{n} B"
    if n < 1024 * 1024:     return f"{n/1024:.1f} KB"
    return f"{n/1024/1024:.2f} MB"


def row(label, avg, baseline):
    if baseline == avg: ratio = "baseline"
    elif avg < baseline: ratio = f"{baseline/avg:.1f}x faster"
    else: ratio = f"{avg/baseline:.1f}x slower"
    print(f"  │  {label:<42} {fmt_time(avg):>10}   {ratio}")


def section(title):
    print(f"\n  ┌─ {title} {'─' * max(0, 76 - len(title))}┐")


def endsection():
    print(f"  └{'─' * 79}┘")


def run_scale(fixture_dir, n):
    nxb_path = fixture_dir / f"records_{n}.nxb"
    json_path = fixture_dir / f"records_{n}.json"
    if not nxb_path.exists():
        return

    nxb_buf = nxb_path.read_bytes()
    json_str = json_path.read_text()

    print(f"\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  n = {n:,}  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
    print(f"  .nxb size:  {fmt_bytes(len(nxb_buf)):>10}")
    print(f"  .json size: {fmt_bytes(len(json_str)):>10}")

    if n >= 1_000_000:
        parse_iters, random_iters, iterate_iters, cold_iters = 5, 30_000, 3, 5
    elif n >= 100_000:
        parse_iters, random_iters, iterate_iters, cold_iters = 30, 100_000, 10, 30
    elif n >= 10_000:
        parse_iters, random_iters, iterate_iters, cold_iters = 300, 200_000, 100, 200
    else:
        parse_iters, random_iters, iterate_iters, cold_iters = 3000, 200_000, 1000, 1000

    # ── 1. Open ─────────────────────────────────────────────────────────────
    section("Open file")
    t_json = bench(parse_iters, lambda: json.loads(json_str))
    row("json.loads(entire document)", t_json, t_json)
    t_py = bench(parse_iters, lambda: PyNxsReader(nxb_buf))
    row("NxsReader (pure Python)", t_py, t_json)
    t_c = bench(parse_iters, lambda: _nxs.Reader(nxb_buf))
    row("_nxs.Reader (C extension)", t_c, t_json)
    endsection()

    # ── 2. Warm random access ──────────────────────────────────────────────
    section("Random-access read (1 field from 1 record)")
    parsed = json.loads(json_str)
    py_reader = PyNxsReader(nxb_buf)
    c_reader  = _nxs.Reader(nxb_buf)
    rng = random.Random(0)
    idxs = [rng.randrange(n) for _ in range(random_iters)]

    t_json = bench(random_iters, lambda: parsed[idxs[rng.randrange(random_iters)]]["username"])
    row("arr[k]['username']", t_json, t_json)
    t_py = bench(random_iters, lambda: py_reader.record(idxs[rng.randrange(random_iters)]).get_str("username"))
    row("py_reader.record(k).get_str(...)", t_py, t_json)
    t_c = bench(random_iters, lambda: c_reader.record(idxs[rng.randrange(random_iters)]).get_str("username"))
    row("c_reader.record(k).get_str(...)", t_c, t_json)
    endsection()

    # ── 3. Cold start ──────────────────────────────────────────────────────
    section("First access — open + read 1 field")
    k = n // 2
    t_json = bench(cold_iters, lambda: json.loads(json_str)[k]["username"])
    row("json.loads + arr[k]['username']", t_json, t_json)
    t_py = bench(cold_iters, lambda: PyNxsReader(nxb_buf).record(k).get_str("username"))
    row("PyNxsReader + record(k).get_str(...)", t_py, t_json)
    t_c = bench(cold_iters, lambda: _nxs.Reader(nxb_buf).record(k).get_str("username"))
    row("_nxs.Reader + record(k).get_str(...)", t_c, t_json)
    endsection()

    # ── 4. Full scan ───────────────────────────────────────────────────────
    section("Full scan — sum of 'score' across all records (per-record API)")
    def json_scan():
        s = 0.0
        for r in parsed: s += r["score"]
    def py_scan():
        s = 0.0
        for r in py_reader.records(): s += r.get_f64("score")
    def c_scan():
        s = 0.0
        rc = c_reader.record_count
        for i in range(rc): s += c_reader.record(i).get_f64("score")

    t_json = bench(iterate_iters, json_scan)
    row("for r in arr: sum += r['score']", t_json, t_json)
    t_py = bench(iterate_iters, py_scan)
    row("py_reader scan", t_py, t_json)
    t_c = bench(iterate_iters, c_scan)
    row("c_reader scan", t_c, t_json)
    endsection()

    # ── 5. Columnar / reducer scan ─────────────────────────────────────────
    section("Columnar scan — same sum, using bulk APIs")
    t_json2 = bench(iterate_iters, lambda: sum(r["score"] for r in parsed))
    row("sum(r['score'] for r in arr)", t_json2, t_json2)
    t_c_list = bench(iterate_iters, lambda: sum(c_reader.scan_f64("score")))
    row("sum(c_reader.scan_f64('score'))", t_c_list, t_json2)
    t_c_sum = bench(iterate_iters, lambda: c_reader.sum_f64("score"))
    row("c_reader.sum_f64('score')  [in-C reducer]", t_c_sum, t_json2)
    endsection()


def main():
    fixture_dir = Path(sys.argv[1] if len(sys.argv) > 1 else "../js/fixtures")

    print("\n╔════════════════════════════════════════════════════════════════════════════════╗")
    print("║     NXS Python  —  C extension vs pure-Python vs stdlib json                  ║")
    print("╚════════════════════════════════════════════════════════════════════════════════╝")
    print(f"\n  Python:   {sys.version.split()[0]}")
    print(f"  Platform: {sys.platform}")
    print(f"  Fixtures: {fixture_dir}")

    for n in (1_000, 10_000, 100_000, 1_000_000):
        run_scale(fixture_dir, n)

    print("\n" + "═" * 80)
    print("  Notes:")
    print("    • json.loads: CPython's built-in C extension — hard to beat on full parse.")
    print("    • _nxs.Reader: custom C extension, O(1) tail-index lookups.")
    print("    • Purely additive over pure-Python — same API surface.\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
