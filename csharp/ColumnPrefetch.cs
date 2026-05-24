// Columnar column-buffer warmup (Adaptive-prefetch-spec §7.4).

using System;
using System.Collections.Generic;

namespace Nxs;

internal sealed class ColumnWarmState
{
    private readonly byte[] _data;
    private readonly Func<long, long, byte[]> _fetch;
    private readonly bool _customFetch;
    private readonly HashSet<int> _warmed = new();
    private readonly Dictionary<int, byte[]> _overlay = new();
    private readonly object _lock = new();
    public int Fetches { get; private set; }

    public ColumnWarmState(byte[] data, Func<long, long, byte[]>? fetchRange, bool customFetch = false)
    {
        _data = data;
        _customFetch = customFetch || fetchRange != null;
        _fetch = fetchRange ?? ((off, len) =>
        {
            long end = off + len;
            if (off < 0 || end > data.Length)
                throw new NxsException("ERR_OUT_OF_BOUNDS", $"column fetch [{off}, {end})");
            int lenInt = checked((int)len);
            var blob = new byte[lenInt];
            Array.Copy(data, (int)off, blob, 0, lenInt);
            return blob;
        });
    }

    public void PrefetchColumn(int slot, ulong[] colOff, ulong[] colLen)
    {
        long off;
        long len;
        lock (_lock)
        {
            if (_warmed.Contains(slot)) return;
            off = (long)colOff[slot];
            len = (long)colLen[slot];
            if (off < 0 || len < 0)
                throw new NxsException("ERR_OUT_OF_BOUNDS", "column buffer");
            if (!_customFetch && off + len > _data.Length)
                throw new NxsException("ERR_OUT_OF_BOUNDS", "column buffer");
        }

        byte[] blob = _fetch(off, len);

        lock (_lock)
        {
            if (_warmed.Contains(slot)) return;
            if (off + blob.Length > _data.Length)
                _overlay[slot] = blob;
            _warmed.Add(slot);
            Fetches++;
        }
    }

    public byte[] Sector(int slot, ulong[] colOff, ulong[] colLen)
    {
        int length = (int)colLen[slot];
        lock (_lock)
        {
            if (_overlay.TryGetValue(slot, out byte[]? o) && o.Length >= length)
            {
                var slice = new byte[length];
                Array.Copy(o, 0, slice, 0, length);
                return slice;
            }
        }
        int off = (int)colOff[slot];
        if (off >= 0 && off + length <= _data.Length)
        {
            var sector = new byte[length];
            Array.Copy(_data, off, sector, 0, length);
            return sector;
        }
        throw new NxsException("ERR_OUT_OF_BOUNDS", "column buffer");
    }
}
