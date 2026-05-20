"""NXS vs JSON benchmark — Python (stdlib only).

Compares:
  1. Open / parse full structure
  2. Random access — 1 field from record k (warm)
  3. Cold start — open file + read 1 field
  4. Full scan — sum of 'score' across all records

Usage: python3 bench.py [fixtures_dir]
"""
from __future__ import annotations

import json
import random
import sys
import time
from pathlib import Path

from nxs import NxsReader


def bench(iters: int, fn) -> float:
    """Return average seconds per iteration."""
    for _ in range(max(3, iters // 10)):
        fn()  # warmup
    start = time.perf_counter()
    for _ in range(iters):
        fn()
    return (time.perf_counter() - start) / iters


def fmt_time(s: float) -> str:
    if s < 1e-6:   return f"{s * 1e9:.0f} ns"
    if s < 1e-3:   return f"{s * 1e6:.1f} µs"
    if s < 1:      return f"{s * 1e3:.2f} ms"
    return f"{s:.2f} s"


def fmt_bytes(n: int) -> str:
    if n < 1024:            return f"{n} B"
    if n < 1024 * 1024:     return f"{n/1024:.1f} KB"
    return f"{n/1024/1024:.2f} MB"


def row(label: str, avg: float, baseline: float) -> None:
    if baseline == avg:
        ratio = "baseline"
    elif avg < baseline:
        ratio = f"{baseline/avg:.1f}x faster"
    else:
        ratio = f"{avg/baseline:.1f}x slower"
    print(f"  │  {label:<42} {fmt_time(avg):>10}   {ratio}")


def section(title: str) -> None:
    print(f"\n  ┌─ {title} {'─' * max(0, 76 - len(title))}┐")


def endsection() -> None:
    print(f"  └{'─' * 79}┘")


def run_scale(fixture_dir: Path, n: int) -> None:
    nxb_path = fixture_dir / f"records_{n}.nxb"
    json_path = fixture_dir / f"records_{n}.json"
    if not nxb_path.exists() or not json_path.exists():
        print(f"\n  ⚠  skipping n={n}: fixtures missing")
        return

    nxb_buf = nxb_path.read_bytes()
    json_str = json_path.read_text()

    print(f"\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  n = {n:,}  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
    print(f"  .nxb size:  {fmt_bytes(len(nxb_buf)):>10}")
    print(f"  .json size: {fmt_bytes(len(json_str)):>10}  ({len(json_str)/len(nxb_buf):.2f}x NXS)")

    # Iteration counts scaled to wall-clock budget
    if n >= 1_000_000:
        parse_iters, random_iters, iterate_iters, cold_iters = 5, 20_000, 3, 5
    elif n >= 100_000:
        parse_iters, random_iters, iterate_iters, cold_iters = 30, 50_000, 10, 30
    elif n >= 10_000:
        parse_iters, random_iters, iterate_iters, cold_iters = 300, 100_000, 100, 200
    else:
        parse_iters, random_iters, iterate_iters, cold_iters = 3000, 100_000, 1000, 1000

    # ── 1. Open / Parse ─────────────────────────────────────────────────────
    section("Open file (parse full structure)")
    t_json_open = bench(parse_iters, lambda: json.loads(json_str))
    row("json.loads(entire document)", t_json_open, t_json_open)
    t_nxs_open = bench(parse_iters, lambda: NxsReader(nxb_buf))
    row("NxsReader(buffer)", t_nxs_open, t_json_open)
    endsection()

    # ── 2. Warm random access ───────────────────────────────────────────────
    section("Random-access read (1 field from 1 record, averaged over random k)")

    parsed = json.loads(json_str)
    reader = NxsReader(nxb_buf)

    rng = random.Random(0)
    indices = [rng.randrange(n) for _ in range(random_iters)]
    it = iter(indices)

    # Pre-materialize iterator to avoid StopIteration overhead in loop
    def json_lookup():
        k = next(it)
        return parsed[k]["username"]

    t_json_random = bench(random_iters, lambda: parsed[indices[rng.randrange(random_iters)]]["username"])
    row("arr[k]['username'] (pre-parsed)", t_json_random, t_json_random)

    t_nxs_random = bench(random_iters, lambda: reader.record(indices[rng.randrange(random_iters)]).get_str("username"))
    row("reader.record(k).get_str('username')", t_nxs_random, t_json_random)
    endsection()

    # ── 3. Cold start ───────────────────────────────────────────────────────
    section("First access — open file + read 1 field (cold start)")
    cold_idx = n // 2
    t_json_cold = bench(cold_iters, lambda: json.loads(json_str)[cold_idx]["username"])
    row("json.loads + arr[k]['username']", t_json_cold, t_json_cold)
    t_nxs_cold = bench(cold_iters, lambda: NxsReader(nxb_buf).record(cold_idx).get_str("username"))
    row("NxsReader + record(k).get_str(...)", t_nxs_cold, t_json_cold)
    endsection()

    # ── 4. Full scan ────────────────────────────────────────────────────────
    section("Full scan — sum of 'score' field across all records")

    def json_scan():
        s = 0.0
        for r in parsed:
            s += r["score"]
        return s

    def nxs_scan():
        s = 0.0
        for r in reader.records():
            s += r.get_f64("score")
        return s

    t_json_scan = bench(iterate_iters, json_scan)
    row("for r in arr: sum += r['score']", t_json_scan, t_json_scan)
    t_nxs_scan = bench(iterate_iters, nxs_scan)
    row("for r in reader.records(): sum += ...", t_nxs_scan, t_json_scan)
    endsection()


def main() -> int:
    fixture_dir = Path(sys.argv[1] if len(sys.argv) > 1 else "../js/fixtures")

    print("\n╔════════════════════════════════════════════════════════════════════════════════╗")
    print("║           NXS vs JSON  —  Python Benchmark                                    ║")
    print("╚════════════════════════════════════════════════════════════════════════════════╝")
    print(f"\n  Python:   {sys.version.split()[0]}")
    print(f"  Platform: {sys.platform}")
    print(f"  Fixtures: {fixture_dir}")

    for n in (1_000, 10_000, 100_000, 1_000_000):
        try:
            run_scale(fixture_dir, n)
        except Exception as e:
            print(f"\n  ⚠  n={n} failed: {e}")

    print("\n" + "═" * 80)
    print("  Notes:")
    print("    • json.loads parses the entire document eagerly.")
    print("    • NxsReader reads only the preamble + schema + tail-index header.")
    print("    • .record(k) is O(1) — no scan through earlier records.")
    print("    • Python's json is written in C; NXS reader is pure Python (struct/memoryview).\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
