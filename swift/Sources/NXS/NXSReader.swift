// NXS Reader — zero-copy .nxb parser for Swift 5.9+
// Implements the Nyxis v1.1 binary wire format spec.
//
// Usage:
//   let data = try Data(contentsOf: url)
//   let reader = try NXSReader(data)
//   let obj = try reader.record(42)
//   let id: Int64 = try obj.getI64("id")

import Foundation

// ── Error ─────────────────────────────────────────────────────────────────────

public enum NXSError: Error {
    case badMagic(String)
    case outOfBounds(String)
    case keyNotFound(String)
    case fieldAbsent(String)
    case invalidFlags(String)
    case incompatibleFlags(String)
    case invalidPageMagic(String)

    /// Conformance error code string (matches nyxis/conformance expected.json).
    public var code: String {
        switch self {
        case .badMagic(let msg):
            if msg.contains("DICT_MISMATCH") { return "ERR_DICT_MISMATCH" }
            return "ERR_BAD_MAGIC"
        case .outOfBounds: return "ERR_OUT_OF_BOUNDS"
        case .keyNotFound: return "ERR_KEY_NOT_FOUND"
        case .fieldAbsent: return "ERR_FIELD_ABSENT"
        case .invalidFlags: return "ERR_INVALID_FLAGS"
        case .incompatibleFlags: return "ERR_INCOMPATIBLE_FLAGS"
        case .invalidPageMagic: return "ERR_INVALID_PAGE_MAGIC"
        }
    }
}

// ── Constants ─────────────────────────────────────────────────────────────────

private let magicFile: UInt32 = 0x4E595842
private let magicObj: UInt32 = 0x4E59584F
private let magicList: UInt32 = 0x4E59584C
private let magicFooter: UInt32 = 0x2153584E
private let flagSchema: UInt16 = 0x0002

// ── Little-endian helpers ─────────────────────────────────────────────────────

func rdU16(_ data: Data, _ off: Int) -> UInt16 {
    data[off..<(off+2)].withUnsafeBytes { $0.loadUnaligned(as: UInt16.self) }.littleEndian
}
func rdU32(_ data: Data, _ off: Int) -> UInt32 {
    data[off..<(off+4)].withUnsafeBytes { $0.loadUnaligned(as: UInt32.self) }.littleEndian
}
func rdU64(_ data: Data, _ off: Int) -> UInt64 {
    data[off..<(off+8)].withUnsafeBytes { $0.loadUnaligned(as: UInt64.self) }.littleEndian
}
func rdI64(_ data: Data, _ off: Int) -> Int64 {
    Int64(bitPattern: rdU64(data, off))
}
func rdF64(_ data: Data, _ off: Int) -> Double {
    Double(bitPattern: rdU64(data, off))
}

// ── Reader ────────────────────────────────────────────────────────────────────

public final class NXSReader {
    let data: Data
    var col: ColLayoutState = ColLayoutState()

    public let version: UInt16
    public let flags: UInt16
    public let dictHash: UInt64
    public let tailPtr: UInt64
    public let keys: [String]
    public let keySigils: [UInt8]
    let keyIndex: [String: Int]
    public var recordCount: Int { col.recordCount }
    public var tailStart: Int { col.tailStart }

    let prefetchLock = NSLock()
    let prefetch: PrefetchState
    var columnWarm: ColumnWarmState?

    public init(_ data: Data, options: NXSOpenOptions = NXSOpenOptions()) throws {
        self.data = data
        let size = data.count
        guard size >= 32 else { throw NXSError.outOfBounds("file too small") }
        guard rdU32(data, 0) == magicFile else { throw NXSError.badMagic("preamble") }
        guard rdU32(data, size - 4) == magicFooter else { throw NXSError.badMagic("footer") }

        version  = rdU16(data, 4)
        flags    = rdU16(data, 6)
        dictHash = rdU64(data, 8)
        let preambleTailPtr = rdU64(data, 16)
        if preambleTailPtr == 0 && size < 44 {
            throw NXSError.outOfBounds("stream footer")
        }
        var resolvedTailPtr = preambleTailPtr != 0 ? preambleTailPtr : rdU64(data, size - 12)

        var ks: [String] = []
        var kSigils: [UInt8] = []
        var kIndex: [String: Int] = [:]

        if flags & flagSchema != 0 {
            (ks, kSigils, kIndex) = try Self.parseEmbeddedSchema(data: data, size: size, dictHash: dictHash)
        }

        keys      = ks
        keySigils = kSigils
        keyIndex  = kIndex

        col = try Self.parseLayoutTail(data: data, flags: flags, preambleTailPtr: preambleTailPtr, keyCount: ks.count)
        if col.layout == .columnar {
            resolvedTailPtr = rdU64(data, size - 20)
        } else if col.layout == .pax {
            resolvedTailPtr = rdU64(data, size - 28)
        }
        tailPtr = resolvedTailPtr
        let tail = col.tailStart
        let count = col.recordCount
        prefetch = PrefetchState(
            options: options, data: data, fileSize: size, tailStart: tail,
            recordCount: { count },
            recordOffset: { i in Int64(rdU64(data, tail + i * 10 + 2)) }
        )
        prefetch.startEagerBackgroundIfNeeded()
        if col.layout == .columnar {
            let colFetch: ((Int, Int) throws -> Data)? = options.fetchRange.map { fr in
                { off, len in try fr(Int64(off), Int64(len)) }
            }
            columnWarm = ColumnWarmState(data: data, fetchRange: colFetch)
        }
    }

    public func slot(_ key: String) throws -> Int {
        guard let s = keyIndex[key] else { throw NXSError.keyNotFound(key) }
        return s
    }

    public func record(_ i: Int) throws -> NYXObject {
        guard i >= 0 && i < recordCount else {
            throw NXSError.outOfBounds("record \(i) out of [0, \(recordCount))")
        }
        if col.layout != .row {
            return NYXObject(reader: self, offset: i, recordIndex: UInt32(i))
        }
        prefetchOnAccess(i)
        let entryOff = tailStart + i * 10 + 2
        let absOff = Int(rdU64(data, entryOff))
        return NYXObject(reader: self, offset: absOff, recordIndex: 0)
    }

    // ── Bulk reducers — operate on a raw pointer to avoid Data subscript overhead

    public func sumF64(_ key: String) throws -> Double {
        if col.layout != .row { return try colSumF64(key: key) }
        let s = try slot(key)
        return data.withUnsafeBytes { ptr in
            let base = ptr.baseAddress!.assumingMemoryBound(to: UInt8.self)
            let size = ptr.count
            var sum = 0.0
            for i in 0..<recordCount {
                let abs = Int(rawU64(base, tailStart + i * 10 + 2))
                if let off = rawScanOffset(base, size: size, objOffset: abs, slot: s) {
                    sum += rawF64(base, off)
                }
            }
            return sum
        }
    }

    public func sumI64(_ key: String) throws -> Int64 {
        let s = try slot(key)
        return data.withUnsafeBytes { ptr in
            let base = ptr.baseAddress!.assumingMemoryBound(to: UInt8.self)
            let size = ptr.count
            var sum: Int64 = 0
            for i in 0..<recordCount {
                let abs = Int(rawU64(base, tailStart + i * 10 + 2))
                if let off = rawScanOffset(base, size: size, objOffset: abs, slot: s) {
                    sum &+= rawI64(base, off)
                }
            }
            return sum
        }
    }

    public func minF64(_ key: String) throws -> Double? {
        let s = try slot(key)
        return data.withUnsafeBytes { ptr in
            let base = ptr.baseAddress!.assumingMemoryBound(to: UInt8.self)
            let size = ptr.count
            var m = Double.infinity; var have = false
            for i in 0..<recordCount {
                let abs = Int(rawU64(base, tailStart + i * 10 + 2))
                guard let off = rawScanOffset(base, size: size, objOffset: abs, slot: s) else { continue }
                let v = rawF64(base, off); if v < m { m = v; have = true }
            }
            return have ? m : nil
        }
    }

    public func maxF64(_ key: String) throws -> Double? {
        let s = try slot(key)
        return data.withUnsafeBytes { ptr in
            let base = ptr.baseAddress!.assumingMemoryBound(to: UInt8.self)
            let size = ptr.count
            var m = -Double.infinity; var have = false
            for i in 0..<recordCount {
                let abs = Int(rawU64(base, tailStart + i * 10 + 2))
                guard let off = rawScanOffset(base, size: size, objOffset: abs, slot: s) else { continue }
                let v = rawF64(base, off); if v > m { m = v; have = true }
            }
            return have ? m : nil
        }
    }

    // ── Raw-pointer helpers (used only in bulk loops) ─────────────────────────

    private func rawU16(_ p: UnsafePointer<UInt8>, _ off: Int) -> Int {
        Int(p[off]) | (Int(p[off+1]) << 8)
    }
    private func rawU64(_ p: UnsafePointer<UInt8>, _ off: Int) -> UInt64 {
        let a = UInt64(p[off  ]); let b = UInt64(p[off+1]); let c = UInt64(p[off+2]); let d = UInt64(p[off+3])
        let e = UInt64(p[off+4]); let f = UInt64(p[off+5]); let g = UInt64(p[off+6]); let h = UInt64(p[off+7])
        return a | b<<8 | c<<16 | d<<24 | e<<32 | f<<40 | g<<48 | h<<56
    }
    private func rawI64(_ p: UnsafePointer<UInt8>, _ off: Int) -> Int64 {
        Int64(bitPattern: rawU64(p, off))
    }
    private func rawF64(_ p: UnsafePointer<UInt8>, _ off: Int) -> Double {
        Double(bitPattern: rawU64(p, off))
    }

    // Locate the value byte-offset for `slot` inside the object at `objOffset`.
    // Works entirely through the raw pointer — no Data subscript, no ARC.
    private func rawScanOffset(_ p: UnsafePointer<UInt8>, size: Int, objOffset: Int, slot: Int) -> Int? {
        var pos = objOffset + 8
        var cur = 0, tableIdx = 0, b: UInt8 = 0
        while true {
            guard pos < size else { return nil }
            b = p[pos]; pos += 1
            let bits = b & 0x7F
            for i in 0..<7 {
                if cur == slot {
                    guard (bits >> i) & 1 == 1 else { return nil }
                    while b & 0x80 != 0 { b = p[pos]; pos += 1 }
                    let ot = pos + tableIdx * 2
                    guard ot + 2 <= size else { return nil }
                    return objOffset + rawU16(p, ot)
                }
                if cur < slot && (bits >> i) & 1 == 1 { tableIdx += 1 }
                cur += 1
            }
            if b & 0x80 == 0 { return nil }
        }
    }

    // Internal: locate value offset for slot — used by NYXObject (Data path, fine for single-field access).
    func scanOffset(_ objOffset: Int, slot: Int) -> Int? {
        var p = objOffset + 8
        var cur = 0, tableIdx = 0
        var b: UInt8 = 0
        while true {
            guard p < data.count else { return nil }
            b = data[p]; p += 1
            let bits = b & 0x7F
            for i in 0..<7 {
                if cur == slot {
                    guard (bits >> i) & 1 == 1 else { return nil }
                    while b & 0x80 != 0 { b = data[p]; p += 1 }
                    let ot = p + tableIdx * 2
                    guard ot + 2 <= data.count else { return nil }
                    let rel = Int(rdU16(data, ot))
                    return objOffset + rel
                }
                if cur < slot && (bits >> i) & 1 == 1 { tableIdx += 1 }
                cur += 1
            }
            if b & 0x80 == 0 { return nil }
        }
    }

    // Internal accessor for NYXObject
    func rawData() -> Data { data }
    func keyIdx() -> [String: Int] { keyIndex }
    func kSigils() -> [UInt8] { keySigils }

    private static func parseEmbeddedSchema(
        data: Data, size: Int, dictHash: UInt64
    ) throws -> ([String], [UInt8], [String: Int]) {
        var off = 32
        guard off + 2 <= size else { throw NXSError.outOfBounds("schema") }
        let keyCount = Int(rdU16(data, off)); off += 2
        guard off + keyCount <= size else { throw NXSError.outOfBounds("type manifest") }
        let kSigils = Array(data[off..<(off + keyCount)]); off += keyCount
        var ks: [String] = []
        var kIndex: [String: Int] = [:]
        for i in 0..<keyCount {
            var end = off
            while end < size && data[end] != 0 { end += 1 }
            guard end < size else { throw NXSError.outOfBounds("string pool") }
            let name = String(bytes: data[off..<end], encoding: .utf8) ?? ""
            ks.append(name)
            kIndex[name] = i
            off = end + 1
        }
        if off % 8 != 0 { off += 8 - (off % 8) }
        if nxsMurmur3_64(data, 32, off - 32) != dictHash {
            throw NXSError.badMagic("ERR_DICT_MISMATCH: schema hash mismatch")
        }
        return (ks, kSigils, kIndex)
    }

}

private func nxsMurmur3_64(_ data: Data, _ off: Int, _ len: Int) -> UInt64 {
    let C1: UInt64 = 0xFF51AFD7ED558CCD
    let C2: UInt64 = 0xC4CEB9FE1A85EC53
    var h: UInt64 = 0x93681D6255313A99
    var p = off
    let end = off + len
    while p < end {
        var k: UInt64 = 0
        for i in 0..<8 where p + i < end {
            k |= UInt64(data[p + i]) << (i * 8)
        }
        k = k &* C1; k ^= k >> 33
        h ^= k
        h = h &* C2; h ^= h >> 33
        p += 8
    }
    h ^= UInt64(len); h ^= h >> 33
    h = h &* C1; h ^= h >> 33
    return h
}

// ── Object ────────────────────────────────────────────────────────────────────

public final class NYXObject {
    private let reader: NXSReader
    private let offset: Int
    private let recordIndex: UInt32
    private var staged = false
    private var bitmaskStart      = 0
    private var offsetTableStart  = 0

    init(reader: NXSReader, offset: Int, recordIndex: UInt32) {
        self.reader = reader
        self.offset = offset
        self.recordIndex = recordIndex
    }

    private func objAtNyxo() -> Bool {
        if offset + 4 > reader.data.count { return false }
        return rdU32(reader.data, offset) == magicObj
    }

    /// Columnar/PAX top-level records use record index; nested NYXO blobs use row paths.
    private func usesColumnarFieldAccess() -> Bool {
        reader.col.layout != .row && !objAtNyxo()
    }

    private func locateBitmask() throws {
        guard !staged else { return }
        let data = reader.rawData()
        let size = data.count
        guard offset + 8 <= size else { throw NXSError.outOfBounds("object header") }
        guard rdU32(data, offset) == magicObj else {
            throw NXSError.badMagic("object at \(offset)")
        }
        var p = offset + 8
        bitmaskStart = p
        while p < size && (data[p] & 0x80) != 0 { p += 1 }
        guard p < size else { throw NXSError.outOfBounds("bitmask") }
        p += 1
        offsetTableStart = p
        staged = true
    }

    private func resolveSlot(_ slot: Int) throws -> Int? {
        try locateBitmask()
        let data = reader.rawData()
        let size = data.count
        var p = bitmaskStart
        var cur = 0, tableIdx = 0
        var b: UInt8 = 0
        while true {
            guard p < size else { return nil }
            b = data[p]; p += 1
            let bits = b & 0x7F
            for i in 0..<7 {
                if cur == slot {
                    guard (bits >> i) & 1 == 1 else { return nil }
                    while b & 0x80 != 0 { b = data[p]; p += 1 }
                    let ot = offsetTableStart + tableIdx * 2
                    guard ot + 2 <= size else { return nil }
                    return offset + Int(rdU16(data, ot))
                }
                if cur < slot && (bits >> i) & 1 == 1 { tableIdx += 1 }
                cur += 1
            }
            if b & 0x80 == 0 { return nil }
        }
    }

    public func getI64(_ key: String) throws -> Int64 {
        let s = try reader.slot(key)
        return try getI64BySlot(s)
    }
    public func getF64(_ key: String) throws -> Double {
        let s = try reader.slot(key)
        return try getF64BySlot(s)
    }
    public func getBool(_ key: String) throws -> Bool {
        let s = try reader.slot(key)
        return try getBoolBySlot(s)
    }
    public func getStr(_ key: String) throws -> String {
        let s = try reader.slot(key)
        return try getStrBySlot(s)
    }

    public func getI64BySlot(_ slot: Int) throws -> Int64 {
        if usesColumnarFieldAccess() {
            guard let cell = reader.colNumericBytes(rec: recordIndex, slot: slot) else {
                throw NXSError.fieldAbsent("slot \(slot)")
            }
            return cell.withUnsafeBytes { $0.load(as: Int64.self).littleEndian }
        }
        guard let off = try resolveSlot(slot) else { throw NXSError.fieldAbsent("slot \(slot)") }
        return rdI64(reader.rawData(), off)
    }
    public func getF64BySlot(_ slot: Int) throws -> Double {
        if usesColumnarFieldAccess() {
            guard let cell = reader.colNumericBytes(rec: recordIndex, slot: slot) else {
                throw NXSError.fieldAbsent("slot \(slot)")
            }
            return cell.withUnsafeBytes { Double(bitPattern: $0.load(as: UInt64.self).littleEndian) }
        }
        guard let off = try resolveSlot(slot) else { throw NXSError.fieldAbsent("slot \(slot)") }
        return rdF64(reader.rawData(), off)
    }
    public func getBoolBySlot(_ slot: Int) throws -> Bool {
        if usesColumnarFieldAccess() {
            guard let cell = reader.colNumericBytes(rec: recordIndex, slot: slot) else {
                throw NXSError.fieldAbsent("slot \(slot)")
            }
            return cell[cell.startIndex] != 0
        }
        guard let off = try resolveSlot(slot) else { throw NXSError.fieldAbsent("slot \(slot)") }
        return reader.rawData()[off] != 0
    }
    public func getStrBySlot(_ slot: Int) throws -> String {
        if usesColumnarFieldAccess() {
            let sigils = reader.kSigils()
            guard slot >= 0 && slot < sigils.count && sigils[slot] == 0x22 else {
                throw NXSError.fieldAbsent("slot \(slot)")
            }
            let (s, ok) = reader.colGetStr(key: reader.keys[slot], recordIndex: recordIndex)
            guard ok else { throw NXSError.fieldAbsent("slot \(slot)") }
            return s
        }
        guard let off = try resolveSlot(slot) else { throw NXSError.fieldAbsent("slot \(slot)") }
        let data = reader.rawData()
        let len  = Int(rdU32(data, off))
        return String(bytes: data[off+4..<(off+4+len)], encoding: .utf8) ?? ""
    }
}
