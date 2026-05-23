// NXSReader prefetch API (split from Prefetch.swift for SwiftLint file_length).

extension NXSReader {
    public var accessHint: AccessHint { prefetch.hint }

    func recordByteOffsetForPrefetch(_ i: Int) -> Int64 {
        Int64(rdU64(data, tailStart + i * 10 + 2))
    }

    public func warmup() { prefetch.warmup() }

    public func close() { prefetch.close() }

    func prefetchOnAccess(_ index: Int) {
        guard col.layout == .row else { return }
        prefetch.onAccess(index)
    }

    public func prefetchViewport(startIndex: Int, endIndex: Int) throws {
        guard col.layout == .row else { return }
        let n = recordCount
        guard startIndex >= 0, endIndex >= startIndex, endIndex < n else {
            throw NXSError.outOfBounds(
                "prefetch_viewport [\(startIndex), \(endIndex)] out of [0, \(n))"
            )
        }

        prefetchLock.lock()
        defer { prefetchLock.unlock() }

        let pageSize = prefetch.pageSize
        let indices = pageIndicesForViewport(
            startIndex: startIndex,
            endIndex: endIndex,
            pageSize: pageSize,
            recordOffset: { [self] i in self.recordByteOffsetForPrefetch(i) }
        )

        var missingSet = Set<Int>()
        for p in indices {
            if !prefetch.cache.has(p) && !prefetch.inFlight.has(p) {
                missingSet.insert(p)
            }
        }
        if missingSet.isEmpty {
            prefetch.cache.pinPages(indices)
            prefetch.cache.unpinAll()
            return
        }

        let missing = Array(missingSet)
        let ranges = clampPageRanges(
            coalescePageIndices(missing, gapPages: prefetch.coalesceGapPages, pageSize: pageSize),
            fileSize: Int64(data.count)
        )
        for pr in ranges {
            try prefetch.fetchCoalescedRangeLocked(pr)
        }
        prefetch.cache.pinPages(indices)
        prefetch.cache.unpinAll()
    }

    public func cacheStats() -> CacheStats {
        let (pagesCached, memoryUsed) = prefetch.cache.stats()
        return CacheStats(
            pagesCached: pagesCached,
            pagesMax: prefetch.cache.maxPages,
            memoryUsedBytes: memoryUsed,
            cacheHits: prefetch.cache.hits,
            cacheMisses: prefetch.cache.misses,
            fetchesIssued: prefetch.fetchesIssued,
            strategy: prefetch.currentStrategy(),
            pattern: prefetch.currentPattern()
        )
    }
}
