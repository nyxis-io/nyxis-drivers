// Adaptive prefetch — page cache, range coalescing, in-flight dedup (spec §6–§8.4).

import Foundation

public let defaultPageSize: Int = 65536
public let defaultMaxPages: Int = 128
public let defaultCoalesceGapPages: Int = 1
public let defaultPrefetchDepth: Int = 4
public let eagerThresholdMb: Int = 10
public let lazyThresholdMb: Int = 50

public enum PrefetchStrategy: String { case lazy, adaptive, eager }

public func initialPrefetchStrategy(hint: AccessHint, fileSize: Int) -> PrefetchStrategy {
    let mb = fileSize / (1024 * 1024)
    if hint == .full && mb <= eagerThresholdMb { return .eager }
    if mb > lazyThresholdMb { return .lazy }
    return .adaptive
}

public func rowDataSector(tailStart: Int, fileSize: Int) -> (start: Int, length: Int) {
    if tailStart > 32 && tailStart <= fileSize { return (32, tailStart - 32) }
    return (32, 0)
}

/// Advisory access hint.
public enum AccessHint: UInt8 {
    case unknown = 0
    case sequential = 1
    case random = 2
    case full = 3
    case partial = 4
}

/// Diagnostic cache and prefetch counters.
public struct CacheStats {
    public let pagesCached: Int
    public let pagesMax: Int
    public let memoryUsedBytes: Int
    public let cacheHits: Int
    public let cacheMisses: Int
    public let fetchesIssued: Int
    public let strategy: String
    public let pattern: String
}

public struct PageRange {
    public let pageStart: Int
    public let pageEnd: Int
    public let byteStart: Int64
    public var byteLength: Int64
}

/// Open-time prefetch configuration.
public struct NXSOpenOptions {
    public var hint: AccessHint = .unknown
    public var maxPages: Int = defaultMaxPages
    public var pageSize: Int = defaultPageSize
    public var coalesceGapPages: Int = defaultCoalesceGapPages
    public var prefetchDepth: Int = defaultPrefetchDepth
    public var fetchRange: ((Int64, Int64) throws -> Data)?

    public init(
        hint: AccessHint = .unknown,
        maxPages: Int = defaultMaxPages,
        pageSize: Int = defaultPageSize,
        coalesceGapPages: Int = defaultCoalesceGapPages,
        prefetchDepth: Int = defaultPrefetchDepth,
        fetchRange: ((Int64, Int64) throws -> Data)? = nil
    ) {
        self.hint = hint
        self.maxPages = maxPages
        self.pageSize = pageSize
        self.coalesceGapPages = coalesceGapPages
        self.prefetchDepth = prefetchDepth
        self.fetchRange = fetchRange
    }
}

/// Merge sorted unique page indices when the gap between consecutive indices is at most gapPages.
public func coalescePageIndices(_ indices: [Int], gapPages: Int, pageSize: Int) -> [PageRange] {
    guard !indices.isEmpty else { return [] }
    var seen = Set<Int>()
    var uniq: [Int] = []
    for p in indices where seen.insert(p).inserted {
        uniq.append(p)
    }
    uniq.sort()

    var spans: [(Int, Int)] = []
    var start = uniq[0]
    var end = uniq[0]
    for i in 1..<uniq.count {
        if uniq[i] - end <= gapPages {
            end = uniq[i]
        } else {
            spans.append((start, end))
            start = uniq[i]
            end = uniq[i]
        }
    }
    spans.append((start, end))

    let ps = Int64(pageSize)
    return spans.map { s in
        PageRange(
            pageStart: s.0,
            pageEnd: s.1,
            byteStart: Int64(s.0) * ps,
            byteLength: Int64(s.1 - s.0 + 1) * ps
        )
    }
}

func clampPageRanges(_ ranges: [PageRange], fileSize: Int64) -> [PageRange] {
    ranges.compactMap { r in
        var length = r.byteLength
        if r.byteStart + length > fileSize {
            length = fileSize - r.byteStart
        }
        guard length > 0 else { return nil }
        var out = r
        out.byteLength = length
        return out
    }
}

func pageIndicesForViewport(
    startIndex: Int,
    endIndex: Int,
    pageSize: Int,
    recordOffset: (Int) -> Int64
) -> [Int] {
    var out: [Int] = []
    out.reserveCapacity(endIndex - startIndex + 1)
    for i in startIndex...endIndex {
        let off = recordOffset(i)
        out.append(Int(off / Int64(pageSize)))
    }
    return out
}

private struct PageEntry {
    var data: Data
    var lastUsed: Int
    var pinned: Bool
}

final class PageCache {
    let maxPages: Int
    let pageSize: Int
    private var pages: [Int: PageEntry] = [:]
    private var clock = 0
    private(set) var hits = 0
    private(set) var misses = 0

    init(maxPages: Int, pageSize: Int) {
        self.maxPages = maxPages
        self.pageSize = pageSize
    }

    func has(_ pageIndex: Int) -> Bool {
        pages[pageIndex] != nil
    }

    func get(_ pageIndex: Int) -> Data? {
        guard var e = pages[pageIndex] else {
            misses += 1
            return nil
        }
        clock += 1
        e.lastUsed = clock
        pages[pageIndex] = e
        hits += 1
        return e.data
    }

    func set(_ pageIndex: Int, data: Data, pinned: Bool = false) {
        guard maxPages > 0 else { return }
        while pages.count >= maxPages {
            if !evictOne() { break }
        }
        clock += 1
        pages[pageIndex] = PageEntry(data: data, lastUsed: clock, pinned: pinned)
    }

    @discardableResult
    private func evictOne() -> Bool {
        var oldest = Int.max
        var victim = -1
        for (idx, e) in pages where !e.pinned {
            if e.lastUsed < oldest {
                oldest = e.lastUsed
                victim = idx
            }
        }
        guard victim >= 0 else { return false }
        pages.removeValue(forKey: victim)
        return true
    }

    func pinPages(_ pageIndices: [Int]) {
        for p in pageIndices {
            if var e = pages[p] {
                e.pinned = true
                pages[p] = e
            }
        }
    }

    func unpinAll() {
        for (idx, e) in pages {
            var entry = e
            entry.pinned = false
            pages[idx] = entry
        }
    }

    func stats() -> (pagesCached: Int, memoryUsed: Int) {
        var memory = 0
        for e in pages.values { memory += e.data.count }
        return (pages.count, memory)
    }
}

final class InFlightEntry {
    let group = DispatchGroup()
    var data: Data?
    var error: Error?
}

final class InFlightMap {
    private let lock = NSLock()
    private var entries: [Int: InFlightEntry] = [:]

    func has(_ pageIndex: Int) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        return entries[pageIndex] != nil
    }

    func wait(_ pageIndex: Int) -> (Data?, Error?) {
        lock.lock()
        let entry = entries[pageIndex]
        lock.unlock()
        guard let entry else { return (nil, nil) }
        entry.group.wait()
        return (entry.data, entry.error)
    }

    func begin(_ pageIndex: Int) -> (InFlightEntry, Bool) {
        lock.lock()
        defer { lock.unlock() }
        if let existing = entries[pageIndex] {
            return (existing, false)
        }
        let entry = InFlightEntry()
        entry.group.enter()
        entries[pageIndex] = entry
        return (entry, true)
    }

    func finish(_ pageIndex: Int, entry: InFlightEntry) {
        entry.group.leave()
        lock.lock()
        if entries[pageIndex] === entry {
            entries.removeValue(forKey: pageIndex)
        }
        lock.unlock()
    }
}

/// Phase-2 prefetch engine owned by NXSReader.
final class PrefetchState {
    var hint: AccessHint
    var pageSize: Int
    var coalesceGapPages: Int
    var prefetchDepth: Int
    let cache: PageCache
    let inFlight: InFlightMap
    var fetchesIssued = 0
    var fetchRange: (Int64, Int64) throws -> Data

    private let stateLock = NSLock()
    private let cacheLock = NSLock()
    private let detector = AccessPatternDetector()
    private var strategy: PrefetchStrategy
    private var eagerStarted = false
    private var eagerComplete = false
    private var eagerCancelled = false
    private var eagerGroup: DispatchGroup?
    private var closed = false
    private let fileSize: Int
    private(set) var tailStart: Int
    private var recordCount: () -> Int
    private var recordOffset: (Int) -> Int64

    init(options: NXSOpenOptions, data: Data, fileSize: Int, tailStart: Int,
         recordCount: @escaping () -> Int, recordOffset: @escaping (Int) -> Int64) {
        hint = options.hint
        pageSize = options.pageSize
        coalesceGapPages = options.coalesceGapPages
        prefetchDepth = options.prefetchDepth
        self.fileSize = fileSize
        self.tailStart = tailStart
        self.recordCount = recordCount
        self.recordOffset = recordOffset
        strategy = initialPrefetchStrategy(hint: options.hint, fileSize: fileSize)
        cache = PageCache(maxPages: options.maxPages, pageSize: options.pageSize)
        inFlight = InFlightMap()
        if let custom = options.fetchRange {
            fetchRange = custom
        } else {
            let buf = data
            fetchRange = { off, len in
                let end = off + len
                guard off >= 0, end <= Int64(buf.count) else {
                    throw NXSError.outOfBounds("fetch range [\(off), \(end))")
                }
                return buf.subdata(in: Int(off)..<Int(end))
            }
        }
    }

    func configureTailStart(_ start: Int) { tailStart = start }
    func startEagerBackgroundIfNeeded() {
        stateLock.lock(); defer { stateLock.unlock() }
        if strategy == .eager { startEagerBackgroundLocked() }
    }
    func prefetchViewport(startIndex: Int, endIndex: Int, recordCount n: Int, fileSize: Int64) throws {
        guard startIndex >= 0, endIndex >= startIndex, endIndex < n else {
            throw NXSError.outOfBounds(
                "prefetch_viewport [\(startIndex), \(endIndex)] out of [0, \(n))"
            )
        }

        cacheLock.lock()
        defer { cacheLock.unlock() }

        let indices = pageIndicesForViewport(
            startIndex: startIndex,
            endIndex: endIndex,
            pageSize: pageSize,
            recordOffset: recordOffset
        )

        var missingSet = Set<Int>()
        for p in indices {
            if !cache.has(p) && !inFlight.has(p) {
                missingSet.insert(p)
            }
        }
        if missingSet.isEmpty {
            cache.pinPages(indices)
            cache.unpinAll()
            return
        }

        let missing = Array(missingSet)
        let ranges = clampPageRanges(
            coalescePageIndices(missing, gapPages: coalesceGapPages, pageSize: pageSize),
            fileSize: fileSize
        )
        for pr in ranges {
            try fetchCoalescedRangeLocked(pr)
        }
        cache.pinPages(indices)
        cache.unpinAll()
    }

    func onAccess(_ index: Int) {
        guard recordCount() > 0 else { return }
        stateLock.lock()
        if closed { stateLock.unlock(); return }
        detector.observe(index)
        maybeUpgradeToEagerLocked()
        let strat = strategy; let done = eagerComplete
        stateLock.unlock()
        if done || strat == .eager { return }
        let off = recordOffset(index)
        if off >= 0 {
            cacheLock.lock(); _ = cache.get(Int(off / Int64(pageSize))); cacheLock.unlock()
        }
        if strat == .adaptive && detector.pattern() == .sequential { speculativePrefetch() }
    }
    func warmup() { eagerGroup?.wait() }
    func close() { stateLock.lock(); closed = true; eagerCancelled = true; stateLock.unlock() }
    func currentStrategy() -> String { stateLock.lock(); defer { stateLock.unlock() }; return strategy.rawValue }
    func currentPattern() -> String { detector.pattern().rawValue }

    private func maybeUpgradeToEagerLocked() {
        guard strategy == .adaptive, detector.pattern() == .sequential,
              detector.sequentialRuns >= UInt32(PatternConstants.upgradeSequentialThreshold),
              fileSize / (1024 * 1024) <= eagerThresholdMb else { return }
        strategy = .eager; startEagerBackgroundLocked()
    }
    private func startEagerBackgroundLocked() {
        guard strategy == .eager, !eagerStarted else { return }
        eagerStarted = true
        let g = DispatchGroup(); eagerGroup = g; g.enter()
        DispatchQueue.global(qos: .utility).async { defer { g.leave() }; self.runEagerBackground() }
    }
    private func runEagerBackground() {
        let s = rowDataSector(tailStart: tailStart, fileSize: fileSize)
        if s.length <= 0 {
            stateLock.lock()
            eagerComplete = true
            stateLock.unlock()
            return
        }
        var end = s.start + s.length
        if end > fileSize { end = fileSize }
        guard s.start < end else {
            stateLock.lock()
            if !eagerCancelled { eagerComplete = true }
            stateLock.unlock()
            return
        }
        let ps = pageSize
        var idx: [Int] = []
        for p in (s.start / ps)...((end - 1) / ps) { idx.append(p) }
        let eagerRanges = clampPageRanges(
            coalescePageIndices(idx, gapPages: coalesceGapPages, pageSize: ps),
            fileSize: Int64(fileSize)
        )
        for pr in eagerRanges {
            stateLock.lock()
            let cancelled = eagerCancelled
            stateLock.unlock()
            if cancelled { return }
            cacheLock.lock()
            try? fetchCoalescedRangeLocked(pr)
            cacheLock.unlock()
        }
        stateLock.lock()
        if !eagerCancelled { eagerComplete = true }
        stateLock.unlock()
    }
    private func speculativePrefetch() {
        let pred = detector.predictNext(depth: prefetchDepth, recordCount: recordCount())
        guard !pred.isEmpty else { return }
        var pages: [Int] = []; var seen = Set<Int>()
        cacheLock.lock()
        for i in pred {
            let off = recordOffset(i); guard off >= 0 else { continue }
            let p = Int(off / Int64(pageSize)); guard seen.insert(p).inserted else { continue }
            if !cache.has(p) && !inFlight.has(p) { pages.append(p) }
        }
        cacheLock.unlock()
        guard !pages.isEmpty else { return }
        let ranges = clampPageRanges(
            coalescePageIndices(pages, gapPages: coalesceGapPages, pageSize: pageSize),
            fileSize: Int64(fileSize)
        )
        DispatchQueue.global(qos: .utility).async { [weak self] in
            guard let self else { return }
            for pr in ranges {
                self.stateLock.lock()
                let closed = self.closed
                self.stateLock.unlock()
                if closed { return }
                self.cacheLock.lock()
                try? self.fetchCoalescedRangeLocked(pr)
                self.cacheLock.unlock()
            }
        }
    }
    func fetchCoalescedRangeLocked(_ pr: PageRange) throws {
        fetchesIssued += 1
        let blob = try fetchRange(pr.byteStart, pr.byteLength)
        let ps = Int64(pageSize)
        for p in pr.pageStart...pr.pageEnd {
            if cache.has(p) { continue }
            let off = Int64(p) * ps - pr.byteStart
            var len = ps; if off + len > Int64(blob.count) { len = Int64(blob.count) - off }
            guard len > 0 else { continue }
            cache.set(p, data: blob.subdata(in: Int(off)..<Int(off + len)), pinned: false)
        }
    }
}
