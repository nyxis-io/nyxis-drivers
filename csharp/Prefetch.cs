// Adaptive prefetch — page cache, range coalescing, in-flight dedup (spec §6–§8.4).

using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

namespace Nxs;

public enum AccessHint
{
    Unknown = 0,
    Sequential = 1,
    Random = 2,
    Full = 3,
    Partial = 4,
}

public static class PrefetchDefaults
{
    public const int DefaultPageSize = 65536;
    public const int DefaultMaxPages = 128;
    public const int DefaultCoalesceGapPages = 1;
}

/// <summary>Prefetch options on <see cref="NxsReader"/> construction (phase 1).</summary>
public sealed class NxsOpenOptions
{
    public AccessHint Hint { get; init; } = AccessHint.Unknown;
    public int MaxPages { get; init; } = PrefetchDefaults.DefaultMaxPages;
    public int PageSize { get; init; } = PrefetchDefaults.DefaultPageSize;
    public int CoalesceGapPages { get; init; } = PrefetchDefaults.DefaultCoalesceGapPages;

    /// <summary>Injectable byte-range fetcher for tests or remote I/O.</summary>
    public Func<long, long, CancellationToken, Task<byte[]>>? FetchRange { get; init; }
}

public readonly record struct CacheStats(
    int PagesCached,
    int PagesMax,
    int MemoryUsedBytes,
    int CacheHits,
    int CacheMisses,
    int FetchesIssued,
    string Strategy,
    string Pattern);

public readonly record struct PageRange(int PageStart, int PageEnd, long ByteStart, long ByteLength);

/// <summary>Merges sorted unique page indices when gap ≤ gapPages (Adaptive-prefetch-spec §7.2).</summary>
public static class PrefetchCoalesce
{
    public static IReadOnlyList<PageRange> CoalescePageIndices(
        IEnumerable<int> indices, int gapPages, int pageSize)
    {
        var uniq = indices.Distinct().OrderBy(p => p).ToList();
        if (uniq.Count == 0) return Array.Empty<PageRange>();

        var spans = new List<(int Start, int End)>();
        int start = uniq[0], end = uniq[0];
        for (int i = 1; i < uniq.Count; i++)
        {
            if (uniq[i] - end <= gapPages)
                end = uniq[i];
            else
            {
                spans.Add((start, end));
                start = end = uniq[i];
            }
        }
        spans.Add((start, end));

        var outRanges = new PageRange[spans.Count];
        for (int i = 0; i < spans.Count; i++)
        {
            var (a, b) = spans[i];
            outRanges[i] = new PageRange(
                a, b,
                (long)a * pageSize,
                (long)(b - a + 1) * pageSize);
        }
        return outRanges;
    }

    public static IReadOnlyList<PageRange> ClampPageRanges(IReadOnlyList<PageRange> ranges, long fileSize)
    {
        var outRanges = new List<PageRange>(ranges.Count);
        foreach (var r in ranges)
        {
            long length = r.ByteLength;
            if (r.ByteStart + length > fileSize)
                length = fileSize - r.ByteStart;
            if (length <= 0) continue;
            outRanges.Add(r with { ByteLength = length });
        }
        return outRanges;
    }

    public static List<int> PageIndicesForViewport(
        int startIndex, int endIndex, int pageSize, Func<int, long> recordOffset)
    {
        var outIndices = new List<int>(endIndex - startIndex + 1);
        for (int i = startIndex; i <= endIndex; i++)
            outIndices.Add((int)(recordOffset(i) / pageSize));
        return outIndices;
    }
}

internal sealed class PageEntry
{
    public byte[] Data = Array.Empty<byte>();
    public int LastUsed;
    public bool Pinned;
}

internal sealed class PageCache
{
    private readonly int _maxPages;
    private readonly Dictionary<int, PageEntry> _pages = new();
    private int _clock;
    public int Hits { get; private set; }
    public int Misses { get; private set; }

    public PageCache(int maxPages, int pageSize)
    {
        _maxPages = maxPages;
        PageSize = pageSize;
    }

    public int PageSize { get; }
    public int MaxPages => _maxPages;

    public bool Has(int pageIndex) => _pages.ContainsKey(pageIndex);

    public byte[]? Get(int pageIndex)
    {
        if (!_pages.TryGetValue(pageIndex, out var e))
        {
            Misses++;
            return null;
        }
        _clock++;
        e.LastUsed = _clock;
        Hits++;
        return e.Data;
    }

    public void Set(int pageIndex, byte[] data, bool pinned = false)
    {
        if (_maxPages <= 0) return;
        while (_pages.Count >= _maxPages)
        {
            if (!EvictOne()) break;
        }
        _clock++;
        _pages[pageIndex] = new PageEntry
        {
            Data = data,
            LastUsed = _clock,
            Pinned = pinned,
        };
    }

    private bool EvictOne()
    {
        int oldest = int.MaxValue;
        int victim = -1;
        foreach (var (idx, e) in _pages)
        {
            if (e.Pinned) continue;
            if (e.LastUsed < oldest)
            {
                oldest = e.LastUsed;
                victim = idx;
            }
        }
        if (victim < 0) return false;
        _pages.Remove(victim);
        return true;
    }

    public void PinPages(IEnumerable<int> pageIndices)
    {
        foreach (int p in pageIndices)
        {
            if (_pages.TryGetValue(p, out var e))
                e.Pinned = true;
        }
    }

    public void UnpinAll()
    {
        foreach (var e in _pages.Values)
            e.Pinned = false;
    }

    public (int PagesCached, int MemoryUsed) Stats()
    {
        int bytes = 0;
        foreach (var e in _pages.Values)
            bytes += e.Data.Length;
        return (_pages.Count, bytes);
    }
}

internal sealed class InFlightMap
{
    private readonly ConcurrentDictionary<int, Task<byte[]>> _map = new();

    public bool Has(int pageIndex) => _map.ContainsKey(pageIndex);

    public Task<byte[]>? Get(int pageIndex) =>
        _map.TryGetValue(pageIndex, out var t) ? t : null;

    public void Set(int pageIndex, Task<byte[]> task)
    {
        _map[pageIndex] = task;
        _ = task.ContinueWith(
            _ => _map.TryRemove(pageIndex, out Task<byte[]>? _),
            CancellationToken.None,
            TaskContinuationOptions.ExecuteSynchronously,
            TaskScheduler.Default);
    }
}
