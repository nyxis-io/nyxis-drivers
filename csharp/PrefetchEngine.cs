// Phase-2 prefetch engine (pattern detector, adaptive strategy, speculative and eager loads).

using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;

namespace Nxs;

internal sealed class PrefetchEngine
{
    private readonly object _stateLock = new();
    private readonly SemaphoreSlim _cacheLock = new(1, 1);
    private readonly PageCache _cache;
    private readonly InFlightMap _inFlight = new();
    private readonly NxsOpenOptions _options;
    private readonly int _fileSize;
    private readonly int _tailStart;
    private readonly int _recordCount;
    private readonly Func<int, long> _recordOffset;
    private readonly Func<long, long, CancellationToken, Task<byte[]>> _fetchRange;
    private readonly AccessPatternDetector _detector = new();

    private PrefetchStrategy _strategy;
    private int _fetchesIssued;
    private CancellationTokenSource? _eagerCts;
    private Task? _eagerTask;
    private bool _eagerStarted;
    private volatile bool _eagerComplete;
    private bool _closed;
    private bool _paused;

    public PrefetchEngine(
        NxsOpenOptions options,
        int fileSize,
        int tailStart,
        int recordCount,
        Func<int, long> recordOffset,
        Func<long, long, CancellationToken, Task<byte[]>> fetchRange)
    {
        _options = options;
        _fileSize = fileSize;
        _tailStart = tailStart;
        _recordCount = recordCount;
        _recordOffset = recordOffset;
        _fetchRange = fetchRange;
        _cache = new PageCache(options.MaxPages, options.PageSize);
        _strategy = PrefetchStrategySelect.Initial(options.Hint, fileSize);
    }

    public void StartEagerBackgroundIfNeeded()
    {
        lock (_stateLock)
        {
            if (_strategy == PrefetchStrategy.Eager)
                StartEagerBackgroundLocked();
        }
    }

    public void OnAccess(int index)
    {
        if (_recordCount == 0) return;
        lock (_stateLock)
        {
            if (_closed || _paused) return;
            _detector.Observe(index);
            MaybeUpgradeToEagerLocked();
        }

        bool eagerReady = IsEagerComplete();
        PrefetchStrategy strategy;
        lock (_stateLock)
        {
            strategy = _strategy;
            if (_closed) return;
        }

        if (eagerReady || strategy == PrefetchStrategy.Eager) return;

        if (_recordOffset(index) >= 0)
        {
            int pageIndex = (int)(_recordOffset(index) / _options.PageSize);
            _cacheLock.Wait();
            try { _ = _cache.Get(pageIndex); }
            finally { _cacheLock.Release(); }
        }

        if (strategy == PrefetchStrategy.Adaptive
            && _detector.Pattern() == AccessPattern.Sequential)
        {
            _ = SpeculativePrefetchAsync(CancellationToken.None);
        }
    }

    public async Task WarmupAsync(CancellationToken cancellationToken = default)
    {
        Task? eager;
        lock (_stateLock) eager = _eagerTask;
        if (eager != null)
            await eager.WaitAsync(cancellationToken).ConfigureAwait(false);
    }

    public void PausePrefetch()
    {
        lock (_stateLock) _paused = true;
    }

    public void ResumePrefetch()
    {
        lock (_stateLock) _paused = false;
    }

    public void Close()
    {
        CancellationTokenSource? cts;
        lock (_stateLock)
        {
            if (_closed) return;
            _closed = true;
            cts = _eagerCts;
        }
        cts?.Cancel();
    }

    public CacheStats CacheStats()
    {
        string strategy;
        string pattern;
        lock (_stateLock)
        {
            strategy = PrefetchStrategyNames.Name(_strategy);
            pattern = AccessPatternDetector.PatternName(_detector.Pattern());
        }
        _cacheLock.Wait();
        try
        {
            var (pagesCached, memoryUsed) = _cache.Stats();
            return new CacheStats(
                pagesCached, _cache.MaxPages, memoryUsed,
                _cache.Hits, _cache.Misses, _fetchesIssued,
                ColumnFetchesIssued: 0, strategy, pattern);
        }
        finally { _cacheLock.Release(); }
    }

    public async Task PrefetchViewportAsync(
        int startIndex, int endIndex, CancellationToken cancellationToken = default)
    {
        if (_recordCount == 0) return;
        await _cacheLock.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            int pageSize = _options.PageSize;
            var indices = PrefetchCoalesce.PageIndicesForViewport(
                startIndex, endIndex, pageSize, _recordOffset);
            var missing = new HashSet<int>();
            foreach (int p in indices)
            {
                if (!_cache.Has(p) && !_inFlight.Has(p)) missing.Add(p);
            }
            if (missing.Count == 0)
            {
                _cache.PinPages(indices);
                _cache.UnpinAll();
                return;
            }
            var ranges = PrefetchCoalesce.ClampPageRanges(
                PrefetchCoalesce.CoalescePageIndices(missing, _options.CoalesceGapPages, pageSize),
                _fileSize);
            foreach (var pr in ranges)
            {
                cancellationToken.ThrowIfCancellationRequested();
                await FetchCoalescedRangeLockedAsync(pr, cancellationToken).ConfigureAwait(false);
            }
            _cache.PinPages(indices);
            _cache.UnpinAll();
        }
        finally { _cacheLock.Release(); }
    }

    private bool IsEagerComplete() =>
        Strategy == PrefetchStrategy.Eager && _eagerComplete;

    private PrefetchStrategy Strategy
    {
        get { lock (_stateLock) return _strategy; }
    }

    private void MaybeUpgradeToEagerLocked()
    {
        if (_paused || _strategy != PrefetchStrategy.Adaptive) return;
        if (_detector.Pattern() != AccessPattern.Sequential) return;
        if (_detector.SequentialRuns < PatternConstants.UpgradeSequentialThreshold) return;
        if (_fileSize / (1024 * 1024) > PrefetchDefaults.EagerThresholdMb) return;
        _strategy = PrefetchStrategy.Eager;
        StartEagerBackgroundLocked();
    }

    private void StartEagerBackgroundLocked()
    {
        if (_paused || _strategy != PrefetchStrategy.Eager || _eagerStarted) return;
        _eagerStarted = true;
        _eagerCts = new CancellationTokenSource();
        var token = _eagerCts.Token;
        _eagerTask = Task.Run(() => RunEagerBackgroundAsync(token), token);
    }

    private async Task RunEagerBackgroundAsync(CancellationToken cancellationToken)
    {
        var (sectorStart, sectorLen) = PrefetchStrategySelect.RowDataSector(_tailStart, _fileSize);
        if (sectorLen == 0) { _eagerComplete = true; return; }
        int end = sectorStart + sectorLen;
        if (end > _fileSize) end = _fileSize;
        if (sectorStart >= end)
        {
            if (!cancellationToken.IsCancellationRequested) _eagerComplete = true;
            return;
        }
        int pageSize = _options.PageSize;
        int firstPage = sectorStart / pageSize;
        int lastPage = (end - 1) / pageSize;
        var indices = new List<int>(lastPage - firstPage + 1);
        for (int p = firstPage; p <= lastPage; p++) indices.Add(p);
        await _cacheLock.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            foreach (var pr in PrefetchCoalesce.ClampPageRanges(
                PrefetchCoalesce.CoalescePageIndices(indices, _options.CoalesceGapPages, pageSize),
                _fileSize))
            {
                cancellationToken.ThrowIfCancellationRequested();
                await FetchCoalescedRangeLockedAsync(pr, cancellationToken).ConfigureAwait(false);
            }
            if (!cancellationToken.IsCancellationRequested) _eagerComplete = true;
        }
        finally { _cacheLock.Release(); }
    }

    private Task SpeculativePrefetchAsync(CancellationToken cancellationToken)
    {
        lock (_stateLock)
        {
            if (_paused) return Task.CompletedTask;
        }
        List<int> predicted;
        lock (_stateLock)
            predicted = _detector.PredictNext(_options.PrefetchDepth, _recordCount);
        if (predicted.Count == 0) return Task.CompletedTask;

        var pageIndices = new List<int>();
        var seen = new HashSet<int>();
        _cacheLock.Wait(cancellationToken);
        try
        {
            foreach (int idx in predicted)
            {
                long off = _recordOffset(idx);
                if (off < 0) continue;
                int p = (int)(off / _options.PageSize);
                if (!seen.Add(p)) continue;
                if (!_cache.Has(p) && !_inFlight.Has(p)) pageIndices.Add(p);
            }
        }
        finally { _cacheLock.Release(); }
        if (pageIndices.Count == 0) return Task.CompletedTask;

        var ranges = PrefetchCoalesce.ClampPageRanges(
            PrefetchCoalesce.CoalescePageIndices(pageIndices, _options.CoalesceGapPages, _options.PageSize),
            _fileSize);
        return Task.Run(async () =>
        {
            await _cacheLock.WaitAsync(cancellationToken).ConfigureAwait(false);
            try
            {
                foreach (var pr in ranges)
                {
                    if (cancellationToken.IsCancellationRequested) return;
                    await FetchCoalescedRangeLockedAsync(pr, cancellationToken).ConfigureAwait(false);
                }
            }
            finally { _cacheLock.Release(); }
        }, cancellationToken);
    }

    private async Task FetchCoalescedRangeLockedAsync(PageRange pr, CancellationToken cancellationToken)
    {
        Interlocked.Increment(ref _fetchesIssued);
        byte[] blob = await _fetchRange(pr.ByteStart, pr.ByteLength, cancellationToken).ConfigureAwait(false);
        long pageSize = _options.PageSize;
        for (int p = pr.PageStart; p <= pr.PageEnd; p++)
        {
            if (_cache.Has(p)) continue;
            long pageOff = (long)p * pageSize - pr.ByteStart;
            long pageLen = pageSize;
            if (pageOff + pageLen > blob.Length) pageLen = blob.Length - pageOff;
            if (pageLen <= 0) continue;
            var pageData = new byte[(int)pageLen];
            Buffer.BlockCopy(blob, (int)pageOff, pageData, 0, (int)pageLen);
            _cache.Set(p, pageData);
        }
    }
}
