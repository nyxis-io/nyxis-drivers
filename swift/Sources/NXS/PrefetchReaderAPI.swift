// NXSReader prefetch API (split from Prefetch.swift for SwiftLint file_length).

extension NXSReader {
    public var accessHint: AccessHint { prefetch.hint }

    func recordByteOffsetForPrefetch(_ i: Int) -> Int64 {
        Int64(rdU64(data, tailStart + i * 10 + 2))
    }

    public func warmup() { prefetch.warmup() }

    public func pausePrefetch() { prefetch.pausePrefetch() }

    public func resumePrefetch() { prefetch.resumePrefetch() }

    public func respondToMemoryPressure() { prefetch.respondToMemoryPressure() }

    public func close() { prefetch.close() }

    func prefetchOnAccess(_ index: Int) {
        guard col.layout == .row else { return }
        prefetch.onAccess(index)
    }

    public func prefetchViewport(startIndex: Int, endIndex: Int) throws {
        guard col.layout == .row else { return }
        try prefetch.prefetchViewport(
            startIndex: startIndex,
            endIndex: endIndex,
            recordCount: recordCount,
            fileSize: Int64(data.count)
        )
    }

    /// Prefetch one column buffer (columnar layout only; §7.4).
    public func prefetchColumn(_ key: String) throws {
        guard col.layout == .columnar, let warm = columnWarm else {
            throw NXSError.keyNotFound("prefetch_column requires columnar layout")
        }
        let slot = try slot(key)
        try warm.prefetchColumn(slot: slot, colOff: col.colBufOff, colLen: col.colBufLen)
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
            columnFetchesIssued: columnWarm?.fetches ?? 0,
            strategy: prefetch.currentStrategy(),
            pattern: prefetch.currentPattern()
        )
    }
}
