// NXS Writer — direct-to-buffer .nxb emitter for Swift 5.9+.
//
// Mirrors the Rust NxsWriter API:
//   NXSSchema — precompile keys once; share across NXSWriter instances.
//   NXSWriter — slot-based hot path; no per-key dictionary lookups during write.
//
// Usage:
//   import NXS
//
//   let schema = NXSSchema(keys: ["id", "username", "score", "active"])
//   let w = NXSWriter(schema: schema)
//   w.beginObject()
//   w.writeI64(slot: 0, value: 42)
//   w.writeStr(slot: 1, value: "alice")
//   w.writeF64(slot: 2, value: 9.5)
//   w.writeBool(slot: 3, value: true)
//   w.endObject()
//   let bytes: [UInt8] = w.finish()

import Foundation

// ── MurmurHash3-64 ────────────────────────────────────────────────────────────

private func murmur3_64(_ data: [UInt8]) -> UInt64 {
    let C1: UInt64 = 0xFF51AFD7ED558CCD
    let C2: UInt64 = 0xC4CEB9FE1A85EC53
    var h: UInt64 = 0x93681D6255313A99
    let len = data.count
    var i = 0
    while i < len {
        var k: UInt64 = 0
        for b in 0..<8 where i + b < len {
            k |= UInt64(data[i + b]) << (b * 8)
        }
        k = k &* C1; k ^= k >> 33
        h ^= k
        h = h &* C2; h ^= h >> 33
        i += 8
    }
    h ^= UInt64(len); h ^= h >> 33
    h = h &* C1;      h ^= h >> 33
    return h
}

// ── Schema ────────────────────────────────────────────────────────────────────

public final class NXSSchema {
    public let keys: [String]
    let bitmaskBytes: Int

    public init(keys: [String]) {
        self.keys = keys
        self.bitmaskBytes = max(1, (keys.count + 6) / 7)
    }

    var count: Int { keys.count }
}

// ── Frame ─────────────────────────────────────────────────────────────────────

private struct Frame {
    let start: Int
    var bitmask: [UInt8]
    var offsetTable: [Int]      // relative offsets, write order
    var slotOffsets: [(Int, Int)] // (slot, bufPos)
    var lastSlot: Int = -1
    var needsSort: Bool = false

    init(start: Int, bitmaskBytes: Int) {
        self.start = start
        self.bitmask = [UInt8](repeating: 0, count: bitmaskBytes)
        for i in 0..<(bitmaskBytes - 1) { self.bitmask[i] = 0x80 }
        self.offsetTable = []
        self.slotOffsets = []
    }
}

// ── Writer ────────────────────────────────────────────────────────────────────

// ── Sigil constants ────────────────────────────────────────────────────────────

private let SIGIL_STR:    UInt8 = 0x22 // '"' — string / var-length
private let SIGIL_I64:    UInt8 = 0x69 // 'i'
private let SIGIL_F64:    UInt8 = 0x64 // 'd'
private let SIGIL_BOOL:   UInt8 = 0x62 // 'b'
private let SIGIL_NULL:   UInt8 = 0x6E // 'n'
private let SIGIL_BINARY: UInt8 = 0x42 // 'B'

public final class NXSWriter {
    private let schema: NXSSchema
    private var buf: [UInt8] = []
    private var frames: [Frame] = []
    private var recordOffsets: [Int] = []
    // Sigil per slot: default str/var-length; updated on each typed write
    private var slotSigils: [UInt8]

    public init(schema: NXSSchema) {
        self.schema = schema
        self.slotSigils = [UInt8](repeating: SIGIL_STR, count: schema.count)
        buf.reserveCapacity(4096)
    }

    public func beginObject() {
        if frames.isEmpty { recordOffsets.append(buf.count) }
        let start = buf.count
        let frame = Frame(start: start, bitmaskBytes: schema.bitmaskBytes)

        appendU32(0x4E59584F)   // NYXO
        appendU32(0)            // length placeholder
        buf.append(contentsOf: frame.bitmask)
        buf.append(contentsOf: [UInt8](repeating: 0, count: schema.count * 2))
        while (buf.count - start) % 8 != 0 { buf.append(0) }

        frames.append(frame)
    }

    public func endObject() {
        precondition(!frames.isEmpty, "endObject without beginObject")
        let frame = frames.removeLast()
        let totalLen = buf.count - frame.start

        // Back-patch Length
        withUInt32LE(UInt32(totalLen)) { buf[$0 + frame.start + 4] = $1 }

        // Back-patch bitmask
        let bmOff = frame.start + 8
        for (i, b) in frame.bitmask.enumerated() { buf[bmOff + i] = b }

        // Back-patch offset table
        let otStart = bmOff + schema.bitmaskBytes
        let present = frame.offsetTable.count

        if !frame.needsSort {
            for (i, rel) in frame.offsetTable.enumerated() {
                let v = UInt16(rel)
                buf[otStart + i * 2]     = UInt8(v & 0xFF)
                buf[otStart + i * 2 + 1] = UInt8(v >> 8)
            }
        } else {
            let sorted = frame.slotOffsets.sorted { $0.0 < $1.0 }
            for (i, (_, bufPos)) in sorted.enumerated() {
                let v = UInt16(bufPos - frame.start)
                buf[otStart + i * 2]     = UInt8(v & 0xFF)
                buf[otStart + i * 2 + 1] = UInt8(v >> 8)
            }
        }
        for i in (present * 2)..<(schema.count * 2) { buf[otStart + i] = 0 }
    }

    public func finish() -> [UInt8] {
        precondition(frames.isEmpty, "unclosed objects")

        let schemaBytes = buildSchemaBytes()
        let dictHash    = murmur3_64(schemaBytes)
        let dataStart   = 32 + schemaBytes.count

        let dataSector  = buf
        let tailPtr     = UInt64(dataStart + dataSector.count)
        let tail        = buildTailIndex(dataStart: dataStart, tailPtr: tailPtr)

        var out = [UInt8]()
        out.reserveCapacity(32 + schemaBytes.count + dataSector.count + tail.count)

        appendU32To(&out, 0x4E595842)         // NYXB
        appendU16To(&out, 0x0101)             // VERSION
        appendU16To(&out, 0x0002)             // FLAG_SCHEMA_EMBEDDED
        appendU64To(&out, dictHash)
        appendU64To(&out, 0)
        out.append(contentsOf: [UInt8](repeating: 0, count: 8)) // reserved

        out.append(contentsOf: schemaBytes)
        out.append(contentsOf: dataSector)
        out.append(contentsOf: tail)
        return out
    }

    // ── Typed write methods ────────────────────────────────────────────────────

    public func writeI64(slot: Int, value: Int64) {
        slotSigils[slot] = SIGIL_I64
        markSlot(slot)
        appendI64(value)
    }

    public func writeF64(slot: Int, value: Double) {
        slotSigils[slot] = SIGIL_F64
        markSlot(slot)
        appendF64(value)
    }

    public func writeBool(slot: Int, value: Bool) {
        slotSigils[slot] = SIGIL_BOOL
        markSlot(slot)
        buf.append(value ? 0x01 : 0x00)
        buf.append(contentsOf: [UInt8](repeating: 0, count: 7))
    }

    public func writeTime(slot: Int, unixNs: Int64) {
        slotSigils[slot] = SIGIL_I64
        writeI64(slot: slot, value: unixNs)
    }

    public func writeNull(slot: Int) {
        slotSigils[slot] = SIGIL_NULL
        markSlot(slot)
        buf.append(contentsOf: [UInt8](repeating: 0, count: 8))
    }

    public func writeStr(slot: Int, value: String) {
        slotSigils[slot] = SIGIL_STR
        markSlot(slot)
        let bytes = Array(value.utf8)
        appendU32(UInt32(bytes.count))
        buf.append(contentsOf: bytes)
        let used = (4 + bytes.count) % 8
        if used != 0 { buf.append(contentsOf: [UInt8](repeating: 0, count: 8 - used)) }
    }

    public func writeBytes(slot: Int, value: [UInt8]) {
        slotSigils[slot] = SIGIL_BINARY
        markSlot(slot)
        appendU32(UInt32(value.count))
        buf.append(contentsOf: value)
        let used = (4 + value.count) % 8
        if used != 0 { buf.append(contentsOf: [UInt8](repeating: 0, count: 8 - used)) }
    }

    public func writeListI64(slot: Int, values: [Int64]) {
        markSlot(slot) // list is var-length — keep SIGIL_STR default
        let total = 16 + values.count * 8
        appendU32(0x4E59584C)                  // NYXL
        appendU32(UInt32(total))
        buf.append(0x3D)                       // '=' sigil
        appendU32(UInt32(values.count))
        buf.append(contentsOf: [0, 0, 0])
        for v in values { appendI64(v) }
    }

    public func writeListF64(slot: Int, values: [Double]) {
        markSlot(slot)
        let total = 16 + values.count * 8
        appendU32(0x4E59584C)
        appendU32(UInt32(total))
        buf.append(0x7E)                       // '~' sigil
        appendU32(UInt32(values.count))
        buf.append(contentsOf: [0, 0, 0])
        for v in values { appendF64(v) }
    }

    // Convenience: write records from [[String: Any]].
    public static func fromRecords(keys: [String],
                                   records: [[String: Any]]) -> [UInt8] {
        let schema = NXSSchema(keys: keys)
        let w = NXSWriter(schema: schema)
        for rec in records {
            w.beginObject()
            for (i, key) in keys.enumerated() {
                guard let val = rec[key] else { continue }
                switch val {
                case is NSNull:        w.writeNull(slot: i)
                case let v as Bool:    w.writeBool(slot: i, value: v)
                case let v as Int:     w.writeI64(slot: i, value: Int64(v))
                case let v as Int64:   w.writeI64(slot: i, value: v)
                case let v as Double:  w.writeF64(slot: i, value: v)
                case let v as String:  w.writeStr(slot: i, value: v)
                default: break
                }
            }
            w.endObject()
        }
        return w.finish()
    }

    // ── Private helpers ────────────────────────────────────────────────────────

    private func markSlot(_ slot: Int) {
        precondition(!frames.isEmpty, "write outside beginObject/endObject")
        let byteIdx = slot / 7
        let bitIdx  = slot % 7
        frames[frames.count - 1].bitmask[byteIdx] |= UInt8(1 << bitIdx)

        let rel = buf.count - frames[frames.count - 1].start
        if slot < frames[frames.count - 1].lastSlot {
            frames[frames.count - 1].needsSort = true
        }
        frames[frames.count - 1].lastSlot = slot
        frames[frames.count - 1].offsetTable.append(rel)
        frames[frames.count - 1].slotOffsets.append((slot, buf.count))
    }

    private func appendU32(_ v: UInt32) {
        buf.append(UInt8(v & 0xFF)); buf.append(UInt8((v >> 8) & 0xFF))
        buf.append(UInt8((v >> 16) & 0xFF)); buf.append(UInt8((v >> 24) & 0xFF))
    }

    private func appendU16(_ v: UInt16) {
        buf.append(UInt8(v & 0xFF)); buf.append(UInt8((v >> 8) & 0xFF))
    }

    private func appendI64(_ v: Int64) {
        let u = UInt64(bitPattern: v)
        for i in 0..<8 { buf.append(UInt8((u >> (i * 8)) & 0xFF)) }
    }

    private func appendF64(_ v: Double) {
        let u = v.bitPattern
        for i in 0..<8 { buf.append(UInt8((u >> (i * 8)) & 0xFF)) }
    }

    private func appendU32To(_ arr: inout [UInt8], _ v: UInt32) {
        arr.append(UInt8(v & 0xFF)); arr.append(UInt8((v >> 8) & 0xFF))
        arr.append(UInt8((v >> 16) & 0xFF)); arr.append(UInt8((v >> 24) & 0xFF))
    }

    private func appendU16To(_ arr: inout [UInt8], _ v: UInt16) {
        arr.append(UInt8(v & 0xFF)); arr.append(UInt8((v >> 8) & 0xFF))
    }

    private func appendU64To(_ arr: inout [UInt8], _ v: UInt64) {
        for i in 0..<8 { arr.append(UInt8((v >> (i * 8)) & 0xFF)) }
    }

    private func withUInt32LE(_ v: UInt32, _ body: (Int, UInt8) -> Void) {
        for i in 0..<4 { body(i, UInt8((v >> (i * 8)) & 0xFF)) }
    }

    private func buildSchemaBytes() -> [UInt8] {
        let n    = schema.count
        let utf8 = schema.keys.map { Array($0.utf8) }
        var size = 2 + n + utf8.reduce(0) { $0 + $1.count + 1 }
        let pad  = (8 - size % 8) % 8
        size += pad

        var b = [UInt8](repeating: 0, count: size)
        var p = 0
        b[p] = UInt8(n & 0xFF); p += 1
        b[p] = UInt8((n >> 8) & 0xFF); p += 1
        for i in 0..<n { b[p] = slotSigils[i]; p += 1 }
        for e in utf8 {
            for byte in e { b[p] = byte; p += 1 }
            b[p] = 0; p += 1
        }
        return b
    }

    private func buildTailIndex(dataStart: Int, tailPtr: UInt64) -> [UInt8] {
        let nr = recordOffsets.count
        var b  = [UInt8]()
        b.reserveCapacity(4 + nr * 10 + 12)

        appendU32To(&b, UInt32(nr))
        for (i, rel) in recordOffsets.enumerated() {
            let lo = UInt16(i & 0xFFFF)
            b.append(UInt8(lo & 0xFF)); b.append(UInt8(lo >> 8))
            let abs = UInt64(dataStart + rel)
            appendU64To(&b, abs)
        }
        appendU64To(&b, tailPtr)
        appendU32To(&b, 0x2153584E) // NXS!
        return b
    }
}
