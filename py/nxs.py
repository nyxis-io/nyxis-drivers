"""NXS Reader — zero-copy .nxb parser for Python.

Implements the Nyxis v1.1 binary wire format.

Usage:
    from nxs import NxsReader

    with open("data.nxb", "rb") as f:
        buf = f.read()                      # or mmap.mmap(...)
    reader = NxsReader(buf)

    reader.record_count                      # -> 1_000_000
    obj = reader.record(42)                  # O(1) jump
    obj.get_str("username")                  # decode one field

The reader does NOT materialize the full file. Each ``record(i)`` returns a
lightweight view; ``.get_*()`` decodes a single field on demand.
"""
from __future__ import annotations

import asyncio
import inspect
import struct
import threading
from typing import Any, Callable, Iterator, Optional, Union

from pattern import (
    PATTERN_SEQUENTIAL,
    UPGRADE_SEQUENTIAL_THRESHOLD,
    AccessPatternDetector,
)


# Adaptive prefetch constants (spec §6–§8.4; native coalesce_gap=1).
HINT_UNKNOWN = 0
HINT_SEQUENTIAL = 1
HINT_RANDOM = 2
HINT_FULL = 3
HINT_PARTIAL = 4

DEFAULT_PAGE_SIZE = 65536
DEFAULT_MAX_PAGES = 64
DEFAULT_COALESCE_GAP_PAGES = 1
DEFAULT_PREFETCH_DEPTH = 4
EAGER_THRESHOLD_MB = 10
LAZY_THRESHOLD_MB = 50


def initial_strategy(hint: int, file_size: int) -> str:
    """Select lazy/adaptive/eager from hint and file size (spec §5.1)."""
    file_size_mb = file_size // (1024 * 1024)
    if hint == HINT_FULL and file_size_mb <= EAGER_THRESHOLD_MB:
        return "eager"
    if file_size_mb > LAZY_THRESHOLD_MB:
        return "lazy"
    return "adaptive"


def row_data_sector(tail_start: int, file_size: int) -> tuple[int, int]:
    """Row-layout data sector byte range (start, length)."""
    sector_start = 32
    if tail_start > sector_start and tail_start <= file_size:
        return sector_start, tail_start - sector_start
    return sector_start, 0


# Magic bytes (little-endian u32)
MAGIC_FILE   = 0x4E595842  # NYXB
MAGIC_OBJ    = 0x4E59584F  # NYXO
MAGIC_LIST   = 0x4E59584C  # NYXL
MAGIC_FOOTER = 0x2153584E  # NXS!

# Sigil bytes
SIGIL_INT     = 0x3D  # =
SIGIL_FLOAT   = 0x7E  # ~
SIGIL_BOOL    = 0x3F  # ?
SIGIL_STR     = 0x22  # "
SIGIL_TIME    = 0x40  # @

# Pre-built struct unpackers (faster than struct.unpack_from with format string each call)
_U16 = struct.Struct("<H")
_U32 = struct.Struct("<I")
_U64 = struct.Struct("<Q")
_I64 = struct.Struct("<q")
_F64 = struct.Struct("<d")


class NxsError(Exception):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code


def coalesce_page_indices(
    indices: list[int],
    gap_pages: int,
    page_size: int = DEFAULT_PAGE_SIZE,
) -> list[dict[str, int]]:
    """Merge sorted unique page indices when gap <= gap_pages (inclusive)."""
    if not indices:
        return []
    uniq = sorted(set(indices))
    spans: list[tuple[int, int]] = []
    start = end = uniq[0]
    for p in uniq[1:]:
        if p - end <= gap_pages:
            end = p
        else:
            spans.append((start, end))
            start = end = p
    spans.append((start, end))
    return [
        {
            "page_start": a,
            "page_end": b,
            "byte_start": a * page_size,
            "byte_length": (b - a + 1) * page_size,
        }
        for a, b in spans
    ]


def _clamp_page_ranges(ranges: list[dict[str, int]], file_size: int) -> list[dict[str, int]]:
    out: list[dict[str, int]] = []
    for r in ranges:
        length = r["byte_length"]
        if r["byte_start"] + length > file_size:
            length = file_size - r["byte_start"]
        if length <= 0:
            continue
        out.append({**r, "byte_length": length})
    return out


def page_indices_for_viewport(
    start_index: int,
    end_index: int,
    page_size: int,
    record_offset: Callable[[int], int],
) -> list[int]:
    return [record_offset(i) // page_size for i in range(start_index, end_index + 1)]


class PageCache:
    """LRU page cache with optional pinning (Adaptive-prefetch-spec §6)."""

    __slots__ = ("max_pages", "page_size", "pages", "_clock", "hits", "misses")

    def __init__(self, max_pages: int = DEFAULT_MAX_PAGES,
                 page_size: int = DEFAULT_PAGE_SIZE) -> None:
        self.max_pages = max_pages
        self.page_size = page_size
        self.pages: dict[int, dict[str, Any]] = {}
        self._clock = 0
        self.hits = 0
        self.misses = 0

    def has(self, page_index: int) -> bool:
        return page_index in self.pages

    def get(self, page_index: int) -> Optional[bytes]:
        entry = self.pages.get(page_index)
        if entry is None:
            self.misses += 1
            return None
        self._clock += 1
        entry["last_used"] = self._clock
        self.hits += 1
        return entry["data"]

    def set(self, page_index: int, data: bytes, *, pinned: bool = False) -> None:
        if self.max_pages <= 0:
            return
        while len(self.pages) >= self.max_pages:
            if not self._evict_one():
                break
        self._clock += 1
        self.pages[page_index] = {
            "data": data,
            "last_used": self._clock,
            "pinned": pinned,
        }

    def _evict_one(self) -> bool:
        victim = -1
        oldest = float("inf")
        for idx, entry in self.pages.items():
            if entry["pinned"]:
                continue
            if entry["last_used"] < oldest:
                oldest = entry["last_used"]
                victim = idx
        if victim < 0:
            return False
        del self.pages[victim]
        return True

    def pin_pages(self, page_indices: list[int]) -> None:
        for p in page_indices:
            entry = self.pages.get(p)
            if entry is not None:
                entry["pinned"] = True

    def unpin_all(self) -> None:
        for entry in self.pages.values():
            entry["pinned"] = False

    def stats(self) -> dict[str, int]:
        memory_used = sum(len(e["data"]) for e in self.pages.values())
        return {
            "pages_cached": len(self.pages),
            "pages_max": self.max_pages,
            "memory_used_bytes": memory_used,
            "cache_hits": self.hits,
            "cache_misses": self.misses,
        }


class _InFlightEntry:
    __slots__ = ("event", "data", "error")

    def __init__(self) -> None:
        self.event = threading.Event()
        self.data: Optional[bytes] = None
        self.error: Optional[BaseException] = None


class InFlightMap:
    """Deduplicates concurrent page fetches (spec §8.4)."""

    __slots__ = ("_lock", "_map")

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._map: dict[int, _InFlightEntry] = {}

    def has(self, page_index: int) -> bool:
        with self._lock:
            return page_index in self._map

    def wait(self, page_index: int) -> Optional[bytes]:
        with self._lock:
            entry = self._map.get(page_index)
        if entry is None:
            return None
        entry.event.wait()
        if entry.error is not None:
            raise entry.error
        return entry.data

    def begin(self, page_index: int) -> tuple[_InFlightEntry, bool]:
        with self._lock:
            entry = self._map.get(page_index)
            if entry is not None:
                return entry, False
            entry = _InFlightEntry()
            self._map[page_index] = entry
            return entry, True

    def finish(self, page_index: int, entry: _InFlightEntry) -> None:
        entry.event.set()
        with self._lock:
            if self._map.get(page_index) is entry:
                del self._map[page_index]


class PrefetchEngine:
    """Page cache, pattern detection, and adaptive/eager strategies (spec §4–§8.4)."""

    __slots__ = (
        "_lock",
        "_mv",
        "_hint",
        "_page_size",
        "_coalesce_gap_pages",
        "_prefetch_depth",
        "_file_size",
        "_tail_start",
        "_record_count",
        "_record_offset",
        "_fetch_range",
        "_cache",
        "_in_flight",
        "_fetches_issued",
        "_strategy",
        "_detector",
        "_closed",
        "_eager_started",
        "_eager_complete",
        "_eager_cancel",
        "_eager_done",
        "_eager_thread",
    )

    def __init__(
        self,
        *,
        hint: int,
        max_pages: int,
        page_size: int,
        coalesce_gap_pages: int,
        prefetch_depth: int,
        file_size: int,
        tail_start: int,
        record_count: int,
        mv: memoryview,
        record_offset: Callable[[int], int],
        fetch_range: Callable[..., Any],
    ) -> None:
        if max_pages <= 0:
            max_pages = DEFAULT_MAX_PAGES
        if page_size <= 0:
            page_size = DEFAULT_PAGE_SIZE
        if coalesce_gap_pages < 0:
            coalesce_gap_pages = DEFAULT_COALESCE_GAP_PAGES
        if prefetch_depth <= 0:
            prefetch_depth = DEFAULT_PREFETCH_DEPTH

        self._lock = threading.Lock()
        self._mv = mv
        self._hint = hint
        self._page_size = page_size
        self._coalesce_gap_pages = coalesce_gap_pages
        self._prefetch_depth = prefetch_depth
        self._file_size = file_size
        self._tail_start = tail_start
        self._record_count = record_count
        self._record_offset = record_offset
        self._fetch_range = fetch_range
        self._cache = PageCache(max_pages, page_size)
        self._in_flight = InFlightMap()
        self._fetches_issued = 0
        self._strategy = initial_strategy(hint, file_size)
        self._detector = AccessPatternDetector()
        self._closed = False
        self._eager_started = False
        self._eager_complete = False
        self._eager_cancel: Optional[threading.Event] = None
        self._eager_done: Optional[threading.Event] = None
        self._eager_thread: Optional[threading.Thread] = None

        if self._strategy == "eager":
            self._start_eager_background()

    def _is_eager_complete(self) -> bool:
        return self._strategy == "eager" and self._eager_complete

    def on_access(self, index: int) -> None:
        if self._record_count == 0:
            return
        with self._lock:
            if self._closed:
                return
            self._detector.observe(index)
            self._maybe_upgrade_to_eager()
            eager = self._is_eager_complete() or self._strategy == "eager"
            adaptive_seq = (
                self._strategy == "adaptive"
                and self._detector.pattern() == PATTERN_SEQUENTIAL
            )
        if eager:
            return
        off = self._record_offset(index)
        if off >= 0:
            self._cache.get(off // self._page_size)
        if adaptive_seq:
            self._speculative_prefetch()

    def _maybe_upgrade_to_eager(self) -> None:
        if self._strategy != "adaptive":
            return
        if self._detector.pattern() != PATTERN_SEQUENTIAL:
            return
        if self._detector.sequential_runs() < UPGRADE_SEQUENTIAL_THRESHOLD:
            return
        if self._file_size // (1024 * 1024) > EAGER_THRESHOLD_MB:
            return
        self._strategy = "eager"
        self._start_eager_background()

    def _start_eager_background(self) -> None:
        if self._strategy != "eager" or self._eager_started:
            return
        self._eager_started = True
        sector_start, sector_len = row_data_sector(self._tail_start, len(self._mv))
        if sector_len == 0:
            self._eager_complete = True
            return

        cancel = threading.Event()
        done = threading.Event()
        self._eager_cancel = cancel
        self._eager_done = done

        def run() -> None:
            try:
                end = min(sector_start + sector_len, len(self._mv))
                if sector_start >= end:
                    if not cancel.is_set():
                        self._eager_complete = True
                    return
                page_size = self._page_size
                first_page = sector_start // page_size
                last_page = (end - 1) // page_size
                indices = list(range(first_page, last_page + 1))
                missing = [
                    p for p in indices
                    if not self._cache.has(p) and not self._in_flight.has(p)
                ]
                if not missing:
                    if not cancel.is_set():
                        self._eager_complete = True
                    return
                ranges = _clamp_page_ranges(
                    coalesce_page_indices(
                        missing, self._coalesce_gap_pages, page_size,
                    ),
                    len(self._mv),
                )
                self._fetches_issued += 1
                for pr in ranges:
                    if cancel.is_set():
                        return
                    self._fetch_coalesced_range(pr)
                if not cancel.is_set():
                    self._eager_complete = True
            finally:
                done.set()

        thread = threading.Thread(target=run, daemon=True)
        self._eager_thread = thread
        thread.start()

    def _speculative_prefetch(self) -> None:
        with self._lock:
            predicted = self._detector.predict_next(
                self._prefetch_depth, self._record_count,
            )
        if not predicted:
            return
        page_size = self._page_size
        seen: set[int] = set()
        page_indices: list[int] = []
        for idx in predicted:
            off = self._record_offset(idx)
            if off < 0:
                continue
            p = off // page_size
            if p in seen:
                continue
            seen.add(p)
            if not self._cache.has(p) and not self._in_flight.has(p):
                page_indices.append(p)
        if not page_indices:
            return
        ranges = _clamp_page_ranges(
            coalesce_page_indices(
                sorted(page_indices), self._coalesce_gap_pages, page_size,
            ),
            len(self._mv),
        )
        for pr in ranges:
            self._fetch_coalesced_range(pr)

    def prefetch_viewport(self, start_index: int, end_index: int) -> None:
        with self._lock:
            if self._closed:
                return
            page_size = self._page_size
            indices = page_indices_for_viewport(
                start_index, end_index, page_size, self._record_offset,
            )
            missing = {
                p for p in set(indices)
                if not self._cache.has(p) and not self._in_flight.has(p)
            }
            if not missing:
                self._cache.pin_pages(indices)
                self._cache.unpin_all()
                return
            ranges = _clamp_page_ranges(
                coalesce_page_indices(
                    sorted(missing), self._coalesce_gap_pages, page_size,
                ),
                len(self._mv),
            )
        for r in ranges:
            self._fetch_coalesced_range(r)
        with self._lock:
            self._cache.pin_pages(indices)
            self._cache.unpin_all()

    def _fetch_coalesced_range(self, page_range: dict[str, int]) -> None:
        self._fetches_issued += 1
        blob = self._fetch_range_bytes(page_range["byte_start"], page_range["byte_length"])
        page_size = self._page_size
        cache = self._cache
        for p in range(page_range["page_start"], page_range["page_end"] + 1):
            if cache.has(p):
                continue
            page_off = p * page_size - page_range["byte_start"]
            page_len = min(page_size, len(blob) - page_off)
            if page_len <= 0:
                continue
            cache.set(p, blob[page_off:page_off + page_len])

    def _fetch_range_bytes(self, byte_start: int, byte_length: int) -> bytes:
        result = self._fetch_range(byte_start, byte_length)
        if inspect.isawaitable(result):
            raise TypeError(
                "async fetch_range requires prefetch_viewport_async(); "
                "use a synchronous fetch_range with prefetch_viewport()",
            )
        return result

    async def prefetch_viewport_async(self, start_index: int, end_index: int) -> None:
        with self._lock:
            if self._closed:
                return
            page_size = self._page_size
            indices = page_indices_for_viewport(
                start_index, end_index, page_size, self._record_offset,
            )
            missing = {
                p for p in set(indices)
                if not self._cache.has(p) and not self._in_flight.has(p)
            }
            if not missing:
                self._cache.pin_pages(indices)
                self._cache.unpin_all()
                return
            ranges = _clamp_page_ranges(
                coalesce_page_indices(
                    sorted(missing), self._coalesce_gap_pages, page_size,
                ),
                len(self._mv),
            )
        await asyncio.gather(
            *(self._fetch_coalesced_range_async(r) for r in ranges),
        )
        with self._lock:
            self._cache.pin_pages(indices)
            self._cache.unpin_all()

    async def _fetch_coalesced_range_async(self, page_range: dict[str, int]) -> None:
        self._fetches_issued += 1
        blob = await self._fetch_range_bytes_async(
            page_range["byte_start"], page_range["byte_length"],
        )
        page_size = self._page_size
        cache = self._cache
        for p in range(page_range["page_start"], page_range["page_end"] + 1):
            if cache.has(p):
                continue
            page_off = p * page_size - page_range["byte_start"]
            page_len = min(page_size, len(blob) - page_off)
            if page_len <= 0:
                continue
            cache.set(p, blob[page_off:page_off + page_len])

    async def _fetch_range_bytes_async(self, byte_start: int, byte_length: int) -> bytes:
        result = self._fetch_range(byte_start, byte_length)
        if inspect.isawaitable(result):
            return await result
        return result

    def cache_stats(self) -> dict[str, Any]:
        with self._lock:
            strategy = self._strategy
            pattern = self._detector.pattern()
        stats = self._cache.stats()
        return {
            **stats,
            "fetches_issued": self._fetches_issued,
            "strategy": strategy,
            "pattern": pattern,
        }

    def warmup(self) -> None:
        if self._eager_done is not None:
            self._eager_done.wait()

    def close(self) -> None:
        with self._lock:
            if self._closed:
                return
            self._closed = True
            cancel = self._eager_cancel
            thread = self._eager_thread
        if cancel is not None:
            cancel.set()
        if thread is not None:
            thread.join()


def _murmur3_64(data: memoryview) -> int:
    """MurmurHash3-64 used to validate the schema DictHash."""
    MASK = 0xFFFFFFFFFFFFFFFF
    C1   = 0xFF51AFD7ED558CCD
    C2   = 0xC4CEB9FE1A85EC53
    h    = 0x93681D6255313A99
    length = len(data)
    i = 0
    while i < length:
        k = 0
        for b in range(8):
            if i + b < length:
                k |= data[i + b] << (b * 8)
        k = (k * C1) & MASK
        k ^= k >> 33
        h ^= k
        h = (h * C2) & MASK
        h ^= h >> 33
        i += 8
    h ^= length
    h ^= h >> 33
    h = (h * C1) & MASK
    h ^= h >> 33
    return h


class NxsReader:
    """Parses the preamble, schema, and tail-index of a .nxb buffer.

    The data sector is not walked — records are loaded lazily via ``record(i)``.
    """

    __slots__ = (
        "buf", "mv",
        "version", "flags", "dict_hash", "tail_ptr",
        "keys", "key_sigils", "key_index",
        "record_count", "_tail_start",
        "_schema_end",
        "_prefetch",
    )

    def __init__(
        self,
        buffer: Union[bytes, bytearray, memoryview],
        *,
        hint: int = HINT_UNKNOWN,
        max_pages: int = DEFAULT_MAX_PAGES,
        page_size: int = DEFAULT_PAGE_SIZE,
        coalesce_gap_pages: int = DEFAULT_COALESCE_GAP_PAGES,
        prefetch_depth: int = DEFAULT_PREFETCH_DEPTH,
        fetch_range: Optional[Callable[..., Any]] = None,
    ) -> None:
        if page_size <= 0:
            raise NxsError("ERR_PARSE", "prefetch page_size must be greater than 0")
        if isinstance(buffer, memoryview):
            self.buf = buffer.tobytes() if not buffer.readonly else buffer
            self.mv = buffer
        else:
            self.buf = buffer
            self.mv = memoryview(buffer)

        if len(self.mv) < 32:
            raise NxsError("ERR_OUT_OF_BOUNDS", "file too small")

        # Preamble
        magic = _U32.unpack_from(self.mv, 0)[0]
        if magic != MAGIC_FILE:
            raise NxsError("ERR_BAD_MAGIC", f"expected NYXB, got 0x{magic:08x}")

        self.version   = _U16.unpack_from(self.mv, 4)[0]
        self.flags     = _U16.unpack_from(self.mv, 6)[0]
        self.dict_hash = _U64.unpack_from(self.mv, 8)[0]
        self.tail_ptr  = _U64.unpack_from(self.mv, 16)[0]

        # Footer check
        footer = _U32.unpack_from(self.mv, len(self.mv) - 4)[0]
        if footer != MAGIC_FOOTER:
            raise NxsError("ERR_BAD_MAGIC", "footer magic mismatch")
        if self.tail_ptr == 0:
            if len(self.mv) < 44:
                raise NxsError("ERR_OUT_OF_BOUNDS", "stream footer missing tail pointer")
            self.tail_ptr = _U64.unpack_from(self.mv, len(self.mv) - 12)[0]

        # Schema (if embedded)
        self.keys: list[str] = []
        self.key_sigils: list[int] = []
        self.key_index: dict[str, int] = {}
        if self.flags & 0x0002:
            self._read_schema(32)
            schema_bytes = self.mv[32:self._schema_end]
            computed = _murmur3_64(schema_bytes)
            if computed != self.dict_hash:
                raise NxsError("ERR_DICT_MISMATCH", "schema hash mismatch")

        # Tail-index
        self._read_tail_index()
        self._init_prefetch(
            hint=hint,
            max_pages=max_pages,
            page_size=page_size,
            coalesce_gap_pages=coalesce_gap_pages,
            prefetch_depth=prefetch_depth,
            fetch_range=fetch_range,
        )

    def _init_prefetch(
        self,
        *,
        hint: int,
        max_pages: int,
        page_size: int,
        coalesce_gap_pages: int,
        prefetch_depth: int,
        fetch_range: Optional[Callable[..., Any]],
    ) -> None:
        if fetch_range is not None:
            fetch_fn = fetch_range
        else:
            mv = self.mv

            def fetch_fn(byte_start: int, byte_length: int) -> bytes:
                end = byte_start + byte_length
                if byte_start < 0 or end > len(mv):
                    raise NxsError(
                        "ERR_OUT_OF_BOUNDS",
                        f"fetch range [{byte_start}, {end})",
                    )
                return bytes(mv[byte_start:end])

        self._prefetch = PrefetchEngine(
            hint=hint,
            max_pages=max_pages,
            page_size=page_size,
            coalesce_gap_pages=coalesce_gap_pages,
            prefetch_depth=prefetch_depth,
            file_size=len(self.mv),
            tail_start=self._tail_start,
            record_count=self.record_count,
            mv=self.mv,
            record_offset=self._record_byte_offset,
            fetch_range=fetch_fn,
        )

    def _record_byte_offset(self, i: int) -> int:
        entry = self._tail_start + i * 10
        return _U64.unpack_from(self.mv, entry + 2)[0]

    def prefetch_viewport(self, start_index: int, end_index: int) -> None:
        """Load pages for records [start_index, end_index] into the page cache."""
        if start_index < 0 or end_index < start_index or end_index >= self.record_count:
            raise NxsError(
                "ERR_OUT_OF_BOUNDS",
                f"prefetch_viewport [{start_index}, {end_index}] "
                f"out of [0, {self.record_count})",
            )
        self._prefetch.prefetch_viewport(start_index, end_index)

    async def prefetch_viewport_async(self, start_index: int, end_index: int) -> None:
        """Async variant: parallel coalesced range fetches when fetch_range is async."""
        if start_index < 0 or end_index < start_index or end_index >= self.record_count:
            raise NxsError(
                "ERR_OUT_OF_BOUNDS",
                f"prefetch_viewport [{start_index}, {end_index}] "
                f"out of [0, {self.record_count})",
            )
        await self._prefetch.prefetch_viewport_async(start_index, end_index)

    def warmup(self) -> None:
        """Block until eager or background prefetch work completes."""
        self._prefetch.warmup()

    def close(self) -> None:
        """Cancel in-flight eager prefetch and wait for the background thread."""
        self._prefetch.close()

    def cache_stats(self) -> dict[str, Any]:
        """Diagnostic cache and prefetch counters."""
        return self._prefetch.cache_stats()

    def _read_schema(self, offset: int) -> None:
        mv = self.mv
        key_count = _U16.unpack_from(mv, offset)[0]
        offset += 2
        self.key_sigils = list(mv[offset:offset + key_count])
        offset += key_count

        # Read null-terminated strings from the pool
        buf = bytes(mv[offset:])
        consumed = 0
        for _ in range(key_count):
            end = buf.index(0x00, consumed)
            self.keys.append(buf[consumed:end].decode("utf-8"))
            consumed = end + 1

        self.key_index = {k: i for i, k in enumerate(self.keys)}
        offset += consumed
        # Pad to 8-byte boundary
        self._schema_end = (offset + 7) & ~7

    def _read_tail_index(self) -> None:
        p = self.tail_ptr
        self.record_count = _U32.unpack_from(self.mv, p)[0]
        self._tail_start = p + 4

    def record(self, i: int) -> "NxsObject":
        """O(1) lookup: get the top-level object at index ``i``."""
        if i < 0 or i >= self.record_count:
            raise NxsError("ERR_OUT_OF_BOUNDS",
                           f"record {i} out of [0, {self.record_count})")
        self._prefetch.on_access(i)
        # Each entry: u16 keyId + u64 offset = 10 bytes
        entry = self._tail_start + i * 10
        abs_offset = _U64.unpack_from(self.mv, entry + 2)[0]
        return NxsObject(self, abs_offset)

    def records(self) -> Iterator["NxsObject"]:
        """Iterate all top-level records."""
        for i in range(self.record_count):
            yield self.record(i)


class NxsObject:
    """A lazy view over one NXS object. Fields are decoded on demand."""

    __slots__ = ("reader", "offset", "_parsed",
                 "_bitmask_bytes", "_offset_table_start", "length")

    def __init__(self, reader: NxsReader, offset: int) -> None:
        self.reader = reader
        self.offset = offset
        self._parsed = False

    def _parse_header(self) -> None:
        if self._parsed:
            return
        mv = self.reader.mv
        p = self.offset

        magic = _U32.unpack_from(mv, p)[0]
        if magic != MAGIC_OBJ:
            raise NxsError("ERR_BAD_MAGIC", f"expected NYXO at {p}")
        p += 4
        self.length = _U32.unpack_from(mv, p)[0]
        p += 4

        # LEB128 bitmask — read until high bit is 0
        bitmask: list[int] = []
        while True:
            b = mv[p]
            p += 1
            bitmask.append(b & 0x7F)
            if (b & 0x80) == 0:
                break

        self._bitmask_bytes = bitmask
        self._offset_table_start = p
        self._parsed = True

    def _field_offset(self, slot: int) -> Optional[int]:
        """Return the absolute byte offset of the field at ``slot``, or None."""
        self._parse_header()
        byte_idx, bit_idx = divmod(slot, 7)
        bitmask = self._bitmask_bytes
        if byte_idx >= len(bitmask):
            return None
        if not (bitmask[byte_idx] >> bit_idx) & 1:
            return None

        # Count present bits before this slot → position in offset table
        entry_idx = 0
        for s in range(slot):
            bi, bb = divmod(s, 7)
            if bi < len(bitmask) and (bitmask[bi] >> bb) & 1:
                entry_idx += 1

        rel = _U16.unpack_from(self.reader.mv,
                               self._offset_table_start + entry_idx * 2)[0]
        return self.offset + rel

    # ── Typed accessors ──────────────────────────────────────────────────────

    def get_i64(self, key: str) -> Optional[int]:
        slot = self.reader.key_index.get(key)
        if slot is None:
            return None
        off = self._field_offset(slot)
        if off is None:
            return None
        return _I64.unpack_from(self.reader.mv, off)[0]

    def get_f64(self, key: str) -> Optional[float]:
        slot = self.reader.key_index.get(key)
        if slot is None:
            return None
        off = self._field_offset(slot)
        if off is None:
            return None
        return _F64.unpack_from(self.reader.mv, off)[0]

    def get_bool(self, key: str) -> Optional[bool]:
        slot = self.reader.key_index.get(key)
        if slot is None:
            return None
        off = self._field_offset(slot)
        if off is None:
            return None
        return self.reader.mv[off] != 0

    def get_str(self, key: str) -> Optional[str]:
        slot = self.reader.key_index.get(key)
        if slot is None:
            return None
        off = self._field_offset(slot)
        if off is None:
            return None
        length = _U32.unpack_from(self.reader.mv, off)[0]
        return bytes(self.reader.mv[off + 4:off + 4 + length]).decode("utf-8")

    def get_time(self, key: str) -> Optional[int]:
        """Unix nanoseconds."""
        return self.get_i64(key)

    def to_dict(self) -> dict:
        """Decode all present fields using sigil type information."""
        self._parse_header()
        out = {}
        for key, slot in self.reader.key_index.items():
            bi, bb = divmod(slot, 7)
            if bi >= len(self._bitmask_bytes):
                continue
            if not (self._bitmask_bytes[bi] >> bb) & 1:
                continue
            sigil = (self.reader.key_sigils[slot]
                     if slot < len(self.reader.key_sigils) else SIGIL_INT)
            if sigil == SIGIL_STR:
                out[key] = self.get_str(key)
            elif sigil == SIGIL_FLOAT:
                out[key] = self.get_f64(key)
            elif sigil == SIGIL_BOOL:
                out[key] = self.get_bool(key)
            elif sigil == SIGIL_TIME:
                out[key] = self.get_time(key)
            else:  # SIGIL_INT and unknown
                out[key] = self.get_i64(key)
        return out


# ── Query engine ──────────────────────────────────────────────────────────────

class _Pred:
    """Base predicate. Subclasses implement __call__(record: NxsObject) -> bool."""

    def __and__(self, other: "_Pred") -> "_Pred":
        return _And(self, other)

    def __or__(self, other: "_Pred") -> "_Pred":
        return _Or(self, other)

    def __invert__(self) -> "_Pred":
        return _Not(self)


class Eq(_Pred):
    """Matches records where ``key == value``.

    Supported value types: ``str``, ``int``, ``float``, ``bool``.
    """

    def __init__(self, key: str, value) -> None:
        self.key = key
        self.value = value

    def __call__(self, obj: "NxsObject") -> bool:
        v = self.value
        if isinstance(v, bool):
            return obj.get_bool(self.key) == v
        if isinstance(v, str):
            return obj.get_str(self.key) == v
        if isinstance(v, float):
            got = obj.get_f64(self.key)
            return got is not None and got == v
        # int (checked last — bool subclasses int in Python)
        got = obj.get_i64(self.key)
        return got is not None and got == int(v)


class Gt(_Pred):
    """Matches records where ``key > value`` (numeric fields only)."""

    def __init__(self, key: str, value: float) -> None:
        self.key = key
        self.value = value

    def __call__(self, obj: "NxsObject") -> bool:
        got = obj.get_f64(self.key)
        if got is None:
            got_i = obj.get_i64(self.key)
            if got_i is None:
                return False
            return got_i > self.value
        return got > self.value


class Lt(_Pred):
    """Matches records where ``key < value`` (numeric fields only)."""

    def __init__(self, key: str, value: float) -> None:
        self.key = key
        self.value = value

    def __call__(self, obj: "NxsObject") -> bool:
        got = obj.get_f64(self.key)
        if got is None:
            got_i = obj.get_i64(self.key)
            if got_i is None:
                return False
            return got_i < self.value
        return got < self.value


class Gte(_Pred):
    """Matches records where ``key >= value`` (numeric fields only)."""

    def __init__(self, key: str, value: float) -> None:
        self.key = key
        self.value = value

    def __call__(self, obj: "NxsObject") -> bool:
        got = obj.get_f64(self.key)
        if got is None:
            got_i = obj.get_i64(self.key)
            if got_i is None:
                return False
            return got_i >= self.value
        return got >= self.value


class Lte(_Pred):
    """Matches records where ``key <= value`` (numeric fields only)."""

    def __init__(self, key: str, value: float) -> None:
        self.key = key
        self.value = value

    def __call__(self, obj: "NxsObject") -> bool:
        got = obj.get_f64(self.key)
        if got is None:
            got_i = obj.get_i64(self.key)
            if got_i is None:
                return False
            return got_i <= self.value
        return got <= self.value


class _And(_Pred):
    def __init__(self, *preds: "_Pred") -> None:
        self._preds = preds

    def __call__(self, obj: "NxsObject") -> bool:
        return all(p(obj) for p in self._preds)


class _Or(_Pred):
    def __init__(self, *preds: "_Pred") -> None:
        self._preds = preds

    def __call__(self, obj: "NxsObject") -> bool:
        return any(p(obj) for p in self._preds)


class _Not(_Pred):
    def __init__(self, pred: "_Pred") -> None:
        self._pred = pred

    def __call__(self, obj: "NxsObject") -> bool:
        return not self._pred(obj)


class Query:
    """Lazy filtered view over an :class:`NxsReader`.

    Typical usage::

        q = Query(reader, Eq("active", True) & Gt("score", 80.0))
        for rec in q:          # yields dict
            print(rec)
        print(q.count())
        print(q.first())

    Or via the convenience method::

        for rec in reader.where(Eq("active", True)):
            ...
    """

    def __init__(self, reader: "NxsReader", pred: Optional["_Pred"] = None) -> None:
        self._reader = reader
        self._pred = pred

    # ── iteration ────────────────────────────────────────────────────────────

    def _iter_objects(self) -> Iterator["NxsObject"]:
        """Yield :class:`NxsObject` instances matching the predicate."""
        pred = self._pred
        for obj in self._reader.records():
            if pred is None or pred(obj):
                yield obj

    def __iter__(self) -> Iterator[dict]:
        """Yield matching records as plain dicts (all fields decoded)."""
        for obj in self._iter_objects():
            yield obj.to_dict()

    # ── terminal operations ───────────────────────────────────────────────────

    def count(self) -> int:
        """Return the number of records matching the predicate."""
        return sum(1 for _ in self._iter_objects())

    def first(self) -> Optional[dict]:
        """Return the first matching record as a dict, or ``None``."""
        for obj in self._iter_objects():
            return obj.to_dict()
        return None


# Attach a convenience method to NxsReader so callers can write
# ``reader.where(Eq("active", True)).count()`` — mirrors Go's Reader.Where.
def _reader_where(self: NxsReader, pred: Optional[_Pred] = None) -> Query:
    """Return a :class:`Query` filtered by *pred* (``None`` = all records)."""
    return Query(self, pred)


NxsReader.where = _reader_where  # type: ignore[attr-defined]
