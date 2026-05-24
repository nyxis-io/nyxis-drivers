// Columnar and PAX layout read paths (OLAP.md).
package nxs

enum class Layout {
    ROW,
    COLUMNAR,
    PAX,
}

private const val FLAG_COLUMNAR: Int = 0x0001
private const val FLAG_PAX: Int = 0x0004

private const val FOOTER_ROW_BYTES = 12
private const val FOOTER_COL_BYTES = 20
private const val FOOTER_PAX_BYTES = 28

private const val COL_TAIL_ENTRY_BYTES = 20
private const val PAX_TAIL_ENTRY_BYTES = 28
private const val MAGIC_PAGE: Int = 0x4E585350

internal fun NxsReader.parseLayoutTail() {
    val f = flags.toInt()
    if (f and FLAG_COLUMNAR != 0 && f and FLAG_PAX != 0) {
        throw NxsError("ERR_INVALID_FLAGS", "columnar and PAX both set")
    }
    if (f and FLAG_COLUMNAR != 0 && tailPtr == 0L) {
        throw NxsError("ERR_INCOMPATIBLE_FLAGS", "columnar with TailPtr=0")
    }

    if (f and FLAG_COLUMNAR != 0) {
        layout = Layout.COLUMNAR
        parseColumnarFooter()
        return
    }
    if (f and FLAG_PAX != 0) {
        layout = Layout.PAX
        parsePaxFooter()
        return
    }

    layout = Layout.ROW
    var tp = tailPtr
    if (tp == 0L) {
        if (data.size < 44) throw NxsError("ERR_OUT_OF_BOUNDS", "streamable footer")
        tp = readU64(data.size - 12)
        tailPtr = tp
    }
    val tpi = tp.toInt()
    if (tpi + 4 > data.size) throw NxsError("ERR_OUT_OF_BOUNDS", "tail index")
    recordCount = readU32(tpi).toInt()
    tailStart = tpi + 4
}

private fun NxsReader.parseColumnarFooter() {
    if (data.size < FOOTER_COL_BYTES) throw NxsError("ERR_OUT_OF_BOUNDS", "columnar footer")
    val fo = data.size - FOOTER_COL_BYTES
    tailPtr = readU64(fo)
    recordCount = readU64(fo + 8).let { if (it > Int.MAX_VALUE) throw NxsError("ERR_OUT_OF_BOUNDS", "record count") else it.toInt() }
    tailStart = tailPtr.toInt()
    val kc = keys.size
    colBufOff = LongArray(kc)
    colBufLen = LongArray(kc)
    for (i in 0 until kc) {
        val e = tailStart + i * COL_TAIL_ENTRY_BYTES
        if (e + COL_TAIL_ENTRY_BYTES > data.size) {
            throw NxsError("ERR_OUT_OF_BOUNDS", "columnar tail entry")
        }
        val fid = readU16(e)
        if (fid >= kc) throw NxsError("ERR_OUT_OF_BOUNDS", "invalid field ID $fid")
        colBufOff[fid] = readU64(e + 4)
        colBufLen[fid] = readU64(e + 12)
    }
}

private fun NxsReader.parsePaxFooter() {
    if (data.size < FOOTER_PAX_BYTES) throw NxsError("ERR_OUT_OF_BOUNDS", "PAX footer")
    val fo = data.size - FOOTER_PAX_BYTES
    tailPtr = readU64(fo)
    recordCount = readU64(fo + 8).let { if (it > Int.MAX_VALUE) throw NxsError("ERR_OUT_OF_BOUNDS", "record count") else it.toInt() }
    pageCount = readU32(fo + 16).toInt()
    pageSizeHint = readU32(fo + 20).toInt()
    tailStart = tailPtr.toInt()
    if (pageCount > 0) {
        pageIndex = IntArray(pageCount)
        pageRecStart = LongArray(pageCount)
        pageRecCount = IntArray(pageCount)
        pageOffset = LongArray(pageCount)
        pageLength = IntArray(pageCount)
        for (i in 0 until pageCount) {
            val e = tailStart + i * PAX_TAIL_ENTRY_BYTES
            if (e + PAX_TAIL_ENTRY_BYTES > data.size) {
                throw NxsError("ERR_OUT_OF_BOUNDS", "PAX tail entry")
            }
            pageIndex[i] = readU32(e).toInt()
            pageRecStart[i] = readU64(e + 4)
            pageRecCount[i] = readU32(e + 12).toInt()
            pageOffset[i] = readU64(e + 16)
            pageLength[i] = readU32(e + 24).toInt()
        }
        val dlen = data.size.toLong()
        for (i in 0 until pageCount) {
            val poff64 = pageOffset[i]
            if (poff64 > dlen || poff64 + 4 > dlen || poff64 > Int.MAX_VALUE) {
                throw NxsError("ERR_OUT_OF_BOUNDS", "PAX page offset")
            }
            if (readU32(poff64.toInt()).toInt() != MAGIC_PAGE) {
                throw NxsError("ERR_INVALID_PAGE_MAGIC", "PAX page magic mismatch")
            }
        }
    }
}

private fun varOffBytesLen(rc: Int): Int {
    val off = (rc.toLong() + 1) * 4
    if (off > Int.MAX_VALUE) throw NxsError("ERR_OUT_OF_BOUNDS", "var offsets overflow")
    return off.toInt()
}

private fun nullBitmapBytes(n: Int): Int {
    val raw = (n + 7) / 8
    return (raw + 7) and 7.inv()
}

private fun colBit(
    bm: ByteArray,
    rec: Int,
): Boolean = ((bm[rec / 8].toInt() shr (rec % 8)) and 1) == 1

private fun isVarSigil(sig: Byte): Boolean = sig == 0x22.toByte() || sig == 0x3C.toByte()

private fun fieldSectorLen(
    data: ByteArray,
    sectorOff: Int,
    rc: Int,
    sigil: Byte,
): Int {
    val bmLen = nullBitmapBytes(rc)
    if (!isVarSigil(sigil)) return bmLen + rc * 8
    val offBytes = varOffBytesLen(rc)
    if (sectorOff + bmLen + offBytes > data.size) {
        throw NxsError("ERR_OUT_OF_BOUNDS", "var offsets")
    }
    val end = readU32Slice(data, sectorOff + bmLen + rc * 4).toInt()
    val total = bmLen + offBytes + end
    if (sectorOff + total > data.size) throw NxsError("ERR_OUT_OF_BOUNDS", "var values")
    return total
}

internal fun varStrAt(
    offsets: ByteArray,
    values: ByteArray,
    recordIndex: Int,
): Pair<String, Boolean> {
    if (offsets.size.toLong() < (recordIndex.toLong() + 2) * 4) return "" to false
    val off = recordIndex * 4
    val start = readU32Slice(offsets, off).toInt()
    val end = readU32Slice(offsets, off + 4).toInt()
    if (end < start || end > values.size) return "" to false
    return String(values, start, end - start, Charsets.UTF_8) to true
}

private fun varBinaryAt(
    offsets: ByteArray,
    values: ByteArray,
    recordIndex: Int,
): Pair<ByteArray, Boolean> {
    if (offsets.size.toLong() < (recordIndex.toLong() + 2) * 4) return byteArrayOf() to false
    val off = recordIndex * 4
    val start = readU32Slice(offsets, off).toInt()
    val end = readU32Slice(offsets, off + 4).toInt()
    if (end < start || end > values.size) return byteArrayOf() to false
    return values.copyOfRange(start, end) to true
}

private fun readU32Slice(
    data: ByteArray,
    off: Int,
): Long =
    (data[off].toLong() and 0xFF) or
        ((data[off + 1].toLong() and 0xFF) shl 8) or
        ((data[off + 2].toLong() and 0xFF) shl 16) or
        ((data[off + 3].toLong() and 0xFF) shl 24)

internal fun NxsReader.colVarParts(slot: Int): Triple<ByteArray, ByteArray, ByteArray> {
    val (bm, tail) = colFieldParts(slot)
    val offBytes = varOffBytesLen(recordCount)
    if (tail.size < offBytes) throw NxsError("ERR_OUT_OF_BOUNDS", "var offsets")
    return Triple(bm, tail.copyOfRange(0, offBytes), tail.copyOfRange(offBytes, tail.size))
}

private fun NxsReader.colVarPartsAt(
    rec: Int,
    slot: Int,
): Triple<ByteArray, ByteArray, ByteArray>? {
    if (slot < 0 || slot >= keySigils.size || !isVarSigil(keySigils[slot])) return null
    if (layout == Layout.COLUMNAR) {
        return try {
            colVarParts(slot)
        } catch (_: NxsError) {
            null
        }
    }
    if (layout == Layout.PAX) {
        val (pi, _) = paxFindPage(rec) ?: return null
        val (bm, tail) = pageFieldParts(pi, slot) ?: return null
        val rc = pageRecCount[pi]
        val offBytes =
            try {
                varOffBytesLen(rc)
            } catch (_: NxsError) {
                return null
            }
        if (tail.size < offBytes) return null
        return Triple(bm, tail.copyOfRange(0, offBytes), tail.copyOfRange(offBytes, tail.size))
    }
    return null
}

data class ColVarBuffer(
    val bitmap: ByteArray,
    val offsets: ByteArray,
    val values: ByteArray,
    val count: Int,
)

fun NxsReader.colVarBuffer(key: String): ColVarBuffer {
    if (layout != Layout.COLUMNAR) {
        throw NxsError("ERR_LAYOUT", "ColVarBuffer is columnar-only (use ColGetStr per record on PAX)")
    }
    val slot = keyIndex[key] ?: throw NxsError("ERR_KEY_NOT_FOUND", key)
    if (slot < 0 || slot >= keySigils.size || !isVarSigil(keySigils[slot])) {
        throw NxsError("ERR_UNSUPPORTED_FIELD_TYPE", key)
    }
    val (bm, offsets, values) = colVarParts(slot)
    return ColVarBuffer(bm, offsets, values, recordCount)
}

internal fun NxsReader.colGetStr(
    key: String,
    recordIndex: Int,
): Pair<String, Boolean> {
    val slot = keyIndex[key] ?: return "" to false
    if (recordIndex >= recordCount || layout == Layout.ROW) return "" to false
    if (keySigils[slot] != 0x22.toByte()) return "" to false
    val parts = colVarPartsAt(recordIndex, slot) ?: return "" to false
    val (bm, offsets, values) = parts
    if (layout == Layout.PAX) {
        val (_, li) = paxFindPage(recordIndex) ?: return "" to false
        if (!colBit(bm, li)) return "" to false
        return varStrAt(offsets, values, li)
    }
    if (!colBit(bm, recordIndex)) return "" to false
    return varStrAt(offsets, values, recordIndex)
}

internal fun NxsReader.colGetBinary(
    key: String,
    recordIndex: Int,
): Pair<ByteArray, Boolean> {
    val slot = keyIndex[key] ?: return byteArrayOf() to false
    if (recordIndex >= recordCount || layout == Layout.ROW) return byteArrayOf() to false
    if (keySigils[slot] != 0x3C.toByte()) return byteArrayOf() to false
    val parts = colVarPartsAt(recordIndex, slot) ?: return byteArrayOf() to false
    val (bm, offsets, values) = parts
    if (layout == Layout.PAX) {
        val (_, li) = paxFindPage(recordIndex) ?: return byteArrayOf() to false
        if (!colBit(bm, li)) return byteArrayOf() to false
        return varBinaryAt(offsets, values, li)
    }
    if (!colBit(bm, recordIndex)) return byteArrayOf() to false
    return varBinaryAt(offsets, values, recordIndex)
}

internal fun NxsReader.colNumericBytes(
    rec: Int,
    slot: Int,
): Pair<ByteArray, Boolean> {
    if (slot >= 0 && slot < keySigils.size && isVarSigil(keySigils[slot])) return byteArrayOf() to false
    if (layout == Layout.COLUMNAR) {
        val (bm, vals) =
            try {
                colFieldParts(slot)
            } catch (_: NxsError) {
                return byteArrayOf() to false
            }
        if (rec >= recordCount || !colBit(bm, rec)) return byteArrayOf() to false
        val off = rec * 8
        if (off + 8 > vals.size) return byteArrayOf() to false
        return vals.copyOfRange(off, off + 8) to true
    }
    if (layout == Layout.PAX) {
        val (pi, li) = paxFindPage(rec) ?: return byteArrayOf() to false
        val (pageBm, pageVals) = pageFieldParts(pi, slot) ?: return byteArrayOf() to false
        if (!colBit(pageBm, li)) return byteArrayOf() to false
        val off = li * 8
        if (off + 8 > pageVals.size) return byteArrayOf() to false
        return pageVals.copyOfRange(off, off + 8) to true
    }
    return byteArrayOf() to false
}

private fun NxsReader.colFieldParts(slot: Int): Pair<ByteArray, ByteArray> {
    if (slot < 0 || slot >= colBufOff.size) throw NxsError("ERR_KEY_NOT_FOUND", "slot")
    val sector =
        columnWarm?.sector(this, slot)
            ?: run {
                val off = colBufOff[slot].toInt()
                val length = colBufLen[slot].toInt()
                if (off + length > data.size) throw NxsError("ERR_OUT_OF_BOUNDS", "column buffer")
                data.copyOfRange(off, off + length)
            }
    val bmLen = nullBitmapBytes(recordCount)
    if (sector.size < bmLen) throw NxsError("ERR_OUT_OF_BOUNDS", "null bitmap")
    return sector.copyOfRange(0, bmLen) to sector.copyOfRange(bmLen, sector.size)
}

fun NxsReader.colSumF64(key: String): Double {
    val slot = slot(key)
    if (layout == Layout.ROW) return sumF64Row(slot)
    if (layout == Layout.PAX) return paxSumF64(slot)
    val (bm, vals) =
        try {
            colFieldParts(slot)
        } catch (_: NxsError) {
            return 0.0
        }
    var sum = 0.0
    for (i in 0 until recordCount) {
        if (!colBit(bm, i)) continue
        val off = i * 8
        if (off + 8 > vals.size) break
        sum += readF64Slice(vals, off)
    }
    return sum
}

internal fun NxsReader.sumF64Row(slot: Int): Double {
    var sum = 0.0
    for (i in 0 until recordCount) {
        val abs = readU64(tailStart + i * 10 + 2).toInt()
        val off = scanOffset(abs, slot)
        if (off >= 0) sum += readF64(off)
    }
    return sum
}

fun NxsReader.colBuffer(key: String): Pair<ByteArray, Boolean> {
    val slot = keyIndex[key] ?: return byteArrayOf() to false
    if (layout == Layout.ROW) return byteArrayOf() to false
    return try {
        colFieldParts(slot).second to true
    } catch (_: NxsError) {
        byteArrayOf() to false
    }
}

private fun NxsReader.paxSumF64(slot: Int): Double {
    var sum = 0.0
    for (pi in 0 until pageCount) {
        val (bm, vals) = pageFieldParts(pi, slot) ?: continue
        val rc = pageRecCount[pi]
        for (i in 0 until rc) {
            if (!colBit(bm, i)) continue
            val off = i * 8
            if (off + 8 > vals.size) break
            sum += readF64Slice(vals, off)
        }
    }
    return sum
}

private fun NxsReader.pageFieldSector(
    pi: Int,
    slot: Int,
): ByteArray? {
    val poff = pageOffset[pi].toInt()
    if (poff + 24 > data.size || readU32(poff).toInt() != MAGIC_PAGE) return null
    val fc = readU16(poff + 20)
    if (slot < 0 || slot >= fc || fc > keySigils.size) return null
    val rc = pageRecCount[pi]
    var body = poff + 24
    for (fi in 0 until slot) {
        val sig = if (fi < keySigils.size) keySigils[fi] else 0x3D.toByte()
        val flen =
            try {
                fieldSectorLen(data, body, rc, sig)
            } catch (_: NxsError) {
                return null
            }
        body += flen
    }
    val sig = if (slot < keySigils.size) keySigils[slot] else 0x3D.toByte()
    val flen =
        try {
            fieldSectorLen(data, body, rc, sig)
        } catch (_: NxsError) {
            return null
        }
    if (body + flen > data.size) return null
    return data.copyOfRange(body, body + flen)
}

private fun NxsReader.pageFieldParts(
    pi: Int,
    slot: Int,
): Pair<ByteArray, ByteArray>? {
    val sector = pageFieldSector(pi, slot) ?: return null
    val bmLen = nullBitmapBytes(pageRecCount[pi])
    if (sector.size < bmLen) return null
    return sector.copyOfRange(0, bmLen) to sector.copyOfRange(bmLen, sector.size)
}

private fun NxsReader.paxFindPage(rec: Int): Pair<Int, Int>? {
    if (pageCount == 0) return null
    val r64 = rec.toLong()
    var lo = 0
    var hi = pageCount - 1
    while (lo <= hi) {
        val mid = lo + (hi - lo) / 2
        val start = pageRecStart[mid]
        val count = pageRecCount[mid].toLong()
        when {
            r64 < start -> hi = mid - 1
            r64 >= start + count -> lo = mid + 1
            else -> return mid to (r64 - start).toInt()
        }
    }
    return null
}

private fun readF64Slice(
    data: ByteArray,
    off: Int,
): Double = java.lang.Double.longBitsToDouble(readU64Slice(data, off))

private fun readU64Slice(
    data: ByteArray,
    off: Int,
): Long = readU32Slice(data, off) or (readU32Slice(data, off + 4) shl 32)
