import XCTest
@testable import NXS

final class PrefetchTests: XCTestCase {

    private func buildCompactRecords(_ n: Int) -> Data {
        let schema = NXSSchema(keys: ["id", "tag"])
        let w = NXSWriter(schema: schema)
        for i in 0..<n {
            w.beginObject()
            w.writeI64(slot: 0, value: Int64(i))
            w.writeStr(slot: 1, value: "r\(i)")
            w.endObject()
        }
        return Data(w.finish())
    }

    private func buildPrefetchRecords(_ n: Int) -> Data {
        let schema = NXSSchema(keys: ["id", "username", "score", "active"])
        let w = NXSWriter(schema: schema)
        for i in 0..<n {
            w.beginObject()
            w.writeI64(slot: 0, value: Int64(i))
            w.writeStr(slot: 1, value: "user_\(i)")
            w.writeF64(slot: 2, value: Double(i) * 0.25)
            w.writeBool(slot: 3, value: i % 2 == 0)
            w.endObject()
        }
        return Data(w.finish())
    }

    func testCoalescePageIndices() {
        let ranges = coalescePageIndices([3, 4, 6, 7, 12], gapPages: 1, pageSize: defaultPageSize)
        XCTAssertEqual(ranges.count, 3)
        XCTAssertEqual(ranges[0].pageStart, 3)
        XCTAssertEqual(ranges[0].pageEnd, 4)
        XCTAssertEqual(ranges[1].pageStart, 6)
        XCTAssertEqual(ranges[1].pageEnd, 7)
        XCTAssertEqual(ranges[2].pageStart, 12)
        XCTAssertEqual(ranges[2].pageEnd, 12)
        XCTAssertEqual(ranges[0].byteLength, Int64(2 * defaultPageSize))

        let deduped = coalescePageIndices([3, 3, 4], gapPages: 1, pageSize: defaultPageSize)
        XCTAssertEqual(deduped.count, 1)
        XCTAssertEqual(deduped[0].pageStart, 3)
        XCTAssertEqual(deduped[0].pageEnd, 4)
    }

    func testPrefetchViewportCoalesce() throws {
        let buf = buildPrefetchRecords(60)
        var fetchRanges: [(Int64, Int64)] = []
        let r = try NXSReader(buf, options: NXSOpenOptions(
            maxPages: 64,
            coalesceGapPages: 1,
            fetchRange: { off, length in
                fetchRanges.append((off, length))
                return buf.subdata(in: Int(off)..<Int(off + length))
            }
        ))
        try r.prefetchViewport(startIndex: 0, endIndex: 49)
        XCTAssertLessThanOrEqual(fetchRanges.count, 3, "expected ≤3 coalesced fetches, got \(fetchRanges)")
        let stats = r.cacheStats()
        XCTAssertEqual(stats.fetchesIssued, fetchRanges.count)
    }

    func testPrefetchViewportBasic() throws {
        let buf = buildPrefetchRecords(50)
        let r = try NXSReader(buf)
        try r.prefetchViewport(startIndex: 0, endIndex: 49)
        let obj = try r.record(42)
        XCTAssertEqual(try obj.getI64("id"), 42)
    }

    func testMemoryEviction() throws {
        let buf = buildPrefetchRecords(20)
        let r = try NXSReader(buf, options: NXSOpenOptions(maxPages: 2, pageSize: 256, coalesceGapPages: 0))
        try r.prefetchViewport(startIndex: 0, endIndex: 0)
        try r.prefetchViewport(startIndex: 19, endIndex: 19)
        let stats = r.cacheStats()
        XCTAssertLessThanOrEqual(stats.pagesCached, 2)
    }

    func testDeduplication() throws {
        let buf = buildPrefetchRecords(10)
        let callLock = NSLock()
        var calls = 0
        let r = try NXSReader(buf, options: NXSOpenOptions(
            maxPages: 8,
            fetchRange: { off, length in
                callLock.lock()
                calls += 1
                callLock.unlock()
                Thread.sleep(forTimeInterval: 0.005)
                return buf.subdata(in: Int(off)..<Int(off + length))
            }
        ))

        let group = DispatchGroup()
        for _ in 0..<2 {
            group.enter()
            DispatchQueue.global().async {
                try? r.prefetchViewport(startIndex: 0, endIndex: 4)
                group.leave()
            }
        }
        group.wait()
        XCTAssertLessThanOrEqual(calls, 3, "too many fetches: \(calls)")
    }

    func testAccessHintStored() throws {
        let buf = buildPrefetchRecords(5)
        let r = try NXSReader(buf, options: NXSOpenOptions(hint: .sequential))
        XCTAssertEqual(r.accessHint, .sequential)
        let stats = r.cacheStats()
        XCTAssertEqual(stats.strategy, "adaptive")
        XCTAssertEqual(stats.pattern, "unknown")
        XCTAssertEqual(stats.pagesMax, defaultMaxPages)
    }

    func testPatternUnknownUntilMinObservations() {
        let d = AccessPatternDetector()
        for i in 0..<8 { d.observe(i) }
        XCTAssertEqual(d.pattern(), .unknown)
        d.observe(8)
        XCTAssertNotEqual(d.pattern(), .unknown)
    }

    func testPatternSequential() {
        let d = AccessPatternDetector()
        for i in 0..<20 { d.observe(i) }
        XCTAssertEqual(d.pattern(), .sequential)
    }

    func testPatternRandom() {
        let d = AccessPatternDetector()
        for i in 0..<8 { d.observe(i) }
        for k in 0..<12 { d.observe(k * 200) }
        XCTAssertEqual(d.pattern(), .random)
    }

    func testPredictNextSequential() {
        let d = AccessPatternDetector()
        for i in 0..<10 { d.observe(i) }
        XCTAssertEqual(d.predictNext(depth: 4, recordCount: 100), [10, 11, 12, 13])
    }

    func testPauseStopsSpeculative() throws {
        let buf = buildCompactRecords(200)
        let r = try NXSReader(buf)
        for i in 0..<25 { _ = try r.record(i) }
        XCTAssertEqual(r.cacheStats().pattern, "sequential")
        let before = r.cacheStats().fetchesIssued
        r.pausePrefetch()
        _ = try r.record(26)
        XCTAssertEqual(r.cacheStats().fetchesIssued, before)
        r.resumePrefetch()
        _ = try r.record(27)
        XCTAssertGreaterThanOrEqual(r.cacheStats().fetchesIssued, before)
    }

    func testHintFullEagerAtOpen() throws {
        let buf = buildPrefetchRecords(200)
        let r = try NXSReader(buf, options: NXSOpenOptions(hint: .full))
        r.warmup()
        XCTAssertEqual(r.cacheStats().strategy, "eager")
    }

    func testSequentialUpgradeToEager() throws {
        let buf = buildPrefetchRecords(200)
        let r = try NXSReader(buf)
        for i in 0..<150 { _ = try r.record(i) }
        r.warmup()
        let stats = r.cacheStats()
        XCTAssertEqual(stats.strategy, "eager")
        XCTAssertEqual(stats.pattern, "sequential")
    }

    func testPrefetchColumnSingleFetch() throws {
        let candidates = [
            URL(fileURLWithPath: #filePath)
                .deletingLastPathComponent()
                .deletingLastPathComponent()
                .deletingLastPathComponent()
                .appendingPathComponent("../../nyxis/conformance/columnar_flat8_dense_100.nxb"),
            URL(fileURLWithPath: #filePath)
                .deletingLastPathComponent()
                .appendingPathComponent("../../../conformance/columnar_flat8_dense_100.nxb"),
        ]
        guard let url = candidates.first(where: { FileManager.default.fileExists(atPath: $0.path) }) else {
            throw XCTSkip("columnar_flat8_dense_100.nxb not found")
        }
        let data = try Data(contentsOf: url)
        var fetches = 0
        let r = try NXSReader(data, options: NXSOpenOptions(fetchRange: { off, len in
            fetches += 1
            return Data(data[Int(off)..<(Int(off) + Int(len))])
        }))
        try r.prefetchColumn("score")
        XCTAssertEqual(fetches, 1)
        let sum = try r.colSumF64(key: "score")
        XCTAssertEqual(sum, 2475.0, accuracy: 1e-6)
        try r.prefetchColumn("score")
        XCTAssertEqual(fetches, 1)
        XCTAssertEqual(r.cacheStats().columnFetchesIssued, 1)
    }
}
