"""NXS Writer — direct-to-buffer .nxb emitter for Python.

Mirrors the Rust NxsWriter API:
    NxsSchema  — precompile keys once; share across NxsWriter instances.
    NxsWriter  — slot-based hot path; no per-field dict lookups during write.

Usage::

    from nxs_writer import NxsSchema, NxsWriter

    schema = NxsSchema(["id", "username", "score"])
    w = NxsWriter(schema)
    w.begin_object()
    w.write_i64(0, 42)
    w.write_str(1, "alice")
    w.write_f64(2, 9.5)
    w.end_object()
    data: bytes = w.finish()
"""
from __future__ import annotations

import struct
from typing import List

# ── Format constants ─────────────────────────────────────────────────────────

MAGIC_FILE    = 0x4E595842  # NYXB
MAGIC_OBJ     = 0x4E59584F  # NYXO
MAGIC_LIST    = 0x4E59584C  # NYXL
MAGIC_FOOTER  = 0x2153584E  # NXS!
VERSION       = 0x0101
FLAG_SCHEMA_EMBEDDED = 0x0002

_U8  = struct.Struct("<B")
_U16 = struct.Struct("<H")
_U32 = struct.Struct("<I")
_U64 = struct.Struct("<Q")
_I64 = struct.Struct("<q")
_F64 = struct.Struct("<d")

# ── MurmurHash3-64 ────────────────────────────────────────────────────────────

def _murmur3_64(data: bytes) -> int:
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

# ── Schema ────────────────────────────────────────────────────────────────────

class NxsSchema:
    """Precompiled schema: maps key names to slot indices."""

    __slots__ = ("keys", "bitmask_bytes")

    def __init__(self, keys: List[str]) -> None:
        self.keys: List[str] = list(keys)
        # LEB128 bitmask byte count: (len + 6) // 7, minimum 1
        self.bitmask_bytes: int = max(1, (len(keys) + 6) // 7)

    def __len__(self) -> int:
        return len(self.keys)


# ── Frame (per open object) ───────────────────────────────────────────────────

class _Frame:
    __slots__ = ("start", "bitmask", "offset_table", "slot_offsets",
                 "last_slot", "needs_sort")

    def __init__(self, start: int, bitmask: bytearray) -> None:
        self.start = start
        self.bitmask = bitmask
        self.offset_table: List[int] = []   # relative offsets (from object start)
        self.slot_offsets: List[tuple] = [] # (slot, buf_off)
        self.last_slot = -1
        self.needs_sort = False


# ── Writer ────────────────────────────────────────────────────────────────────

class NxsWriter:
    """Slot-based .nxb emitter.

    Call ``begin_object()`` / typed write methods / ``end_object()`` for each
    record; call ``finish()`` to get the complete file bytes.
    """

    __slots__ = ("schema", "_buf", "_frames", "_record_offsets")

    def __init__(self, schema: NxsSchema) -> None:
        self.schema = schema
        self._buf = bytearray()
        self._frames: List[_Frame] = []
        self._record_offsets: List[int] = []

    # ── Object lifetime ──────────────────────────────────────────────────────

    def begin_object(self) -> None:
        """Open an object.  Must be balanced with end_object()."""
        schema = self.schema

        if not self._frames:
            self._record_offsets.append(len(self._buf))

        start = len(self._buf)

        # Build bitmask with LEB128 continuation bits pre-set
        bm = bytearray(schema.bitmask_bytes)
        for i in range(schema.bitmask_bytes - 1):
            bm[i] = 0x80  # continuation bit set; data bits clear

        self._frames.append(_Frame(start, bm))

        # Magic (4) + length placeholder (4)
        self._buf += _U32.pack(MAGIC_OBJ)
        self._buf += _U32.pack(0)  # back-patched in end_object

        # Reserve bitmask bytes
        self._buf += bytes(bm)

        # Reserve offset table: u16 per schema key (upper bound)
        self._buf += bytes(len(schema) * 2)

        # Align data area to 8 bytes from object start
        while (len(self._buf) - start) % 8 != 0:
            self._buf += b'\x00'

    def end_object(self) -> None:
        """Close the current object and back-patch the header."""
        if not self._frames:
            raise RuntimeError("end_object without begin_object")
        frame = self._frames.pop()

        total_len = len(self._buf) - frame.start

        # Back-patch Length at frame.start + 4
        _U32.pack_into(self._buf, frame.start + 4, total_len)

        # Back-patch bitmask at frame.start + 8
        bm_off = frame.start + 8
        for i, b in enumerate(frame.bitmask):
            self._buf[bm_off + i] = b

        # Back-patch offset table at frame.start + 8 + bitmask_bytes
        ot_start = bm_off + self.schema.bitmask_bytes
        present_count = len(frame.offset_table)

        if not frame.needs_sort:
            for i, rel in enumerate(frame.offset_table):
                _U16.pack_into(self._buf, ot_start + i * 2, rel)
        else:
            pairs = sorted(frame.slot_offsets, key=lambda x: x[0])
            for i, (_, buf_off) in enumerate(pairs):
                rel = buf_off - frame.start
                _U16.pack_into(self._buf, ot_start + i * 2, rel)

        # Zero unused offset-table slots
        used = present_count * 2
        reserved = len(self.schema) * 2
        for i in range(used, reserved):
            self._buf[ot_start + i] = 0

    def finish(self) -> bytes:
        """Assemble and return the complete .nxb file bytes."""
        if self._frames:
            raise RuntimeError("unclosed objects")

        schema_bytes = _build_schema(self.schema.keys)
        dict_hash    = _murmur3_64(schema_bytes)
        data_start_abs = 32 + len(schema_bytes)

        data_sector = bytes(self._buf)

        tail_ptr = data_start_abs + len(data_sector)
        tail    = _build_tail_index_records(data_start_abs, self._record_offsets, tail_ptr)

        out = bytearray()

        # Preamble (32 bytes)
        out += _U32.pack(MAGIC_FILE)
        out += _U16.pack(VERSION)
        out += _U16.pack(FLAG_SCHEMA_EMBEDDED)
        out += _U64.pack(dict_hash)
        out += _U64.pack(0)
        out += bytes(8)  # reserved

        out += schema_bytes
        out += data_sector
        out += tail

        return bytes(out)

    # ── Typed write methods ──────────────────────────────────────────────────

    def write_i64(self, slot: int, v: int) -> None:
        self._mark_slot(slot)
        self._buf += _I64.pack(v)

    def write_f64(self, slot: int, v: float) -> None:
        self._mark_slot(slot)
        self._buf += _F64.pack(v)

    def write_bool(self, slot: int, v: bool) -> None:
        self._mark_slot(slot)
        self._buf += bytes([0x01 if v else 0x00])
        self._buf += bytes(7)  # 7 padding bytes

    def write_time(self, slot: int, unix_ns: int) -> None:
        self._mark_slot(slot)
        self._buf += _I64.pack(unix_ns)

    def write_null(self, slot: int) -> None:
        self._mark_slot(slot)
        self._buf += bytes(8)

    def write_str(self, slot: int, v: str) -> None:
        self._mark_slot(slot)
        b = v.encode("utf-8")
        self._buf += _U32.pack(len(b))
        self._buf += b
        # pad to 8
        used = (4 + len(b)) % 8
        if used:
            self._buf += bytes(8 - used)

    def write_bytes(self, slot: int, data: bytes) -> None:
        self._mark_slot(slot)
        self._buf += _U32.pack(len(data))
        self._buf += data
        used = (4 + len(data)) % 8
        if used:
            self._buf += bytes(8 - used)

    def write_list_i64(self, slot: int, values: List[int]) -> None:
        self._mark_slot(slot)
        total = 16 + len(values) * 8
        self._buf += _U32.pack(MAGIC_LIST)
        self._buf += _U32.pack(total)
        self._buf += bytes([0x3D])  # SIGIL_INT '='
        self._buf += _U32.pack(len(values))
        self._buf += bytes(3)  # padding
        for v in values:
            self._buf += _I64.pack(v)

    def write_list_f64(self, slot: int, values: List[float]) -> None:
        self._mark_slot(slot)
        total = 16 + len(values) * 8
        self._buf += _U32.pack(MAGIC_LIST)
        self._buf += _U32.pack(total)
        self._buf += bytes([0x7E])  # SIGIL_FLOAT '~'
        self._buf += _U32.pack(len(values))
        self._buf += bytes(3)  # padding
        for v in values:
            self._buf += _F64.pack(v)

    # ── Convenience ──────────────────────────────────────────────────────────

    @staticmethod
    def from_records(keys: List[str], records: List[dict]) -> bytes:
        """Write records from plain dicts.  Returns complete .nxb bytes."""
        schema = NxsSchema(keys)
        w = NxsWriter(schema)
        for rec in records:
            w.begin_object()
            for i, key in enumerate(keys):
                if key not in rec:
                    continue
                val = rec[key]
                if val is None:
                    w.write_null(i)
                elif isinstance(val, bool):
                    w.write_bool(i, val)
                elif isinstance(val, int):
                    w.write_i64(i, val)
                elif isinstance(val, float):
                    w.write_f64(i, val)
                elif isinstance(val, str):
                    w.write_str(i, val)
                elif isinstance(val, (bytes, bytearray)):
                    w.write_bytes(i, bytes(val))
            w.end_object()
        return w.finish()

    # ── Private ──────────────────────────────────────────────────────────────

    def _mark_slot(self, slot: int) -> None:
        if not self._frames:
            raise RuntimeError("no active object")
        frame = self._frames[-1]

        # Set bitmask bit
        byte_idx = slot // 7
        bit_idx  = slot % 7
        frame.bitmask[byte_idx] |= (1 << bit_idx)

        # Record relative offset from object start
        rel = len(self._buf) - frame.start

        if slot < frame.last_slot:
            frame.needs_sort = True
        frame.last_slot = slot

        frame.offset_table.append(rel)
        frame.slot_offsets.append((slot, len(self._buf)))


# ── Module-level helpers ──────────────────────────────────────────────────────

def _build_schema(keys: List[str]) -> bytes:
    """Build the binary Schema Header bytes (padded to 8-byte boundary)."""
    encoded = [k.encode("utf-8") for k in keys]
    n = len(keys)

    # KeyCount(2) + TypeManifest(n) + null-terminated strings
    size = 2 + n + sum(len(e) + 1 for e in encoded)
    padded = size + ((-size) % 8)

    buf = bytearray(padded)
    p = 0
    _U16.pack_into(buf, p, n); p += 2
    for _ in keys:
        buf[p] = 0x22; p += 1  # '"' sigil
    for e in encoded:
        buf[p:p + len(e)] = e; p += len(e)
        buf[p] = 0x00;         p += 1
    # Remaining bytes already zero (bytearray init)
    return bytes(buf)


def _build_tail_index_records(data_start: int, record_offsets: List[int], tail_ptr: int) -> bytes:
    """Build the Tail-Index for all records."""
    n = len(record_offsets)
    # EntryCount(4) + N * [KeyID(2) + AbsOff(8)] + FooterTailPtr(8) + MagicFooter(4)
    buf = bytearray(4 + n * 10 + 12)
    p = 0

    _U32.pack_into(buf, p, n); p += 4

    for i, rel_off in enumerate(record_offsets):
        _U16.pack_into(buf, p, i); p += 2          # KeyID = record index
        _U64.pack_into(buf, p, data_start + rel_off); p += 8

    _U64.pack_into(buf, p, tail_ptr); p += 8

    # MagicFooter
    _U32.pack_into(buf, p, MAGIC_FOOTER)

    return bytes(buf)
