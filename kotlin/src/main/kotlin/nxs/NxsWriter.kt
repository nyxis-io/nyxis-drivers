package nxs

import java.io.ByteArrayOutputStream

/**
 * NXS Writer — direct-to-buffer .nxb emitter for Kotlin/JVM.
 *
 * Mirrors the Rust NxsWriter API:
 * - [NxsSchema] — precompile keys once; share across [NxsWriter] instances.
 * - [NxsWriter] — slot-based hot path; no per-key map lookups during write.
 *
 * Usage:
 * ```
 * val schema = NxsSchema(listOf("id", "username", "score", "active"))
 * val w = NxsWriter(schema)
 * w.beginObject()
 * w.writeI64(0, 42L)
 * w.writeStr(1, "alice")
 * w.writeF64(2, 9.5)
 * w.writeBool(3, true)
 * w.endObject()
 * val bytes: ByteArray = w.finish()
 * ```
 */

private fun murmurHash64(data: ByteArray): Long {
    val c1 = -0xAE502812AA7333L // 0xFF51AFD7ED558CCD as Long
    val c2 = -0x3B314601E57A13ADL // 0xC4CEB9FE1A85EC53 as Long
    var h = -0x6C97E29DAACEC567L // 0x93681D6255313A99 as Long
    val len = data.size
    var i = 0
    while (i < len) {
        var k = 0L
        for (b in 0..7) {
            if (i + b < len) k = k or ((data[i + b].toLong() and 0xFF) shl (b * 8))
        }
        k *= c1
        k = k xor (k ushr 33)
        h = h xor k
        h *= c2
        h = h xor (h ushr 33)
        i += 8
    }
    h = h xor len.toLong()
    h = h xor (h ushr 33)
    h *= c1
    h = h xor (h ushr 33)
    return h
}

class NxsSchema(val keys: List<String>) {
    val bitmaskBytes: Int = maxOf(1, (keys.size + 6) / 7)
    val count: Int get() = keys.size
}

private class Frame(val start: Int, bitmaskBytes: Int) {
    val bitmask: ByteArray =
        ByteArray(bitmaskBytes).also {
            for (i in 0 until bitmaskBytes - 1) it[i] = 0x80.toByte()
        }
    val offsetTable = mutableListOf<Int>()
    val slotOffsets = mutableListOf<Pair<Int, Int>>() // (slot, bufPos)
    var lastSlot: Int = -1
    var needsSort: Boolean = false
}

private const val sigilStr = 0x22 // '"' — string / var-length
private const val sigilI64 = 0x69 // 'i'
private const val sigilF64 = 0x64 // 'd'
private const val sigilBool = 0x62 // 'b'
private const val sigilNull = 0x6E // 'n'
private const val sigilBinary = 0x42 // 'B'

class NxsWriter(private val schema: NxsSchema) {
    private val buf = ByteArrayOutputStream(4096)
    private val frames = ArrayDeque<Frame>()
    private val recordOffsets = mutableListOf<Int>()
    private val slotSigils = IntArray(schema.count) { sigilStr }

    fun beginObject() {
        if (frames.isEmpty()) recordOffsets += buf.size()
        val start = buf.size()
        val frame = Frame(start, schema.bitmaskBytes)
        frames.addLast(frame)

        writeU32(0x4E59584F) // NYXO
        writeU32(0) // length placeholder
        buf.write(frame.bitmask)
        repeat(schema.count * 2) { buf.write(0) }
        while ((buf.size() - start) % 8 != 0) buf.write(0)
    }

    fun endObject() {
        check(frames.isNotEmpty()) { "endObject without beginObject" }
        val frame = frames.removeLast()
        val bytes = buf.toByteArray() // needed for back-patching
        // Note: we operate on buf's internal array via a trick —
        // ByteArrayOutputStream doesn't expose the array, so we rebuild.
        // This is acceptable since endObject is called per-record, not per-field.
        val arr = bytes
        val totalLen = arr.size - frame.start

        // Back-patch Length at start + 4
        putU32(arr, frame.start + 4, totalLen)

        // Back-patch bitmask at start + 8
        val bmOff = frame.start + 8
        frame.bitmask.forEachIndexed { i, b -> arr[bmOff + i] = b }

        // Back-patch offset table
        val otStart = bmOff + schema.bitmaskBytes
        val present = frame.offsetTable.size

        if (!frame.needsSort) {
            frame.offsetTable.forEachIndexed { i, rel ->
                putU16(arr, otStart + i * 2, rel)
            }
        } else {
            val sorted = frame.slotOffsets.sortedBy { it.first }
            sorted.forEachIndexed { i, (_, bufPos) ->
                putU16(arr, otStart + i * 2, bufPos - frame.start)
            }
        }
        for (i in present * 2 until schema.count * 2) arr[otStart + i] = 0

        buf.reset()
        buf.write(arr)
    }

    fun finish(): ByteArray {
        check(frames.isEmpty()) { "unclosed objects" }

        val schemaBytes = buildSchemaBytes()
        val dictHash = murmurHash64(schemaBytes)
        val dataStart = 32 + schemaBytes.size
        val dataSector = buf.toByteArray()
        val tailPtr = (dataStart + dataSector.size).toLong()
        val tail = buildTailIndex(dataStart, tailPtr)

        val out = ByteArrayOutputStream(32 + schemaBytes.size + dataSector.size + tail.size)

        fun u32(v: Int) {
            out.write(v and 0xFF)
            out.write((v shr 8) and 0xFF)
            out.write((v shr 16) and 0xFF)
            out.write((v shr 24) and 0xFF)
        }

        fun u16(v: Int) {
            out.write(v and 0xFF)
            out.write((v shr 8) and 0xFF)
        }

        fun u64(v: Long) {
            for (i in 0..7) out.write(((v shr (i * 8)) and 0xFF).toInt())
        }

        u32(0x4E595842) // NYXB
        u16(0x0101) // VERSION
        u16(0x0002) // FLAG_SCHEMA_EMBEDDED
        u64(dictHash)
        u64(0)
        repeat(8) { out.write(0) } // reserved

        out.write(schemaBytes)
        out.write(dataSector)
        out.write(tail)
        return out.toByteArray()
    }

    fun writeI64(
        slot: Int,
        value: Long,
    ) {
        slotSigils[slot] = sigilI64
        markSlot(slot)
        for (i in 0..7) buf.write(((value ushr (i * 8)) and 0xFF).toInt())
    }

    fun writeF64(
        slot: Int,
        value: Double,
    ) {
        slotSigils[slot] = sigilF64
        writeI64(slot, java.lang.Double.doubleToRawLongBits(value))
    }

    fun writeBool(
        slot: Int,
        value: Boolean,
    ) {
        slotSigils[slot] = sigilBool
        markSlot(slot)
        buf.write(if (value) 1 else 0)
        repeat(7) { buf.write(0) }
    }

    fun writeTime(
        slot: Int,
        unixNs: Long,
    ) {
        slotSigils[slot] = sigilI64
        writeI64(slot, unixNs)
    }

    fun writeNull(slot: Int) {
        slotSigils[slot] = sigilNull
        markSlot(slot)
        repeat(8) { buf.write(0) }
    }

    fun writeStr(
        slot: Int,
        value: String,
    ) {
        slotSigils[slot] = sigilStr
        markSlot(slot)
        val b = value.toByteArray(Charsets.UTF_8)
        writeU32(b.size)
        buf.write(b)
        val used = (4 + b.size) % 8
        if (used != 0) repeat(8 - used) { buf.write(0) }
    }

    fun writeBytes(
        slot: Int,
        value: ByteArray,
    ) {
        slotSigils[slot] = sigilBinary
        markSlot(slot)
        writeU32(value.size)
        buf.write(value)
        val used = (4 + value.size) % 8
        if (used != 0) repeat(8 - used) { buf.write(0) }
    }

    fun writeListI64(
        slot: Int,
        values: LongArray,
    ) {
        markSlot(slot)
        val total = 16 + values.size * 8
        writeU32(0x4E59584C)
        writeU32(total)
        buf.write(0x3D) // '=' sigil
        writeU32(values.size)
        repeat(3) { buf.write(0) }
        for (v in values) writeRawI64(v)
    }

    fun writeListF64(
        slot: Int,
        values: DoubleArray,
    ) {
        markSlot(slot)
        val total = 16 + values.size * 8
        writeU32(0x4E59584C)
        writeU32(total)
        buf.write(0x7E) // '~' sigil
        writeU32(values.size)
        repeat(3) { buf.write(0) }
        for (v in values) writeRawI64(java.lang.Double.doubleToRawLongBits(v))
    }

    // Convenience: write records from a list of maps.
    companion object {
        fun fromRecords(
            keys: List<String>,
            records: List<Map<String, Any?>>,
        ): ByteArray {
            val schema = NxsSchema(keys)
            val w = NxsWriter(schema)
            for (rec in records) {
                w.beginObject()
                for ((i, key) in keys.withIndex()) {
                    if (!rec.containsKey(key)) continue
                    when (val v = rec[key]) {
                        null -> w.writeNull(i)
                        is Boolean -> w.writeBool(i, v)
                        is Int -> w.writeI64(i, v.toLong())
                        is Long -> w.writeI64(i, v)
                        is Float -> w.writeF64(i, v.toDouble())
                        is Double -> w.writeF64(i, v)
                        is String -> w.writeStr(i, v)
                        is ByteArray -> w.writeBytes(i, v)
                    }
                }
                w.endObject()
            }
            return w.finish()
        }
    }

    private fun markSlot(slot: Int) {
        check(frames.isNotEmpty()) { "write outside beginObject/endObject" }
        val frame = frames.last()
        frame.bitmask[slot / 7] = (frame.bitmask[slot / 7].toInt() or (1 shl (slot % 7))).toByte()
        val rel = buf.size() - frame.start
        if (slot < frame.lastSlot) frame.needsSort = true
        frame.lastSlot = slot
        frame.offsetTable += rel
        frame.slotOffsets += Pair(slot, buf.size())
    }

    private fun writeU32(v: Int) {
        buf.write(v and 0xFF)
        buf.write((v shr 8) and 0xFF)
        buf.write((v shr 16) and 0xFF)
        buf.write((v shr 24) and 0xFF)
    }

    private fun writeRawI64(v: Long) {
        for (i in 0..7) buf.write(((v ushr (i * 8)) and 0xFF).toInt())
    }

    private fun putU32(
        arr: ByteArray,
        off: Int,
        v: Int,
    ) {
        arr[off] = (v and 0xFF).toByte()
        arr[off + 1] = ((v shr 8) and 0xFF).toByte()
        arr[off + 2] = ((v shr 16) and 0xFF).toByte()
        arr[off + 3] = ((v shr 24) and 0xFF).toByte()
    }

    private fun putU16(
        arr: ByteArray,
        off: Int,
        v: Int,
    ) {
        arr[off] = (v and 0xFF).toByte()
        arr[off + 1] = ((v shr 8) and 0xFF).toByte()
    }

    private fun buildSchemaBytes(): ByteArray {
        val n = schema.count
        val utf8 = schema.keys.map { it.toByteArray(Charsets.UTF_8) }
        var size = 2 + n + utf8.sumOf { it.size + 1 }
        val pad = (8 - size % 8) % 8
        size += pad

        val b = ByteArray(size)
        var p = 0
        b[p++] = (n and 0xFF).toByte()
        b[p++] = ((n shr 8) and 0xFF).toByte()
        repeat(n) { i -> b[p++] = slotSigils[i].toByte() }
        for (e in utf8) {
            e.copyInto(b, p)
            p += e.size
            b[p++] = 0
        }
        return b
    }

    private fun buildTailIndex(
        dataStart: Int,
        tailPtr: Long,
    ): ByteArray {
        val nr = recordOffsets.size
        val out = ByteArrayOutputStream(4 + nr * 10 + 12)

        fun u32(v: Int) {
            out.write(v and 0xFF)
            out.write((v shr 8) and 0xFF)
            out.write((v shr 16) and 0xFF)
            out.write((v shr 24) and 0xFF)
        }

        fun u64(v: Long) {
            for (i in 0..7) out.write(((v ushr (i * 8)) and 0xFF).toInt())
        }

        fun u16(v: Int) {
            out.write(v and 0xFF)
            out.write((v shr 8) and 0xFF)
        }

        u32(nr)
        for ((i, rel) in recordOffsets.withIndex()) {
            u16(i)
            u64((dataStart + rel).toLong())
        }
        u64(tailPtr)
        u32(0x2153584E) // NXS!
        return out.toByteArray()
    }
}
