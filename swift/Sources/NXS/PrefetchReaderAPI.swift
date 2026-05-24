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
        try prefetch.prefetchViewport(
            startIndex: startIndex,
            endIndex: endIndex,
            recordCount: recordCount,
            fileSize: Int64(data.count)
        )
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
