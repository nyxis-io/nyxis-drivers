using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using Nxs;
using Xunit;

namespace Nxs.Tests;

public sealed class PrefetchTests
{
    private static byte[] BuildRecords(int n)
    {
        var schema = new NxsSchema(["id", "username", "score", "active"]);
        var w = new NxsWriter(schema);
        for (int i = 0; i < n; i++)
        {
            w.BeginObject();
            w.WriteI64(0, i);
            w.WriteStr(1, $"user_{i}");
            w.WriteF64(2, i * 0.25);
            w.WriteBool(3, i % 2 == 0);
            w.EndObject();
        }
        return w.Finish();
    }

    [Fact]
    public void CoalescePageIndices_Gap1_ProducesThreeRanges()
    {
        var ranges = PrefetchCoalesce.CoalescePageIndices(
            [3, 4, 6, 7, 12], 1, PrefetchDefaults.DefaultPageSize);
        Assert.Equal(3, ranges.Count);
        Assert.Equal(3, ranges[0].PageStart);
        Assert.Equal(4, ranges[0].PageEnd);
        Assert.Equal(6, ranges[1].PageStart);
        Assert.Equal(7, ranges[1].PageEnd);
        Assert.Equal(12, ranges[2].PageStart);
        Assert.Equal(12, ranges[2].PageEnd);
        Assert.Equal(2L * PrefetchDefaults.DefaultPageSize, ranges[0].ByteLength);
    }

    [Fact]
    public void OpenOptions_DefaultMaxPages_Is128()
    {
        Assert.Equal(128, new NxsOpenOptions().MaxPages);
    }

    [Fact]
    public async Task PrefetchViewportAsync_CoalescesToAtMostThreeFetches()
    {
        byte[] buf = BuildRecords(60);
        var ranges = new List<(long Start, long Len)>();
        var reader = new NxsReader(buf, new NxsOpenOptions
        {
            MaxPages = 64,
            CoalesceGapPages = 1,
            FetchRange = (off, len, _) =>
            {
                ranges.Add((off, len));
                return Task.FromResult(CopyRange(buf, off, len));
            },
        });

        await reader.PrefetchViewportAsync(0, 49);
        Assert.True(ranges.Count <= 3, $"expected ≤3 fetches, got {ranges.Count}");
        var stats = reader.CacheStats();
        Assert.Equal(ranges.Count, stats.FetchesIssued);
    }

    [Fact]
    public async Task PrefetchViewportAsync_RecordsReadableAfterPrefetch()
    {
        byte[] buf = BuildRecords(55);
        var reader = new NxsReader(buf, new NxsOpenOptions
        {
            FetchRange = (off, len, _) => Task.FromResult(CopyRange(buf, off, len)),
        });
        await reader.PrefetchViewportAsync(0, 49);
        Assert.Equal(49L, reader.Record(49).GetI64("id"));
    }

    [Fact]
    public async Task PrefetchViewportAsync_RespectsMaxPagesEviction()
    {
        byte[] buf = BuildRecords(20);
        var reader = new NxsReader(buf, new NxsOpenOptions
        {
            MaxPages = 2,
            PageSize = 256,
            CoalesceGapPages = 0,
        });
        await reader.PrefetchViewportAsync(0, 0);
        await reader.PrefetchViewportAsync(19, 19);
        Assert.True(reader.CacheStats().PagesCached <= 2);
    }

    [Fact]
    public async Task PrefetchViewportAsync_DeduplicatesParallelRequests()
    {
        byte[] buf = BuildRecords(10);
        int calls = 0;
        var reader = new NxsReader(buf, new NxsOpenOptions
        {
            MaxPages = 8,
            FetchRange = async (off, len, _) =>
            {
                Interlocked.Increment(ref calls);
                await Task.Delay(5);
                return CopyRange(buf, off, len);
            },
        });

        await Task.WhenAll(
            reader.PrefetchViewportAsync(0, 4),
            reader.PrefetchViewportAsync(0, 4));
        Assert.True(calls <= 3, $"too many fetches: {calls}");
    }

    [Fact]
    public void AccessHint_StoredFromOpenOptions()
    {
        byte[] buf = BuildRecords(1);
        var reader = new NxsReader(buf, new NxsOpenOptions { Hint = AccessHint.Full });
        Assert.Equal(AccessHint.Full, reader.AccessHint);
    }

    private static byte[] CopyRange(byte[] buf, long off, long len)
    {
        var outBuf = new byte[len];
        Buffer.BlockCopy(buf, (int)off, outBuf, 0, (int)len);
        return outBuf;
    }
}
