// Prefetch lifecycle controls (split from Prefetch.swift for SwiftLint file_length).

extension PrefetchState {
    func pausePrefetch() {
        stateLock.lock()
        paused = true
        stateLock.unlock()
    }

    func resumePrefetch() {
        stateLock.lock()
        paused = false
        stateLock.unlock()
    }

    /// Evict unpinned pages and halve maxPages (platform memory pressure, §8.2).
    func respondToMemoryPressure() {
        cacheLock.lock()
        defer { cacheLock.unlock() }
        let newMax = max(1, cache.maxPages / 2)
        cache.maxPages = newMax
        while cache.stats().pagesCached > cache.maxPages {
            if !cache.evictOneUnpinned() { break }
        }
    }

    func close() { stateLock.lock(); closed = true; eagerCancelled = true; stateLock.unlock() }

    func currentStrategy() -> String {
        stateLock.lock()
        defer { stateLock.unlock() }
        return strategy.rawValue
    }

    func currentPattern() -> String { detector.pattern().rawValue }
}
