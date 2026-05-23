// Adaptive prefetch — page cache, range coalescing, in-flight dedup (spec §6–§8.4).

import Foundation

public let defaultPageSize: Int = 65536
public let defaultMaxPages: Int = 128
public let defaultCoalesceGapPages: Int = 1

/// Advisory access hint (stored only in phase 1).
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
    /// Injectable byte-range fetcher (tests or remote I/O). Default copies from buffer.
    public var fetchRange: ((Int64, Int64) throws -> Data)?

    public init(
        hint: AccessHint = .unknown,
        maxPages: Int = defaultMaxPages,
        pageSize: Int = defaultPageSize,
        coalesceGapPages: Int = defaultCoalesceGapPages,
        fetchRange: ((Int64, Int64) throws -> Data)? = nil
    ) {
        self.hint = hint
        self.maxPages = maxPages
        self.pageSize = pageSize
        self.coalesceGapPages = coalesceGapPages
        self.fetchRange = fetchRange
    }
}

/// Merge sorted unique page indices when the gap between consecutive indices is at most gapPages.
public func coalescePageIndices(_ indices: [Int], gapPages: Int, pageSize: Int) -> [PageRange] {
    guard !indices.isEmpty else { return [] }
    var seen = Set<Int>()
    var uniq: [Int] = []
    for p in indices {
        if seen.insert(p).inserted { uniq.append(p) }
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

/// Prefetch state owned by NXSReader.
final class PrefetchState {
    var hint: AccessHint
    var pageSize: Int
    var coalesceGapPages: Int
    let cache: PageCache
    let inFlight: InFlightMap
    var fetchesIssued = 0
    let strategy = "lazy"
    let pattern = "unknown"
    var fetchRange: (Int64, Int64) throws -> Data

    init(options: NXSOpenOptions, data: Data) {
        hint = options.hint
        pageSize = options.pageSize
        coalesceGapPages = options.coalesceGapPages
        cache = PageCache(maxPages: options.maxPages, pageSize: options.pageSize)
        inFlight = InFlightMap()
        if let custom = options.fetchRange {
            fetchRange = custom
        } else {
            let buf = data
            fetchRange = { off, length in
                let end = off + length
                guard off >= 0, end <= Int64(buf.count) else {
                    throw NXSError.outOfBounds("fetch range [\(off), \(end))")
                }
                return buf.subdata(in: Int(off)..<Int(end))
            }
        }
    }
}
