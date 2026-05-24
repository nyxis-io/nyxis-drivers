// Columnar column-buffer warmup (Adaptive-prefetch-spec §7.4).

import Foundation

final class ColumnWarmState {
    private let data: Data
    private let fetch: (Int, Int) throws -> Data
    private var warmed = Set<Int>()
    private var overlay = [Int: Data]()
    private let lock = NSLock()
    private(set) var fetches = 0

    init(data: Data, fetchRange: ((Int, Int) throws -> Data)?) {
        self.data = data
        if let custom = fetchRange {
            fetch = custom
        } else {
            fetch = { off, len in
                guard off >= 0, off + len <= data.count else {
                    throw NXSError.outOfBounds("column fetch [\(off), \(off + len))")
                }
                return Data(data[off..<(off + len)])
            }
        }
    }

    func prefetchColumn(slot: Int, colOff: [UInt64], colLen: [UInt64]) throws {
        var off = 0
        var len = 0
        lock.lock()
        if warmed.contains(slot) {
            lock.unlock()
            return
        }
        off = Int(colOff[slot])
        len = Int(colLen[slot])
        if off < 0 || len < 0 || off + len > data.count {
            lock.unlock()
            throw NXSError.outOfBounds("column buffer")
        }
        lock.unlock()

        let blob = try fetch(off, len)

        lock.lock()
        defer { lock.unlock() }
        if warmed.contains(slot) { return }
        if off + blob.count > data.count {
            overlay[slot] = blob
        }
        warmed.insert(slot)
        fetches += 1
    }

    func sector(slot: Int, colOff: [UInt64], colLen: [UInt64]) throws -> Data {
        lock.lock()
        if let o = overlay[slot] {
            let need = Int(colLen[slot])
            if o.count >= need {
                lock.unlock()
                return Data(o[..<need])
            }
        }
        lock.unlock()
        let off = Int(colOff[slot])
        let length = Int(colLen[slot])
        guard off + length <= data.count else {
            throw NXSError.outOfBounds("column buffer")
        }
        return Data(data[off..<(off + length)])
    }
}
