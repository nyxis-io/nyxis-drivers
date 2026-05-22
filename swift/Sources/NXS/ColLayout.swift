// Columnar and PAX layout read paths (OLAP.md). Mirrors nyxis-drivers/go/col.go.

import Foundation

// ── Layout flags & constants ──────────────────────────────────────────────────

enum NXSLayout: Int {
    case row = 0
    case columnar = 1
    case pax = 2
}

private let flagColumnar: UInt16 = 0x0001
private let flagPAX: UInt16 = 0x0004
private let magicPage: UInt32 = 0x4E585350

private let footerRowBytes = 12
private let footerColBytes = 20
private let footerPaxBytes = 28
private let colTailEntryBytes = 20
private let paxTailEntryBytes = 28

// ── Reader layout state (filled by parseLayoutTail) ───────────────────────────

struct ColLayoutState {
    var layout: NXSLayout = .row
    var recordCount: Int = 0
    var tailStart: Int = 0
    var colBufOff: [UInt64] = []
    var colBufLen: [UInt64] = []
    var pageCount: UInt32 = 0
    var pageSizeHint: UInt32 = 0
    var pageIndex: [UInt32] = []
    var pageRecStart: [UInt64] = []
    var pageRecCount: [UInt32] = []
    var pageOffset: [UInt64] = []
    var pageLength: [UInt32] = []
}

// ── Layout tail parsing ───────────────────────────────────────────────────────

extension NXSReader {

    static func parseLayoutTail(
        data: Data,
        flags: UInt16,
        preambleTailPtr: UInt64,
        keyCount: Int
    ) throws -> ColLayoutState {
        var s = ColLayoutState()
        let size = data.count

        if (flags & flagColumnar) != 0 && (flags & flagPAX) != 0 {
            throw NXSError.invalidFlags("columnar and PAX both set")
        }
        if (flags & flagColumnar) != 0 && preambleTailPtr == 0 {
            throw NXSError.incompatibleFlags("columnar with TailPtr=0")
        }

        if (flags & flagColumnar) != 0 {
            s.layout = .columnar
            try parseColumnarFooter(data: data, keyCount: keyCount, into: &s)
            return s
        }
        if (flags & flagPAX) != 0 {
            s.layout = .pax
            try parsePAXFooter(data: data, into: &s)
            return s
        }

        s.layout = .row
        var tp = preambleTailPtr
        if tp == 0 {
            guard size >= 44 else { throw NXSError.outOfBounds("streamable footer") }
            tp = rdU64(data, size - footerRowBytes)
        }
        let tip = Int(tp)
        guard tip + 4 <= size else { throw NXSError.outOfBounds("tail index") }
        s.recordCount = Int(rdU32(data, tip))
        s.tailStart = tip + 4
        return s
    }

    private static func parseColumnarFooter(data: Data, keyCount: Int, into s: inout ColLayoutState) throws {
        let size = data.count
        guard size >= footerColBytes else { throw NXSError.outOfBounds("columnar footer") }
        let fo = size - footerColBytes
        let tailPtr = rdU64(data, fo)
        s.recordCount = Int(rdU64(data, fo + 8))
        s.tailStart = Int(tailPtr)
        s.colBufOff = Array(repeating: 0, count: keyCount)
        s.colBufLen = Array(repeating: 0, count: keyCount)
        for i in 0..<keyCount {
            let e = s.tailStart + i * colTailEntryBytes
            guard e + colTailEntryBytes <= size else {
                throw NXSError.outOfBounds("columnar tail entry")
            }
            let fid = Int(rdU16(data, e))
            guard fid >= 0 && fid < keyCount else {
                throw NXSError.outOfBounds("invalid field ID \(fid)")
            }
            s.colBufOff[fid] = rdU64(data, e + 4)
            s.colBufLen[fid] = rdU64(data, e + 12)
        }
    }

    private static func parsePAXFooter(data: Data, into s: inout ColLayoutState) throws {
        let size = data.count
        guard size >= footerPaxBytes else { throw NXSError.outOfBounds("PAX footer") }
        let fo = size - footerPaxBytes
        let tailPtr = rdU64(data, fo)
        s.recordCount = Int(rdU64(data, fo + 8))
        s.pageCount = rdU32(data, fo + 16)
        s.pageSizeHint = rdU32(data, fo + 20)
        s.tailStart = Int(tailPtr)
        guard s.pageCount > 0 else { return }

        let pc = Int(s.pageCount)
        s.pageIndex = Array(repeating: 0, count: pc)
        s.pageRecStart = Array(repeating: 0, count: pc)
        s.pageRecCount = Array(repeating: 0, count: pc)
        s.pageOffset = Array(repeating: 0, count: pc)
        s.pageLength = Array(repeating: 0, count: pc)

        for i in 0..<pc {
            let e = s.tailStart + i * paxTailEntryBytes
            guard e + paxTailEntryBytes <= size else {
                throw NXSError.outOfBounds("PAX tail entry")
            }
            s.pageIndex[i] = rdU32(data, e)
            s.pageRecStart[i] = rdU64(data, e + 4)
            s.pageRecCount[i] = rdU32(data, e + 12)
            s.pageOffset[i] = rdU64(data, e + 16)
            s.pageLength[i] = rdU32(data, e + 24)
        }
        let dlen = UInt64(size)
        for i in 0..<pc {
            let poff64 = s.pageOffset[i]
            if poff64 > dlen || poff64 + 4 > dlen || poff64 > UInt64(Int.max) {
                throw NXSError.outOfBounds("PAX page offset")
            }
            let poff = Int(poff64)
            if rdU32(data, poff) != magicPage {
                throw NXSError.invalidPageMagic("PAX page magic mismatch")
            }
        }
    }
}

// ── Shared helpers ────────────────────────────────────────────────────────────

func nxsNullBitmapBytes(_ n: UInt32) -> Int {
    let raw = Int((n + 7) / 8)
    return (raw + 7) & ~7
}

func nxsColBit(_ bm: Data, _ rec: UInt32) -> Bool {
    let i = Int(rec)
    return (bm[i / 8] >> (i % 8)) & 1 == 1
}

func nxsIsVarSigil(_ sig: UInt8) -> Bool {
    sig == 0x22 || sig == 0x3C // " and <
}

func nxsVarOffBytesLen(_ rc: UInt32) throws -> Int {
    let off = (UInt64(rc) + 1) * 4
    guard off <= UInt64(Int.max) else {
        throw NXSError.outOfBounds("var offsets overflow")
    }
    return Int(off)
}

func nxsFieldSectorLen(data: Data, sectorOff: Int, rc: UInt32, sigil: UInt8) throws -> Int {
    let bmLen = nxsNullBitmapBytes(rc)
    if !nxsIsVarSigil(sigil) {
        return bmLen + Int(rc) * 8
    }
    let offBytes = try nxsVarOffBytesLen(rc)
    if sectorOff + bmLen + offBytes > data.count {
        throw NXSError.outOfBounds("var offsets")
    }
    let end = Int(rdU32(data, sectorOff + bmLen + Int(rc) * 4))
    let total = bmLen + offBytes + end
    if sectorOff + total > data.count {
        throw NXSError.outOfBounds("var values")
    }
    return total
}

func nxsVarStrAt(offsets: Data, values: Data, recordIndex: UInt32) -> (String, Bool) {
    let need = (UInt64(recordIndex) + 2) * 4
    guard UInt64(offsets.count) >= need else { return ("", false) }
    let off = Int(recordIndex) * 4
    let start = Int(rdU32(offsets, off))
    let end = Int(rdU32(offsets, off + 4))
    guard end >= start && end <= values.count else { return ("", false) }
    let s = String(bytes: values[start..<end], encoding: .utf8) ?? ""
    return (s, true)
}

// ── Columnar / PAX field access on NXSReader ──────────────────────────────────

extension NXSReader {

    var layoutKind: NXSLayout { col.layout }

    func paxFindPage(_ rec: UInt32) -> (page: Int, local: Int, ok: Bool) {
        if col.pageCount == 0 { return (0, 0, false) }
        let r64 = UInt64(rec)
        var lo = 0
        var hi = Int(col.pageCount) - 1
        while lo <= hi {
            let mid = lo + (hi - lo) / 2
            let start = col.pageRecStart[mid]
            let count = UInt64(col.pageRecCount[mid])
            if r64 < start {
                hi = mid - 1
            } else if r64 >= start + count {
                lo = mid + 1
            } else {
                return (mid, Int(r64 - start), true)
            }
        }
        return (0, 0, false)
    }

    func colFieldParts(slot: Int) throws -> (bm: Data, vals: Data) {
        guard slot >= 0 && slot < col.colBufOff.count else {
            throw NXSError.keyNotFound("slot \(slot)")
        }
        let off = Int(col.colBufOff[slot])
        let length = Int(col.colBufLen[slot])
        guard off + length <= data.count else {
            throw NXSError.outOfBounds("column buffer")
        }
        let bmLen = nxsNullBitmapBytes(UInt32(col.recordCount))
        guard length >= bmLen else {
            throw NXSError.outOfBounds("null bitmap")
        }
        return (Data(data[off..<(off + bmLen)]), Data(data[(off + bmLen)..<(off + length)]))
    }

    func colVarParts(slot: Int) throws -> (bm: Data, offsets: Data, values: Data) {
        let (bm, tail) = try colFieldParts(slot: slot)
        let offBytes = try nxsVarOffBytesLen(UInt32(col.recordCount))
        guard tail.count >= offBytes else {
            throw NXSError.outOfBounds("var offsets")
        }
        return (bm, Data(tail[..<offBytes]), Data(tail[offBytes...]))
    }

    func pageFieldSector(pi: UInt32, slot: Int) -> Data? {
        let poff = Int(col.pageOffset[Int(pi)])
        guard poff + 24 <= data.count, rdU32(data, poff) == magicPage else { return nil }
        let fc = Int(rdU16(data, poff + 20))
        guard slot >= 0 && slot < fc && fc <= keySigils.count else { return nil }
        let rc = col.pageRecCount[Int(pi)]
        var body = poff + 24
        for fi in 0..<slot {
            var sig: UInt8 = 0x3D
            if fi < keySigils.count { sig = keySigils[fi] }
            guard let flen = try? nxsFieldSectorLen(data: data, sectorOff: body, rc: rc, sigil: sig) else {
                return nil
            }
            body += flen
        }
        var sig: UInt8 = 0x3D
        if slot < keySigils.count { sig = keySigils[slot] }
        guard let flen = try? nxsFieldSectorLen(data: data, sectorOff: body, rc: rc, sigil: sig),
              body + flen <= data.count else { return nil }
        return Data(data[body..<(body + flen)])
    }

    func pageFieldParts(pi: UInt32, slot: Int) -> (bm: Data, vals: Data)? {
        guard let sector = pageFieldSector(pi: pi, slot: slot) else { return nil }
        let bmLen = nxsNullBitmapBytes(col.pageRecCount[Int(pi)])
        guard sector.count >= bmLen else { return nil }
        return (Data(sector[..<bmLen]), Data(sector[bmLen...]))
    }

    func colVarPartsAt(rec: UInt32, slot: Int) -> (bm: Data, offsets: Data, values: Data, ok: Bool) {
        guard slot >= 0 && slot < keySigils.count, nxsIsVarSigil(keySigils[slot]) else {
            return (Data(), Data(), Data(), false)
        }
        if col.layout == .columnar {
            do {
                let (bm, off, val) = try colVarParts(slot: slot)
                return (bm, off, val, true)
            } catch {
                return (Data(), Data(), Data(), false)
            }
        }
        if col.layout == .pax {
            let (pi, _, found) = paxFindPage(rec)
            guard found, let (bm, tail) = pageFieldParts(pi: UInt32(pi), slot: slot) else {
                return (Data(), Data(), Data(), false)
            }
            let rc = col.pageRecCount[pi]
            guard let offBytes = try? nxsVarOffBytesLen(rc), tail.count >= offBytes else {
                return (Data(), Data(), Data(), false)
            }
            return (bm, Data(tail[..<offBytes]), Data(tail[offBytes...]), true)
        }
        return (Data(), Data(), Data(), false)
    }

    func colNumericBytes(rec: UInt32, slot: Int) -> Data? {
        guard slot >= 0 && slot < keySigils.count, !nxsIsVarSigil(keySigils[slot]) else {
            return nil
        }
        if col.layout == .columnar {
            guard let (bm, vals) = try? colFieldParts(slot: slot),
                  rec < UInt32(col.recordCount), nxsColBit(bm, rec) else { return nil }
            let off = Int(rec) * 8
            guard off + 8 <= vals.count else { return nil }
            return Data(vals[off..<(off + 8)])
        }
        if col.layout == .pax {
            let (pi, li, found) = paxFindPage(rec)
            guard found, let (pageBm, pageVals) = pageFieldParts(pi: UInt32(pi), slot: slot),
                  nxsColBit(pageBm, UInt32(li)) else { return nil }
            let off = li * 8
            guard off + 8 <= pageVals.count else { return nil }
            return Data(pageVals[off..<(off + 8)])
        }
        return nil
    }

    public func colGetStr(key: String, recordIndex: UInt32) -> (String, Bool) {
        guard let slot = keyIndex[key], recordIndex < UInt32(col.recordCount),
              col.layout != .row, keySigils[slot] == 0x22 else { return ("", false) }
        let (bm, offsets, values, ok) = colVarPartsAt(rec: recordIndex, slot: slot)
        guard ok else { return ("", false) }
        if col.layout == .pax {
            let (_, li, found) = paxFindPage(recordIndex)
            guard found, nxsColBit(bm, UInt32(li)) else { return ("", false) }
            return nxsVarStrAt(offsets: offsets, values: values, recordIndex: UInt32(li))
        }
        guard nxsColBit(bm, recordIndex) else { return ("", false) }
        return nxsVarStrAt(offsets: offsets, values: values, recordIndex: recordIndex)
    }

    public func colBuffer(key: String) -> (Data, Bool) {
        guard col.layout == .columnar,
              let slot = keyIndex[key],
              let (_, vals) = try? colFieldParts(slot: slot) else { return (Data(), false) }
        return (vals, true)
    }

    public func colVarBuffer(key: String) throws -> (bitmap: Data, offsets: Data, values: Data, count: UInt32) {
        guard col.layout == .columnar else {
            throw NXSError.outOfBounds("ERR_LAYOUT: ColVarBuffer is columnar-only")
        }
        guard let slot = keyIndex[key], slot < keySigils.count, nxsIsVarSigil(keySigils[slot]) else {
            throw NXSError.keyNotFound(key)
        }
        let (bm, off, val) = try colVarParts(slot: slot)
        return (bm, off, val, UInt32(col.recordCount))
    }

    func colSumF64(key: String) throws -> Double {
        let slot = try slot(key)
        if col.layout == .pax { return paxSumF64(slot: slot) }
        let (bm, vals) = try colFieldParts(slot: slot)
        var sum = 0.0
        for i in 0..<col.recordCount {
            guard nxsColBit(bm, UInt32(i)) else { continue }
            let off = i * 8
            guard off + 8 <= vals.count else { break }
            sum += rdF64(vals, off)
        }
        return sum
    }

    private func paxSumF64(slot: Int) -> Double {
        var sum = 0.0
        for pi in 0..<Int(col.pageCount) {
            guard let (bm, vals) = pageFieldParts(pi: UInt32(pi), slot: slot) else { continue }
            let rc = col.pageRecCount[pi]
            for i in 0..<Int(rc) {
                guard nxsColBit(bm, UInt32(i)) else { continue }
                let off = i * 8
                guard off + 8 <= vals.count else { break }
                sum += rdF64(vals, off)
            }
        }
        return sum
    }
}
