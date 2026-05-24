"""Prefetch unit tests — run: python3 test_prefetch.py"""
from __future__ import annotations

import asyncio
import sys
import threading
import time

from nxs import (
    DEFAULT_PAGE_SIZE,
    HINT_FULL,
    InFlightMap,
    NxsReader,
    PageCache,
    coalesce_page_indices,
)
from nxs_writer import NxsSchema, NxsWriter
from pattern import AccessPatternDetector


def build_compact_records(n: int) -> bytes:
    schema = NxsSchema(["id", "tag"])
    w = NxsWriter(schema)
    for i in range(n):
        w.begin_object()
        w.write_i64(0, i)
        w.write_str(1, f"r{i}")
        w.end_object()
    return w.finish()


def build_records(n: int) -> bytes:
    schema = NxsSchema(["id", "username", "score", "active"])
    w = NxsWriter(schema)
    for i in range(n):
        w.begin_object()
        w.write_i64(0, i)
        w.write_str(1, f"user_{i}")
        w.write_f64(2, i * 0.25)
        w.write_bool(3, i % 2 == 0)
        w.end_object()
    return w.finish()


def main() -> int:
    passed = failed = 0

    def case(name: str, fn) -> None:
        nonlocal passed, failed
        try:
            fn()
            print(f"  ✓ {name}")
            passed += 1
        except Exception as e:
            print(f"  ✗ {name}\n      {e}")
            failed += 1

    async def case_async(name: str, fn) -> None:
        nonlocal passed, failed
        try:
            await fn()
            print(f"  ✓ {name}")
            passed += 1
        except Exception as e:
            print(f"  ✗ {name}\n      {e}")
            failed += 1

    print("\nNXS Python Prefetch — Tests\n")

    case("pattern unknown until min observations", lambda: _test_pattern_unknown())
    case("pattern sequential small deltas", lambda: _test_pattern_sequential())
    case("pattern random large jumps", lambda: _test_pattern_random())
    case("predict_next sequential", lambda: _test_predict_next())
    case(
        "coalesce_page_indices [3,4,6,7,12] gap=1 → 3 ranges",
        lambda: _test_coalesce(),
    )
    case("PageCache LRU evicts at max_pages", lambda: _test_lru())
    case("InFlightMap dedupes concurrent page loads", lambda: _test_inflight())
    case(
        "prefetch_viewport uses ≤3 coalesced fetch_range calls for 50 records",
        lambda: _test_coalesced_fetch(),
    )
    case(
        "prefetch_viewport_basic — records readable after prefetch",
        lambda: _test_basic(),
    )
    case("prefetch_memory_eviction", lambda: _test_eviction())
    case(
        "prefetch_deduplication — parallel viewport same page",
        lambda: _test_dedup(),
    )
    case("cache_stats returns expected keys", lambda: _test_cache_stats())
    case("pause stops speculative prefetch", lambda: _test_pause_stops_speculative())
    case("hint full small file eager at open", lambda: _test_hint_full_eager())
    case(
        "sequential upgrade to eager after 150 record() calls",
        lambda: _test_sequential_upgrade(),
    )

    async def run_async_tests() -> None:
        await case_async(
            "prefetch_viewport_async with async fetch_range",
            lambda: _test_async_fetch(),
        )

    asyncio.run(run_async_tests())

    print(f"\n{passed} passed, {failed} failed\n")
    return 0 if failed == 0 else 1


def _test_pattern_unknown() -> None:
    d = AccessPatternDetector()
    for i in range(8):
        d.observe(i)
    assert d.pattern() == "unknown", f"expected unknown, got {d.pattern()}"
    d.observe(8)
    assert d.pattern() != "unknown", "expected classified pattern after 9th access"


def _test_pattern_sequential() -> None:
    d = AccessPatternDetector()
    for i in range(20):
        d.observe(i)
    assert d.pattern() == "sequential", f"expected sequential, got {d.pattern()}"


def _test_pattern_random() -> None:
    d = AccessPatternDetector()
    for i in range(8):
        d.observe(i)
    for k in range(12):
        d.observe(k * 200)
    assert d.pattern() == "random", f"expected random, got {d.pattern()}"


def _test_predict_next() -> None:
    d = AccessPatternDetector()
    for i in range(10):
        d.observe(i)
    assert d.predict_next(4, 100) == [10, 11, 12, 13]


def _test_coalesce() -> None:
    ranges = coalesce_page_indices([3, 4, 6, 7, 12], 1, DEFAULT_PAGE_SIZE)
    assert len(ranges) == 3, f"expected 3 ranges, got {len(ranges)}"
    assert ranges[0]["page_start"] == 3 and ranges[0]["page_end"] == 4
    assert ranges[1]["page_start"] == 6 and ranges[1]["page_end"] == 7
    assert ranges[2]["page_start"] == 12 and ranges[2]["page_end"] == 12


def _test_lru() -> None:
    cache = PageCache(2, 64)
    cache.set(0, b"x" * 64)
    cache.set(1, b"y" * 64)
    cache.get(0)
    cache.set(2, b"z" * 64)
    assert not cache.has(1), "page 1 should be evicted"
    assert cache.has(0) and cache.has(2)


def _test_inflight() -> None:
    inflight = InFlightMap()
    fetches = {"n": 0}
    lock = threading.Lock()

    def load() -> None:
        entry, leader = inflight.begin(3)
        if not leader:
            inflight.wait(3)
            return
        time.sleep(0.01)
        with lock:
            fetches["n"] += 1
        entry.data = b"\x00" * 8
        inflight.finish(3, entry)

    threads = [threading.Thread(target=load) for _ in range(2)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert fetches["n"] == 1, f"expected 1 fetch, got {fetches['n']}"


def _test_coalesced_fetch() -> None:
    buf = build_records(60)
    ranges: list[dict[str, int]] = []

    def fetch_range(start: int, length: int) -> bytes:
        ranges.append({"start": start, "len": length})
        return bytes(buf[start:start + length])

    reader = NxsReader(
        buf,
        max_pages=64,
        coalesce_gap_pages=1,
        fetch_range=fetch_range,
    )
    reader.prefetch_viewport(0, 49)
    assert len(ranges) <= 3, f"expected ≤3 fetches, got {len(ranges)}: {ranges}"
    stats = reader.cache_stats()
    assert stats["fetches_issued"] == len(ranges), "fetches_issued mismatch"


def _test_basic() -> None:
    buf = build_records(55)
    reader = NxsReader(buf)
    reader.prefetch_viewport(0, 49)
    assert reader.record(49).get_i64("id") == 49


def _test_eviction() -> None:
    buf = build_records(20)
    reader = NxsReader(buf, max_pages=2, page_size=256, coalesce_gap_pages=0)
    reader.prefetch_viewport(0, 0)
    reader.prefetch_viewport(19, 19)
    stats = reader.cache_stats()
    assert stats["pages_cached"] <= 2, f"cache grew past max: {stats['pages_cached']}"


def _test_dedup() -> None:
    buf = build_records(10)
    calls = {"n": 0}
    lock = threading.Lock()

    def fetch_range(start: int, length: int) -> bytes:
        with lock:
            calls["n"] += 1
        time.sleep(0.005)
        return bytes(buf[start:start + length])

    reader = NxsReader(buf, max_pages=8, fetch_range=fetch_range)
    threads = [
        threading.Thread(target=reader.prefetch_viewport, args=(0, 4))
        for _ in range(2)
    ]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert calls["n"] <= 3, f"too many fetches: {calls['n']}"


def _test_cache_stats() -> None:
    buf = build_records(5)
    reader = NxsReader(buf, hint=1, max_pages=32)
    reader.prefetch_viewport(0, 4)
    stats = reader.cache_stats()
    for key in (
        "pages_cached", "pages_max", "memory_used_bytes",
        "cache_hits", "cache_misses", "fetches_issued",
        "strategy", "pattern",
    ):
        assert key in stats, f"missing key {key}"
    assert stats["pages_max"] == 32
    assert stats["strategy"] == "adaptive"
    assert stats["pattern"] == "unknown"


def _test_pause_stops_speculative() -> None:
    buf = build_compact_records(200)
    reader = NxsReader(buf)
    for i in range(25):
        reader.record(i)
    assert reader.cache_stats()["pattern"] == "sequential"
    before = reader.cache_stats()["fetches_issued"]
    reader.pause_prefetch()
    reader.record(26)
    assert reader.cache_stats()["fetches_issued"] == before
    reader.resume_prefetch()
    reader.record(27)
    assert reader.cache_stats()["fetches_issued"] >= before


def _test_hint_full_eager() -> None:
    buf = build_records(200)
    reader = NxsReader(buf, hint=HINT_FULL)
    reader.warmup()
    assert reader.cache_stats()["strategy"] == "eager"


def _test_sequential_upgrade() -> None:
    buf = build_records(200)
    reader = NxsReader(buf)
    for i in range(150):
        reader.record(i)
    reader.warmup()
    stats = reader.cache_stats()
    assert stats["strategy"] == "eager", f"expected eager after upgrade, got {stats['strategy']}"
    assert stats["pattern"] == "sequential", (
        f"expected sequential pattern, got {stats['pattern']}"
    )


async def _test_async_fetch() -> None:
    buf = build_records(10)
    calls: list[tuple[int, int]] = []

    async def fetch_range(start: int, length: int) -> bytes:
        await asyncio.sleep(0.001)
        calls.append((start, length))
        return bytes(buf[start:start + length])

    reader = NxsReader(buf, fetch_range=fetch_range)
    await reader.prefetch_viewport_async(0, 9)
    assert reader.record(9).get_i64("id") == 9
    assert len(calls) >= 1


if __name__ == "__main__":
    sys.exit(main())
