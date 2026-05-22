// NXS Reader — zero-copy .nxb parser for C# (.NET 8)
// Implements Nyxis v1.1 row layout plus OLAP columnar and PAX (OLAP.md).
//
// Usage:
//   var data = File.ReadAllBytes("data.nxb");
//   var reader = new NxsReader(data);
//   var obj = reader.Record(42);
//   long id = obj.GetI64("id");
using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Text;

namespace Nxs;

// ── Layout & buffers ─────────────────────────────────────────────────────────

public enum Layout
{
    Row,
    Columnar,
    PAX,
}

public readonly struct ColVarBuffer
{
    public byte[] Bitmap { get; init; }
    public byte[] Offsets { get; init; }
    public byte[] Values { get; init; }
    public uint Count { get; init; }
}

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
    private const uint MagicPage = 0x4E585350u;
    private const ushort FlagColumnar = 0x0001;
    private const ushort FlagPAX = 0x0004;
    private const ushort FlagSchema = 0x0002;

    private const int FooterRowBytes = 12;
    private const int FooterColBytes = 20;
    private const int FooterPaxBytes = 28;
    private const int ColTailEntryBytes = 20;
    private const int PaxTailEntryBytes = 28;

    private readonly byte[] _data;

    public ushort Version { get; }
    public ushort Flags { get; }
    public ulong DictHash { get; }
    public ulong TailPtr { get; private set; }
    public string[] Keys { get; }
    public byte[] KeySigils { get; }
    public Layout LayoutKind { get; }

    private readonly Dictionary<string, int> _keyIndex;
    private uint _recordCount;
    private readonly int _tailStart;

    private readonly ulong[] _colBufOff;
    private readonly ulong[] _colBufLen;

    private uint _pageCount;
    private uint _pageSizeHint;
    private uint[] _pageIndex = Array.Empty<uint>();
    private ulong[] _pageRecStart = Array.Empty<ulong>();
    private uint[] _pageRecCount = Array.Empty<uint>();
    private ulong[] _pageOffset = Array.Empty<ulong>();
    private uint[] _pageLength = Array.Empty<uint>();

    public int RecordCount
    {
        get
        {
            if (_recordCount > int.MaxValue)
                throw new NxsException("ERR_OUT_OF_BOUNDS", "record_count exceeds int.MaxValue");
            return (int)_recordCount;
        }
    }

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
        ulong preambleTail = RdU64(16);
        TailPtr = preambleTail;

        if ((Flags & FlagColumnar) != 0 && (Flags & FlagPAX) != 0)
            throw new NxsException("ERR_INVALID_FLAGS", "columnar and PAX both set");
        if ((Flags & FlagColumnar) != 0 && preambleTail == 0)
            throw new NxsException("ERR_INCOMPATIBLE_FLAGS", "columnar with TailPtr=0");

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
                off++;
            }
            if (off % 8 != 0) off += 8 - (off % 8);
            ulong computed = Murmur3_64(data, 32, off - 32);
            if (computed != DictHash)
                throw new NxsException("ERR_DICT_MISMATCH", "schema hash mismatch");
        }

        Keys = ks;
        KeySigils = kSig;

        int kc = ks.Length;
        _colBufOff = new ulong[kc];
        _colBufLen = new ulong[kc];

        if ((Flags & FlagColumnar) != 0)
        {
            LayoutKind = Layout.Columnar;
            ParseColumnarFooter(size);
            _tailStart = (int)TailPtr;
        }
        else if ((Flags & FlagPAX) != 0)
        {
            LayoutKind = Layout.PAX;
            var pax = ParsePaxFooter(size);
            _pageCount = pax.pageCount;
            _pageSizeHint = pax.pageSizeHint;
            _pageIndex = pax.pageIndex;
            _pageRecStart = pax.pageRecStart;
            _pageRecCount = pax.pageRecCount;
            _pageOffset = pax.pageOffset;
            _pageLength = pax.pageLength;
            _tailStart = (int)TailPtr;
        }
        else
        {
            LayoutKind = Layout.Row;
            if (TailPtr == 0)
            {
                if (size < 44) throw new NxsException("ERR_OUT_OF_BOUNDS", "stream footer missing tail pointer");
                TailPtr = RdU64(size - FooterRowBytes);
            }
            int tp = (int)TailPtr;
            if (tp + 4 > size) throw new NxsException("ERR_OUT_OF_BOUNDS", "tail index");
            _recordCount = RdU32(tp);
            _tailStart = tp + 4;
        }
    }

    private void ParseColumnarFooter(int size)
    {
        if (size < FooterColBytes) throw new NxsException("ERR_OUT_OF_BOUNDS", "columnar footer");
        int fo = size - FooterColBytes;
        TailPtr = RdU64(fo);
        _recordCount = RecordCountFromFooter(RdU64(fo + 8));
        int kc = Keys.Length;
        int tail = (int)TailPtr;
        for (int i = 0; i < kc; i++)
        {
            int e = tail + i * ColTailEntryBytes;
            if (e + ColTailEntryBytes > size) throw new NxsException("ERR_OUT_OF_BOUNDS", "columnar tail entry");
            int fid = RdU16(e);
            if (fid >= kc) throw new NxsException("ERR_OUT_OF_BOUNDS", $"invalid field ID {fid}");
            _colBufOff[fid] = RdU64(e + 4);
            _colBufLen[fid] = RdU64(e + 12);
        }
    }

    private (uint pageCount, uint pageSizeHint, uint[] pageIndex, ulong[] pageRecStart, uint[] pageRecCount,
        ulong[] pageOffset, uint[] pageLength) ParsePaxFooter(int size)
    {
        if (size < FooterPaxBytes) throw new NxsException("ERR_OUT_OF_BOUNDS", "PAX footer");
        int fo = size - FooterPaxBytes;
        TailPtr = RdU64(fo);
        _recordCount = RecordCountFromFooter(RdU64(fo + 8));
        uint pageCount = RdU32(fo + 16);
        uint pageSizeHint = RdU32(fo + 20);
        int tail = (int)TailPtr;

        var pageIndex = Array.Empty<uint>();
        var pageRecStart = Array.Empty<ulong>();
        var pageRecCount = Array.Empty<uint>();
        var pageOffset = Array.Empty<ulong>();
        var pageLength = Array.Empty<uint>();

        if (pageCount > 0)
        {
            pageIndex = new uint[pageCount];
            pageRecStart = new ulong[pageCount];
            pageRecCount = new uint[pageCount];
            pageOffset = new ulong[pageCount];
            pageLength = new uint[pageCount];
            for (uint i = 0; i < pageCount; i++)
            {
                int e = tail + (int)i * PaxTailEntryBytes;
                if (e + PaxTailEntryBytes > size) throw new NxsException("ERR_OUT_OF_BOUNDS", "PAX tail entry");
                pageIndex[i] = RdU32(e);
                pageRecStart[i] = RdU64(e + 4);
                pageRecCount[i] = RdU32(e + 12);
                pageOffset[i] = RdU64(e + 16);
                pageLength[i] = RdU32(e + 24);
            }
            for (uint i = 0; i < pageCount; i++)
            {
                ulong poff64 = pageOffset[i];
                if (poff64 > (ulong)size || poff64 + 4 > (ulong)size || poff64 > int.MaxValue)
                    throw new NxsException("ERR_OUT_OF_BOUNDS", "PAX page offset");
                if (RdU32((int)poff64) != MagicPage)
                    throw new NxsException("ERR_INVALID_PAGE_MAGIC", "PAX page magic mismatch");
            }
        }

        return (pageCount, pageSizeHint, pageIndex, pageRecStart, pageRecCount, pageOffset, pageLength);
    }

    public int Slot(string key)
    {
        if (_keyIndex.TryGetValue(key, out int s)) return s;
        throw new NxsException("ERR_KEY_NOT_FOUND", key);
    }

    public NxsObject Record(int i)
    {
        if ((uint)i >= _recordCount)
            throw new NxsException("ERR_OUT_OF_BOUNDS", $"record {i} out of [0, {_recordCount})");
        if (LayoutKind != Layout.Row)
            return new NxsObject(this, i, (uint)i);
        int entryOff = _tailStart + i * 10 + 2;
        int absOff = (int)RdU64(entryOff);
        return new NxsObject(this, absOff, (uint)i);
    }

    // ── Columnar / PAX API ────────────────────────────────────────────────────

    public double ColSumF64(string key)
    {
        int slot = Slot(key);
        if (LayoutKind == Layout.Row)
            return SumF64(key);
        if (LayoutKind == Layout.PAX)
            return PaxSumF64(slot);
        var (bm, vals) = ColFieldParts(slot);
        double sum = 0;
        for (int i = 0; i < (int)_recordCount; i++)
        {
            if (!ColBit(bm, (uint)i)) continue;
            int off = i * 8;
            if (off + 8 > vals.Length) break;
            sum += RdF64(vals, off);
        }
        return sum;
    }

    public byte[]? ColBuffer(string key)
    {
        if (LayoutKind == Layout.Row) return null;
        int slot = Slot(key);
        if (IsVarSigil(KeySigils[slot])) return null;
        var (_, vals) = ColFieldParts(slot);
        return vals;
    }

    public byte[]? ColNullBitmap(string key)
    {
        if (LayoutKind == Layout.Row) return null;
        int slot = Slot(key);
        var (bm, _) = ColFieldParts(slot);
        return bm;
    }

    public ColVarBuffer ColVarBuffer(string key)
    {
        if (LayoutKind != Layout.Columnar)
            throw new NxsException("ERR_LAYOUT", "ColVarBuffer is columnar-only (use per-record GetStr on PAX)");
        int slot = Slot(key);
        if (!IsVarSigil(KeySigils[slot]))
            throw new NxsException("ERR_UNSUPPORTED_FIELD_TYPE", key);
        var (bm, offsets, values) = ColVarParts(slot);
        return new ColVarBuffer
        {
            Bitmap = bm,
            Offsets = offsets,
            Values = values,
            Count = _recordCount,
        };
    }

    internal string? ColGetStr(string key, uint recordIndex)
    {
        int slot = Slot(key);
        if (recordIndex >= _recordCount || KeySigils[slot] != (byte)'"')
            return null;
        if (!ColVarPartsAt(recordIndex, slot, out var bm, out var offsets, out var values, out uint bitIdx))
            return null;
        if (!ColBit(bm, bitIdx)) return null;
        return VarStrAt(offsets, values, bitIdx);
    }

    // ── Bulk reducers (row or layout-aware) ───────────────────────────────────

    public double SumF64(string key)
    {
        if (LayoutKind != Layout.Row)
            return ColSumF64(key);
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
        if (LayoutKind != Layout.Row)
            return ColSumI64(key);
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
        if (LayoutKind != Layout.Row)
            return ColMinF64(key);
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
        if (LayoutKind != Layout.Row)
            return ColMaxF64(key);
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

    private static uint RecordCountFromFooter(ulong rc)
    {
        if (rc > uint.MaxValue)
            throw new NxsException("ERR_OUT_OF_BOUNDS", "record_count overflow");
        return (uint)rc;
    }

    private long ColSumI64(int slot)
    {
        var (bm, vals) = ColFieldParts(slot);
        long sum = 0;
        for (uint i = 0; i < _recordCount; i++)
        {
            if (!ColBit(bm, i)) continue;
            int off = (int)i * 8;
            if (off + 8 > vals.Length) break;
            sum += RdI64(vals, off);
        }
        return sum;
    }

    public long ColSumI64(string key) => ColSumI64(Slot(key));

    private double? ColMinF64(int slot)
    {
        var (bm, vals) = ColFieldParts(slot);
        double? m = null;
        for (uint i = 0; i < _recordCount; i++)
        {
            if (!ColBit(bm, i)) continue;
            int off = (int)i * 8;
            if (off + 8 > vals.Length) break;
            double v = RdF64(vals, off);
            m = m is null ? v : Math.Min(m.Value, v);
        }
        return m;
    }

    private double? ColMaxF64(int slot)
    {
        var (bm, vals) = ColFieldParts(slot);
        double? m = null;
        for (uint i = 0; i < _recordCount; i++)
        {
            if (!ColBit(bm, i)) continue;
            int off = (int)i * 8;
            if (off + 8 > vals.Length) break;
            double v = RdF64(vals, off);
            m = m is null ? v : Math.Max(m.Value, v);
        }
        return m;
    }

    public double? ColMinF64(string key) => ColMinF64(Slot(key));

    public double? ColMaxF64(string key) => ColMaxF64(Slot(key));

    // ── Internal columnar/PAX ─────────────────────────────────────────────────

    private static int NullBitmapBytes(uint n)
    {
        int raw = (int)((n + 7) / 8);
        return (raw + 7) & ~7;
    }

    private static bool IsVarSigil(byte sig) => sig == (byte)'"' || sig == (byte)'<';

    private static bool ColBit(byte[] bm, uint rec) =>
        ((bm[rec / 8] >> (int)(rec % 8)) & 1) == 1;

    private (byte[] bm, byte[] vals) ColFieldParts(int slot)
    {
        if (slot < 0 || slot >= _colBufOff.Length)
            throw new NxsException("ERR_KEY_NOT_FOUND", $"slot {slot}");
        int off = (int)_colBufOff[slot];
        int length = (int)_colBufLen[slot];
        if (off + length > _data.Length)
            throw new NxsException("ERR_OUT_OF_BOUNDS", "column buffer");
        int bmLen = NullBitmapBytes(_recordCount);
        if (length < bmLen)
            throw new NxsException("ERR_OUT_OF_BOUNDS", "null bitmap");
        var bm = new byte[bmLen];
        Array.Copy(_data, off, bm, 0, bmLen);
        var vals = new byte[length - bmLen];
        Array.Copy(_data, off + bmLen, vals, 0, vals.Length);
        return (bm, vals);
    }

    private (byte[] bm, byte[] offsets, byte[] values) ColVarParts(int slot)
    {
        var (bm, tail) = ColFieldParts(slot);
        int offBytes = VarOffBytesLen(_recordCount);
        if (tail.Length < offBytes)
            throw new NxsException("ERR_OUT_OF_BOUNDS", "var offsets");
        var offsets = new byte[offBytes];
        Array.Copy(tail, 0, offsets, 0, offBytes);
        var values = new byte[tail.Length - offBytes];
        Array.Copy(tail, offBytes, values, 0, values.Length);
        return (bm, offsets, values);
    }

    private bool ColVarPartsAt(uint rec, int slot, out byte[] bm, out byte[] offsets, out byte[] values, out uint bitIdx)
    {
        bm = Array.Empty<byte>();
        offsets = Array.Empty<byte>();
        values = Array.Empty<byte>();
        bitIdx = rec;
        if (slot < 0 || slot >= KeySigils.Length || !IsVarSigil(KeySigils[slot]))
            return false;

        if (LayoutKind == Layout.Columnar)
        {
            (bm, offsets, values) = ColVarParts(slot);
            return true;
        }

        if (LayoutKind == Layout.PAX)
        {
            if (!PaxFindPage(rec, out int pi, out int li)) return false;
            if (!PageFieldParts((uint)pi, slot, out bm, out var tail)) return false;
            uint rc = _pageRecCount[pi];
            int offBytes = VarOffBytesLen(rc);
            if (tail.Length < offBytes) return false;
            offsets = new byte[offBytes];
            Array.Copy(tail, 0, offsets, 0, offBytes);
            values = new byte[tail.Length - offBytes];
            Array.Copy(tail, offBytes, values, 0, values.Length);
            bitIdx = (uint)li;
            return true;
        }

        return false;
    }

    internal bool ColNumericBytes(uint rec, int slot, out byte[] cell)
    {
        cell = Array.Empty<byte>();
        if (slot >= 0 && slot < KeySigils.Length && IsVarSigil(KeySigils[slot]))
            return false;

        if (LayoutKind == Layout.Columnar)
        {
            var (bm, vals) = ColFieldParts(slot);
            if (rec >= _recordCount || !ColBit(bm, rec)) return false;
            int off = (int)rec * 8;
            if (off + 8 > vals.Length) return false;
            cell = new byte[8];
            Array.Copy(vals, off, cell, 0, 8);
            return true;
        }

        if (LayoutKind == Layout.PAX)
        {
            if (!PaxFindPage(rec, out int pi, out int li)) return false;
            if (!PageFieldParts((uint)pi, slot, out var bm, out var vals)) return false;
            if (!ColBit(bm, (uint)li)) return false;
            int off = li * 8;
            if (off + 8 > vals.Length) return false;
            cell = new byte[8];
            Array.Copy(vals, off, cell, 0, 8);
            return true;
        }

        return false;
    }

    private static int VarOffBytesLen(uint rc) => (int)((rc + 1) * 4);

    private static string VarStrAt(byte[] offsets, byte[] values, uint recordIndex)
    {
        int off = (int)recordIndex * 4;
        if (off + 8 > offsets.Length) return "";
        int start = (int)BitConverter.ToUInt32(offsets, off);
        int end = (int)BitConverter.ToUInt32(offsets, off + 4);
        if (end < start || end > values.Length) return "";
        return Encoding.UTF8.GetString(values, start, end - start);
    }

    private double PaxSumF64(int slot)
    {
        double sum = 0;
        for (uint pi = 0; pi < _pageCount; pi++)
        {
            if (!PageFieldParts(pi, slot, out var bm, out var vals)) continue;
            uint rc = _pageRecCount[pi];
            for (uint i = 0; i < rc; i++)
            {
                if (!ColBit(bm, i)) continue;
                int off = (int)i * 8;
                if (off + 8 > vals.Length) break;
                sum += RdF64(vals, off);
            }
        }
        return sum;
    }

    private bool PaxFindPage(uint rec, out int pageIndex, out int localIndex)
    {
        pageIndex = 0;
        localIndex = 0;
        if (_pageCount == 0) return false;
        int lo = 0, hi = (int)_pageCount - 1;
        while (lo <= hi)
        {
            int mid = lo + (hi - lo) / 2;
            ulong start = _pageRecStart[mid];
            uint count = _pageRecCount[mid];
            if (rec < start) hi = mid - 1;
            else if (rec >= start + count) lo = mid + 1;
            else
            {
                pageIndex = mid;
                localIndex = (int)(rec - start);
                return true;
            }
        }
        return false;
    }

    private int FieldSectorLen(int sectorOff, uint rc, byte sigil)
    {
        int bmLen = NullBitmapBytes(rc);
        if (!IsVarSigil(sigil))
            return bmLen + (int)(rc * 8);
        int offBytes = VarOffBytesLen(rc);
        if (sectorOff + bmLen + offBytes > _data.Length)
            throw new NxsException("ERR_OUT_OF_BOUNDS", "var offsets");
        int end = (int)RdU32(sectorOff + bmLen + (int)rc * 4);
        return bmLen + offBytes + end;
    }

    private byte[]? PageFieldSector(uint pageIndex, int slot)
    {
        int poff = (int)_pageOffset[pageIndex];
        if (poff + 24 > _data.Length || RdU32(poff) != MagicPage) return null;
        int fc = RdU16(poff + 20);
        if (slot < 0 || slot >= fc || fc > KeySigils.Length) return null;
        uint rc = _pageRecCount[pageIndex];
        int body = poff + 24;
        for (int fi = 0; fi < slot; fi++)
        {
            byte sig = fi < KeySigils.Length ? KeySigils[fi] : (byte)'=';
            body += FieldSectorLen(body, rc, sig);
        }
        byte slotSig = slot < KeySigils.Length ? KeySigils[slot] : (byte)'=';
        int flen = FieldSectorLen(body, rc, slotSig);
        if (body + flen > _data.Length) return null;
        var sector = new byte[flen];
        Array.Copy(_data, body, sector, 0, flen);
        return sector;
    }

    private bool PageFieldParts(uint pageIndex, int slot, out byte[] bm, out byte[] vals)
    {
        bm = Array.Empty<byte>();
        vals = Array.Empty<byte>();
        var sector = PageFieldSector(pageIndex, slot);
        if (sector == null) return false;
        int bmLen = NullBitmapBytes(_pageRecCount[pageIndex]);
        if (sector.Length < bmLen) return false;
        bm = new byte[bmLen];
        Array.Copy(sector, 0, bm, 0, bmLen);
        vals = new byte[sector.Length - bmLen];
        Array.Copy(sector, bmLen, vals, 0, vals.Length);
        return true;
    }

    // ── Internal row helpers ──────────────────────────────────────────────────

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

    private static double RdF64(byte[] buf, int off) =>
        BitConverter.ToDouble(buf, off);

    private static long RdI64(byte[] buf, int off) =>
        BitConverter.ToInt64(buf, off);

    internal string RdStr(int off)
    {
        int len = (int)RdU32(off);
        return Encoding.UTF8.GetString(_data, off + 4, len);
    }

    internal int DataSize => _data.Length;
    internal byte DataAt(int off) => _data[off];
    internal Dictionary<string, int> KeyIndex => _keyIndex;

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
    private const uint MagicList = 0x4E59584Cu;

    private readonly NxsReader _reader;
    private readonly int _offset;
    private readonly uint _recordIndex;
    private bool _staged;
    private int _bitmaskStart;
    private int _offsetTableStart;

    internal NxsObject(NxsReader reader, int offset, uint recordIndex)
    {
        _reader = reader;
        _offset = offset;
        _recordIndex = recordIndex;
    }

    private bool ObjAtNyxo()
    {
        if (_offset + 4 > _reader.DataSize) return false;
        return _reader.RdU32(_offset) == MagicObj;
    }

    /// <summary>Columnar/PAX top-level records use record index; nested NYXO blobs use row paths.</summary>
    private bool UsesColumnarFieldAccess() =>
        _reader.LayoutKind != Layout.Row && !ObjAtNyxo();

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

    public bool HasField(string key)
    {
        if (!_reader.KeyIndex.TryGetValue(key, out int slot)) return false;
        return HasFieldBySlot(slot);
    }

    public bool HasFieldBySlot(int slot)
    {
        if (UsesColumnarFieldAccess())
            return _reader.ColNumericBytes(_recordIndex, slot, out _) ||
                   TryColStr(slot, out _);
        return ResolveSlot(slot) >= 0;
    }

    private bool TryColStr(int slot, out string? value)
    {
        value = null;
        if (slot < 0 || slot >= _reader.KeySigils.Length || _reader.KeySigils[slot] != (byte)'"')
            return false;
        value = _reader.ColGetStr(_reader.Keys[slot], _recordIndex);
        return value != null;
    }

    public long GetI64(string key) => GetI64BySlot(_reader.Slot(key));
    public double GetF64(string key) => GetF64BySlot(_reader.Slot(key));
    public bool GetBool(string key) => GetBoolBySlot(_reader.Slot(key));
    public string GetStr(string key) => GetStrBySlot(_reader.Slot(key));

    public long GetI64BySlot(int slot)
    {
        if (UsesColumnarFieldAccess())
        {
            if (!_reader.ColNumericBytes(_recordIndex, slot, out var cell))
                throw new NxsException("ERR_FIELD_ABSENT", $"slot {slot}");
            return BitConverter.ToInt64(cell, 0);
        }
        int off = ResolveSlot(slot);
        if (off < 0) throw new NxsException("ERR_FIELD_ABSENT", $"slot {slot}");
        return _reader.RdI64(off);
    }

    public double GetF64BySlot(int slot)
    {
        if (UsesColumnarFieldAccess())
        {
            if (!_reader.ColNumericBytes(_recordIndex, slot, out var cell))
                throw new NxsException("ERR_FIELD_ABSENT", $"slot {slot}");
            return BitConverter.ToDouble(cell, 0);
        }
        int off = ResolveSlot(slot);
        if (off < 0) throw new NxsException("ERR_FIELD_ABSENT", $"slot {slot}");
        return _reader.RdF64(off);
    }

    public bool GetBoolBySlot(int slot)
    {
        if (UsesColumnarFieldAccess())
        {
            if (!_reader.ColNumericBytes(_recordIndex, slot, out var cell))
                throw new NxsException("ERR_FIELD_ABSENT", $"slot {slot}");
            return cell[0] != 0;
        }
        int off = ResolveSlot(slot);
        if (off < 0) throw new NxsException("ERR_FIELD_ABSENT", $"slot {slot}");
        return _reader.DataAt(off) != 0;
    }

    public string GetStrBySlot(int slot)
    {
        if (UsesColumnarFieldAccess())
        {
            if (!TryColStr(slot, out var s) || s == null)
                throw new NxsException("ERR_FIELD_ABSENT", $"slot {slot}");
            return s;
        }
        int off = ResolveSlot(slot);
        if (off < 0) throw new NxsException("ERR_FIELD_ABSENT", $"slot {slot}");
        return _reader.RdStr(off);
    }

    /// <summary>Try to read a field; returns null when absent (conformance-friendly).</summary>
    internal object? TryGetField(int slot, byte sigil)
    {
        if (UsesColumnarFieldAccess())
        {
            if (sigil == 0x22)
            {
                if (!TryColStr(slot, out var s)) return null;
                return s;
            }
            if (!_reader.ColNumericBytes(_recordIndex, slot, out var cell)) return null;
            return sigil switch
            {
                0x7E => (object)BitConverter.ToDouble(cell, 0),
                0x3F => (object)(cell[0] != 0),
                _ => (object)BitConverter.ToInt64(cell, 0),
            };
        }

        int off = ResolveSlot(slot);
        if (off < 0) return null;

        if (off + 4 <= _reader.DataSize && _reader.RdU32(off) == MagicList)
            return ReadList(off);

        return sigil switch
        {
            0x3D => (object)_reader.RdI64(off),
            0x7E => (object)_reader.RdF64(off),
            0x3F => (object)(_reader.DataAt(off) != 0),
            0x22 => (object)_reader.RdStr(off),
            0x40 => (object)_reader.RdI64(off),
            0x5E => null,
            _ => (object)_reader.RdI64(off),
        };
    }

    private object?[] ReadList(int off)
    {
        byte elemSigil = _reader.DataAt(off + 8);
        int elemCount = (int)_reader.RdU32(off + 9);
        int dataStart = off + 16;
        var result = new object?[elemCount];
        for (int i = 0; i < elemCount; i++)
        {
            int elemOff = dataStart + i * 8;
            if (elemOff + 8 > _reader.DataSize) break;
            result[i] = elemSigil switch
            {
                0x3D => (object)_reader.RdI64(elemOff),
                0x7E => (object)_reader.RdF64(elemOff),
                _ => null,
            };
        }
        return result;
    }
}
