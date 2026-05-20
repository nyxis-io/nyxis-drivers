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

import struct
from typing import Iterator, Optional, Union


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
    )

    def __init__(self, buffer: Union[bytes, bytearray, memoryview]) -> None:
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
