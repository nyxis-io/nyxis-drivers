// NXS Reader — zero-copy .nxb parser for C# (.NET 8)
// Implements the Nyxis v1.1 binary wire format spec.
//
// Usage:
//   var data = File.ReadAllBytes("data.nxb");
//   var reader = new NxsReader(data);
//   var obj = reader.Record(42);
//   long id = obj.GetI64("id");
using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Nxs;

// ── Exceptions ────────────────────────────────────────────────────────────────

public sealed class NxsException(string code, string msg)
    : Exception($"{code}: {msg}")
{
    public string Code { get; } = code;
}

// ── Reader ────────────────────────────────────────────────────────────────────

public sealed class NxsReader
{
    private const uint MagicFile = 0x4E595842u;
    private const uint MagicObj = 0x4E59584Fu;
    private const uint MagicList = 0x4E59584Cu;
    private const uint MagicFooter = 0x2153584Eu;
    private const ushort FlagSchema = 0x0002;

    private readonly byte[] _data;

    public ushort Version { get; }
    public ushort Flags { get; }
    public ulong DictHash { get; }
    public ulong TailPtr { get; }
    public string[] Keys { get; }
    public byte[] KeySigils { get; }

    private readonly Dictionary<string, int> _keyIndex;
    public int RecordCount { get; }
    private readonly int _tailStart;

    public NxsReader(byte[] data)
    {
        _data = data;
        int size = data.Length;
        if (size < 32) throw new NxsException("ERR_OUT_OF_BOUNDS", "file too small");
        if (RdU32(0) != MagicFile) throw new NxsException("ERR_BAD_MAGIC", "preamble");
        if (RdU32(size - 4) != MagicFooter) throw new NxsException("ERR_BAD_MAGIC", "footer");

        Version = RdU16(4);
        Flags = RdU16(6);
        DictHash = RdU64(8);
        TailPtr = RdU64(16);
        if (TailPtr == 0)
        {
            if (size < 44) throw new NxsException("ERR_OUT_OF_BOUNDS", "stream footer missing tail pointer");
            TailPtr = RdU64(size - 12);
        }

        var ks = Array.Empty<string>();
        var kSig = Array.Empty<byte>();
        _keyIndex = new Dictionary<string, int>();

        if ((Flags & FlagSchema) != 0)
        {
            int off = 32;
            int keyCount = RdU16(off); off += 2;
            kSig = new byte[keyCount];
            Array.Copy(data, off, kSig, 0, keyCount); off += keyCount;
            ks = new string[keyCount];
            for (int i = 0; i < keyCount; i++)
            {
                int start = off;
                while (off < size && data[off] != 0) off++;
                ks[i] = Encoding.UTF8.GetString(data, start, off - start);
                _keyIndex[ks[i]] = i;
                off++; // skip NUL
            }
            // Pad to 8-byte boundary to find schema end
            if (off % 8 != 0) off += 8 - (off % 8);
            int schemaEnd = off;
            ulong computed = Murmur3_64(data, 32, schemaEnd - 32);
            if (computed != DictHash)
                throw new NxsException("ERR_DICT_MISMATCH", "schema hash mismatch");
        }

        Keys = ks;
        KeySigils = kSig;

        int tp = (int)TailPtr;
        if (tp + 4 > size) throw new NxsException("ERR_OUT_OF_BOUNDS", "tail index");
        RecordCount = (int)RdU32(tp);
        _tailStart = tp + 4;
    }

    public int Slot(string key)
    {
        if (_keyIndex.TryGetValue(key, out int s)) return s;
        throw new NxsException("ERR_KEY_NOT_FOUND", key);
    }

    public NxsObject Record(int i)
    {
        if ((uint)i >= (uint)RecordCount)
            throw new NxsException("ERR_OUT_OF_BOUNDS", $"record {i} out of [0, {RecordCount})");
        int entryOff = _tailStart + i * 10 + 2;
        int absOff = (int)RdU64(entryOff);
        return new NxsObject(this, absOff);
    }

    // ── Bulk reducers ─────────────────────────────────────────────────────────

    public double SumF64(string key)
    {
        int s = Slot(key);
        double sum = 0;
        for (int i = 0; i < RecordCount; i++)
        {
            int abs = (int)RdU64(_tailStart + i * 10 + 2);
            int off = ScanOffset(abs, s);
            if (off >= 0) sum += RdF64(off);
        }
        return sum;
    }

    public long SumI64(string key)
    {
        int s = Slot(key);
        long sum = 0;
        for (int i = 0; i < RecordCount; i++)
        {
            int abs = (int)RdU64(_tailStart + i * 10 + 2);
            int off = ScanOffset(abs, s);
            if (off >= 0) sum += RdI64(off);
        }
        return sum;
    }

    public double? MinF64(string key)
    {
        int s = Slot(key);
        double? m = null;
        for (int i = 0; i < RecordCount; i++)
        {
            int abs = (int)RdU64(_tailStart + i * 10 + 2);
            int off = ScanOffset(abs, s);
            if (off < 0) continue;
            double v = RdF64(off);
            m = m is null ? v : Math.Min(m.Value, v);
        }
        return m;
    }

    public double? MaxF64(string key)
    {
        int s = Slot(key);
        double? m = null;
        for (int i = 0; i < RecordCount; i++)
        {
            int abs = (int)RdU64(_tailStart + i * 10 + 2);
            int off = ScanOffset(abs, s);
            if (off < 0) continue;
            double v = RdF64(off);
            m = m is null ? v : Math.Max(m.Value, v);
        }
        return m;
    }

    // ── Internal ──────────────────────────────────────────────────────────────

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal ushort RdU16(int off) =>
        (ushort)(_data[off] | (_data[off + 1] << 8));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal uint RdU32(int off) =>
        (uint)(_data[off] | (_data[off + 1] << 8) | (_data[off + 2] << 16) | (_data[off + 3] << 24));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal ulong RdU64(int off)
    {
        ulong lo = RdU32(off);
        ulong hi = RdU32(off + 4);
        return lo | (hi << 32);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal long RdI64(int off) => (long)RdU64(off);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal unsafe double RdF64(int off)
    {
        ulong bits = RdU64(off);
        return *(double*)&bits;
    }

    internal string RdStr(int off)
    {
        int len = (int)RdU32(off);
        return Encoding.UTF8.GetString(_data, off + 4, len);
    }

    internal int DataSize => _data.Length;
    internal byte DataAt(int off) => _data[off];
    internal Dictionary<string, int> KeyIndex => _keyIndex;

    // ── DictHash validation ───────────────────────────────────────────────────

    private static ulong Murmur3_64(byte[] data, int offset, int length)
    {
        const ulong C1 = 0xFF51AFD7ED558CCDUL;
        const ulong C2 = 0xC4CEB9FE1A85EC53UL;
        ulong h = 0x93681D6255313A99UL;
        int p = offset, end = offset + length;
        while (p < end)
        {
            ulong k = 0;
            for (int i = 0; i < 8 && p + i < end; i++)
                k |= (ulong)data[p + i] << (i * 8);
            k *= C1; k ^= k >> 33;
            h ^= k; h *= C2; h ^= h >> 33;
            p += 8;
        }
        h ^= (ulong)length; h ^= h >> 33;
        h *= C1; h ^= h >> 33;
        return h;
    }

    // Locate slot's value offset inside object at objOffset, or -1.
    internal int ScanOffset(int objOffset, int slot)
    {
        int p = objOffset + 8;
        int cur = 0, tableIdx = 0;
        int b = 0;
        while (true)
        {
            if (p >= _data.Length) return -1;
            b = _data[p++]; int bits = b & 0x7F;
            for (int i = 0; i < 7; i++)
            {
                if (cur == slot)
                {
                    if (((bits >> i) & 1) == 0) return -1;
                    while ((b & 0x80) != 0) { b = _data[p++]; }
                    int ot = p + tableIdx * 2;
                    if (ot + 2 > _data.Length) return -1;
                    return objOffset + RdU16(ot);
                }
                if (cur < slot && ((bits >> i) & 1) == 1) tableIdx++;
                cur++;
            }
            if ((b & 0x80) == 0) return -1;
        }
    }
}

// ── Object ────────────────────────────────────────────────────────────────────

public sealed class NxsObject
{
    private const uint MagicObj = 0x4E59584Fu;

    private readonly NxsReader _reader;
    private readonly int _offset;
    private bool _staged;
    private int _bitmaskStart;
    private int _offsetTableStart;

    internal NxsObject(NxsReader reader, int offset)
    {
        _reader = reader;
        _offset = offset;
    }

    private void LocateBitmask()
    {
        if (_staged) return;
        if (_offset + 8 > _reader.DataSize)
            throw new NxsException("ERR_OUT_OF_BOUNDS", "object header");
        if (_reader.RdU32(_offset) != MagicObj)
            throw new NxsException("ERR_BAD_MAGIC", $"object at {_offset}");
        int p = _offset + 8;
        _bitmaskStart = p;
        while (p < _reader.DataSize && (_reader.DataAt(p) & 0x80) != 0) p++;
        if (p >= _reader.DataSize)
            throw new NxsException("ERR_OUT_OF_BOUNDS", "bitmask");
        p++;
        _offsetTableStart = p;
        _staged = true;
    }

    private int ResolveSlot(int slot)
    {
        LocateBitmask();
        int p = _bitmaskStart, cur = 0, tableIdx = 0, b = 0;
        while (true)
        {
            if (p >= _reader.DataSize) return -1;
            b = _reader.DataAt(p++); int bits = b & 0x7F;
            for (int i = 0; i < 7; i++)
            {
                if (cur == slot)
                {
                    if (((bits >> i) & 1) == 0) return -1;
                    while ((b & 0x80) != 0) { b = _reader.DataAt(p++); }
                    int ot = _offsetTableStart + tableIdx * 2;
                    if (ot + 2 > _reader.DataSize) return -1;
                    return _offset + _reader.RdU16(ot);
                }
                if (cur < slot && ((bits >> i) & 1) == 1) tableIdx++;
                cur++;
            }
            if ((b & 0x80) == 0) return -1;
        }
    }

    /// <summary>Returns true if the field exists and is present in this record.</summary>
    public bool HasField(string key)
    {
        if (!_reader.KeyIndex.TryGetValue(key, out int slot)) return false;
        return ResolveSlot(slot) >= 0;
    }

    public long GetI64(string key) => GetI64BySlot(_reader.Slot(key));
    public double GetF64(string key) => GetF64BySlot(_reader.Slot(key));
    public bool GetBool(string key) => GetBoolBySlot(_reader.Slot(key));
    public string GetStr(string key) => GetStrBySlot(_reader.Slot(key));

    public long GetI64BySlot(int slot)
    {
        int off = ResolveSlot(slot);
        if (off < 0) throw new NxsException("ERR_FIELD_ABSENT", $"slot {slot}");
        return _reader.RdI64(off);
    }
    public double GetF64BySlot(int slot)
    {
        int off = ResolveSlot(slot);
        if (off < 0) throw new NxsException("ERR_FIELD_ABSENT", $"slot {slot}");
        return _reader.RdF64(off);
    }
    public bool GetBoolBySlot(int slot)
    {
        int off = ResolveSlot(slot);
        if (off < 0) throw new NxsException("ERR_FIELD_ABSENT", $"slot {slot}");
        return _reader.DataAt(off) != 0;
    }
    public string GetStrBySlot(int slot)
    {
        int off = ResolveSlot(slot);
        if (off < 0) throw new NxsException("ERR_FIELD_ABSENT", $"slot {slot}");
        return _reader.RdStr(off);
    }
}
