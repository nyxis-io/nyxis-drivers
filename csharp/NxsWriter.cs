// NXS Writer — direct-to-buffer .nxb emitter for C# (.NET 8).
//
// Mirrors the Rust NxsWriter API:
//   NxsSchema — precompile keys once; share across NxsWriter instances.
//   NxsWriter — slot-based hot path; no per-key dictionary lookups during write.
//
// Usage:
//   using Nxs;
//
//   var schema = new NxsSchema(["id", "username", "score", "active"]);
//   var w = new NxsWriter(schema);
//   w.BeginObject();
//   w.WriteI64(0, 42);
//   w.WriteStr(1, "alice");
//   w.WriteF64(2, 9.5);
//   w.WriteBool(3, true);
//   w.EndObject();
//   byte[] bytes = w.Finish();

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Buffers.Binary;

namespace Nxs
{
    // ── MurmurHash3-64 ────────────────────────────────────────────────────────

    internal static class Murmur3
    {
        internal static ulong Hash64(ReadOnlySpan<byte> data)
        {
            const ulong C1 = 0xFF51AFD7ED558CCDul;
            const ulong C2 = 0xC4CEB9FE1A85EC53ul;
            ulong h = 0x93681D6255313A99ul;
            int len = data.Length;
            int i = 0;
            while (i < len)
            {
                ulong k = 0;
                for (int b = 0; b < 8 && i + b < len; b++)
                    k |= (ulong)data[i + b] << (b * 8);
                k = unchecked(k * C1); k ^= k >> 33;
                h ^= k;
                h = unchecked(h * C2); h ^= h >> 33;
                i += 8;
            }
            h ^= (ulong)len; h ^= h >> 33;
            h = unchecked(h * C1); h ^= h >> 33;
            return h;
        }
    }

    // ── Schema ────────────────────────────────────────────────────────────────

    public sealed class NxsSchema
    {
        public readonly string[] Keys;
        public readonly int BitmaskBytes;
        public int Count => Keys.Length;

        public NxsSchema(string[] keys)
        {
            Keys = keys;
            BitmaskBytes = Math.Max(1, (keys.Length + 6) / 7);
        }
    }

    // ── Frame ─────────────────────────────────────────────────────────────────

    internal sealed class Frame
    {
        public readonly int Start;
        public readonly byte[] Bitmask;
        public readonly List<int> OffsetTable = new();
        public readonly List<(int Slot, int BufPos)> SlotOffsets = new();
        public int LastSlot = -1;
        public bool NeedsSort;

        public Frame(int start, int bitmaskBytes)
        {
            Start = start;
            Bitmask = new byte[bitmaskBytes];
            for (int i = 0; i < bitmaskBytes - 1; i++) Bitmask[i] = 0x80;
        }
    }

    // ── Writer ────────────────────────────────────────────────────────────────

    internal static class Sigils
    {
        internal const byte Str    = 0x22; // '"' — string / var-length
        internal const byte I64    = 0x69; // 'i'
        internal const byte F64    = 0x64; // 'd'
        internal const byte Bool   = 0x62; // 'b'
        internal const byte Null   = 0x6E; // 'n'
        internal const byte Binary = 0x42; // 'B'
    }

    public sealed class NxsWriter
    {
        private readonly NxsSchema _schema;
        private readonly MemoryStream _buf = new(4096);
        private readonly Stack<Frame> _frames = new();
        private readonly List<int> _recordOffsets = new();
        private readonly byte[] _slotSigils;

        public NxsWriter(NxsSchema schema)
        {
            _schema = schema;
            _slotSigils = new byte[schema.Count];
            Array.Fill(_slotSigils, Sigils.Str);
        }

        public void BeginObject()
        {
            if (_frames.Count == 0) _recordOffsets.Add((int)_buf.Length);
            var frame = new Frame((int)_buf.Length, _schema.BitmaskBytes);
            _frames.Push(frame);

            WriteU32(0x4E59584F);                             // NYXO
            WriteU32(0);                                      // length placeholder
            _buf.Write(frame.Bitmask);
            _buf.Write(new byte[_schema.Count * 2]);          // offset table placeholder
            while ((_buf.Length - frame.Start) % 8 != 0) _buf.WriteByte(0);
        }

        public void EndObject()
        {
            if (_frames.Count == 0) throw new InvalidOperationException("EndObject without BeginObject");
            var frame = _frames.Pop();

            byte[] arr = _buf.GetBuffer();
            int totalLen = (int)_buf.Length - frame.Start;

            // Back-patch Length at start + 4
            BinaryPrimitives.WriteUInt32LittleEndian(arr.AsSpan(frame.Start + 4), (uint)totalLen);

            // Back-patch bitmask at start + 8
            int bmOff = frame.Start + 8;
            frame.Bitmask.CopyTo(arr, bmOff);

            // Back-patch offset table
            int otStart = bmOff + _schema.BitmaskBytes;
            int present = frame.OffsetTable.Count;

            if (!frame.NeedsSort)
            {
                for (int i = 0; i < present; i++)
                    BinaryPrimitives.WriteUInt16LittleEndian(arr.AsSpan(otStart + i * 2), (ushort)frame.OffsetTable[i]);
            }
            else
            {
                var sorted = new List<(int Slot, int BufPos)>(frame.SlotOffsets);
                sorted.Sort((a, b) => a.Slot.CompareTo(b.Slot));
                for (int i = 0; i < sorted.Count; i++)
                    BinaryPrimitives.WriteUInt16LittleEndian(arr.AsSpan(otStart + i * 2),
                        (ushort)(sorted[i].BufPos - frame.Start));
            }

            for (int i = present * 2; i < _schema.Count * 2; i++) arr[otStart + i] = 0;
        }

        public byte[] Finish()
        {
            if (_frames.Count != 0) throw new InvalidOperationException("unclosed objects");

            byte[] schemaBytes = BuildSchemaBytes();
            ulong dictHash = Murmur3.Hash64(schemaBytes);
            int dataStart = 32 + schemaBytes.Length;
            byte[] dataSector = _buf.ToArray();
            ulong tailPtr = (ulong)(dataStart + dataSector.Length);
            byte[] tail = BuildTailIndex(dataStart, tailPtr);

            using var out_ = new MemoryStream(32 + schemaBytes.Length + dataSector.Length + tail.Length);
            Span<byte> tmp = stackalloc byte[8];

            BinaryPrimitives.WriteUInt32LittleEndian(tmp, 0x4E595842u); out_.Write(tmp[..4]); // NYXB
            BinaryPrimitives.WriteUInt16LittleEndian(tmp, 0x0101); out_.Write(tmp[..2]); // VERSION
            BinaryPrimitives.WriteUInt16LittleEndian(tmp, 0x0002); out_.Write(tmp[..2]); // FLAG_SCHEMA
            BinaryPrimitives.WriteUInt64LittleEndian(tmp, dictHash); out_.Write(tmp);
            BinaryPrimitives.WriteUInt64LittleEndian(tmp, 0); out_.Write(tmp);
            out_.Write(new byte[8]); // reserved

            out_.Write(schemaBytes);
            out_.Write(dataSector);
            out_.Write(tail);
            return out_.ToArray();
        }

        // ── Typed write methods ──────────────────────────────────────────────

        public void WriteI64(int slot, long value)
        {
            _slotSigils[slot] = Sigils.I64;
            MarkSlot(slot);
            Span<byte> b = stackalloc byte[8];
            BinaryPrimitives.WriteInt64LittleEndian(b, value);
            _buf.Write(b);
        }

        public void WriteF64(int slot, double value)
        {
            _slotSigils[slot] = Sigils.F64;
            MarkSlot(slot);
            Span<byte> b = stackalloc byte[8];
            BinaryPrimitives.WriteDoubleLittleEndian(b, value);
            _buf.Write(b);
        }

        public void WriteBool(int slot, bool value)
        {
            _slotSigils[slot] = Sigils.Bool;
            MarkSlot(slot);
            _buf.WriteByte(value ? (byte)1 : (byte)0);
            _buf.Write(new byte[7]);
        }

        public void WriteTime(int slot, long unixNs)
        {
            _slotSigils[slot] = Sigils.I64;
            WriteI64(slot, unixNs);
        }

        public void WriteNull(int slot)
        {
            _slotSigils[slot] = Sigils.Null;
            MarkSlot(slot);
            _buf.Write(new byte[8]);
        }

        public void WriteStr(int slot, string value)
        {
            _slotSigils[slot] = Sigils.Str;
            MarkSlot(slot);
            byte[] b = Encoding.UTF8.GetBytes(value);
            WriteU32((uint)b.Length);
            _buf.Write(b);
            int used = (4 + b.Length) % 8;
            if (used != 0) _buf.Write(new byte[8 - used]);
        }

        public void WriteBytes(int slot, ReadOnlySpan<byte> value)
        {
            _slotSigils[slot] = Sigils.Binary;
            MarkSlot(slot);
            WriteU32((uint)value.Length);
            _buf.Write(value);
            int used = (4 + value.Length) % 8;
            if (used != 0) _buf.Write(new byte[8 - used]);
        }

        public void WriteListI64(int slot, long[] values)
        {
            MarkSlot(slot); // list is var-length — keep Sigils.Str default
            int total = 16 + values.Length * 8;
            WriteU32(0x4E59584C); WriteU32((uint)total);
            _buf.WriteByte(0x3D); // '=' sigil
            WriteU32((uint)values.Length);
            _buf.Write(new byte[3]);
            Span<byte> tmp64i = stackalloc byte[8];
            foreach (var v in values) { BinaryPrimitives.WriteInt64LittleEndian(tmp64i, v); _buf.Write(tmp64i); }
        }

        public void WriteListF64(int slot, double[] values)
        {
            MarkSlot(slot);
            int total = 16 + values.Length * 8;
            WriteU32(0x4E59584C); WriteU32((uint)total);
            _buf.WriteByte(0x7E); // '~' sigil
            WriteU32((uint)values.Length);
            _buf.Write(new byte[3]);
            Span<byte> tmp64f = stackalloc byte[8];
            foreach (var v in values) { BinaryPrimitives.WriteDoubleLittleEndian(tmp64f, v); _buf.Write(tmp64f); }
        }

        // Convenience: write records from a list of dictionaries.
        public static byte[] FromRecords(string[] keys, IEnumerable<Dictionary<string, object?>> records)
        {
            var schema = new NxsSchema(keys);
            var w = new NxsWriter(schema);
            foreach (var rec in records)
            {
                w.BeginObject();
                for (int i = 0; i < keys.Length; i++)
                {
                    if (!rec.TryGetValue(keys[i], out var val)) continue;
                    switch (val)
                    {
                        case null: w.WriteNull(i); break;
                        case bool b: w.WriteBool(i, b); break;
                        case int iv: w.WriteI64(i, iv); break;
                        case long lv: w.WriteI64(i, lv); break;
                        case float fv: w.WriteF64(i, fv); break;
                        case double dv: w.WriteF64(i, dv); break;
                        case string sv: w.WriteStr(i, sv); break;
                    }
                }
                w.EndObject();
            }
            return w.Finish();
        }

        // ── Private helpers ──────────────────────────────────────────────────

        private void MarkSlot(int slot)
        {
            if (_frames.Count == 0) throw new InvalidOperationException("write outside BeginObject/EndObject");
            var frame = _frames.Peek();
            frame.Bitmask[slot / 7] |= (byte)(1 << (slot % 7));
            int rel = (int)_buf.Length - frame.Start;
            if (slot < frame.LastSlot) frame.NeedsSort = true;
            frame.LastSlot = slot;
            frame.OffsetTable.Add(rel);
            frame.SlotOffsets.Add((slot, (int)_buf.Length));
        }

        private void WriteU32(uint v)
        {
            Span<byte> b = stackalloc byte[4];
            BinaryPrimitives.WriteUInt32LittleEndian(b, v);
            _buf.Write(b);
        }

        private byte[] BuildSchemaBytes()
        {
            int n = _schema.Count;
            var encoded = new byte[n][];
            int size = 2 + n;
            for (int i = 0; i < n; i++) { encoded[i] = Encoding.UTF8.GetBytes(_schema.Keys[i]); size += encoded[i].Length + 1; }
            int padded = size + ((8 - size % 8) % 8);
            var b = new byte[padded];
            int p = 0;
            BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(p), (ushort)n); p += 2;
            for (int i = 0; i < n; i++) b[p++] = _slotSigils[i];
            foreach (var e in encoded) { e.CopyTo(b, p); p += e.Length; b[p++] = 0; }
            return b;
        }

        private byte[] BuildTailIndex(int dataStart, ulong tailPtr)
        {
            int nr = _recordOffsets.Count;
            using var t = new MemoryStream(4 + nr * 10 + 12);
            Span<byte> tmp = stackalloc byte[8];

            BinaryPrimitives.WriteUInt32LittleEndian(tmp, (uint)nr); t.Write(tmp[..4]);
            for (int i = 0; i < nr; i++)
            {
                BinaryPrimitives.WriteUInt16LittleEndian(tmp, (ushort)i); t.Write(tmp[..2]);
                BinaryPrimitives.WriteUInt64LittleEndian(tmp, (ulong)(dataStart + _recordOffsets[i])); t.Write(tmp);
            }
            BinaryPrimitives.WriteUInt64LittleEndian(tmp, tailPtr); t.Write(tmp);
            BinaryPrimitives.WriteUInt32LittleEndian(tmp, 0x2153584Eu); t.Write(tmp[..4]); // NXS!
            return t.ToArray();
        }
    }
}
