// NXS Reader — zero-copy .nxb parser for Kotlin/JVM
// Implements the Nyxis v1.1 binary wire format spec.
//
// Usage:
//   val buf = File("data.nxb").readBytes()
//   val reader = NxsReader(buf)
//   val obj = reader.record(42)
//   val id: Long = obj.getI64("id")
package nxs

import java.nio.ByteBuffer
import java.nio.ByteOrder

// ── Exceptions ────────────────────────────────────────────────────────────────

class NxsError(val code: String, msg: String) : Exception("$code: $msg")

// ── Constants ─────────────────────────────────────────────────────────────────

private const val MAGIC_FILE: Int = 0x4E595842.toInt()
private const val MAGIC_OBJ: Int = 0x4E59584F.toInt()
private const val MAGIC_FOOTER: Int = 0x2153584E.toInt()
private const val FLAG_SCHEMA: Int = 0x0002

// ── Reader ────────────────────────────────────────────────────────────────────

class NxsReader(private val data: ByteArray) {
    val version: Short
    val flags: Short
    val dictHash: Long
    val tailPtr: Long
    val keys: List<String>
    val keySigils: ByteArray
    internal val keyIndex: Map<String, Int>
    val recordCount: Int
    private val tailStart: Int

    init {
        if (data.size < 32) throw NxsError("ERR_OUT_OF_BOUNDS", "file too small")

        val buf = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
        if (buf.getInt(0) != MAGIC_FILE) throw NxsError("ERR_BAD_MAGIC", "preamble")
        if (buf.getInt(data.size - 4) != MAGIC_FOOTER) throw NxsError("ERR_BAD_MAGIC", "footer")

        version = buf.getShort(4)
        flags = buf.getShort(6)
        dictHash = buf.getLong(8)
        tailPtr =
            buf.getLong(16).let { preambleTailPtr ->
                if (preambleTailPtr != 0L) {
                    preambleTailPtr
                } else {
                    if (data.size < 44) {
                        throw NxsError("ERR_OUT_OF_BOUNDS", "stream footer")
                    }
                    buf.getLong(data.size - 12)
                }
            }

        val ks = mutableListOf<String>()
        val ki = mutableMapOf<String, Int>()
        var kSigils = ByteArray(0)

        if (flags.toInt() and FLAG_SCHEMA != 0) {
            var off = 32
            val keyCount = buf.getShort(off).toInt() and 0xFFFF
            off += 2
            kSigils = data.copyOfRange(off, off + keyCount)
            off += keyCount
            repeat(keyCount) { i ->
                var end = off
                while (end < data.size && data[end] != 0.toByte()) end++
                val name = String(data, off, end - off, Charsets.UTF_8)
                ks.add(name)
                ki[name] = i
                off = end + 1
            }
            // Pad to 8-byte boundary to find schema end
            if (off % 8 != 0) off += 8 - (off % 8)
            val schemaEnd = off
            val computed = murmur3Hash64(data, 32, schemaEnd - 32)
            if (computed != dictHash) throw NxsError("ERR_DICT_MISMATCH", "schema hash mismatch")
        }

        keys = ks
        keySigils = kSigils
        keyIndex = ki

        val tp = tailPtr.toInt()
        if (tp + 4 > data.size) throw NxsError("ERR_OUT_OF_BOUNDS", "tail index")
        recordCount = buf.getInt(tp)
        tailStart = tp + 4
    }

    private val buf: ByteBuffer get() = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)

    fun slot(key: String): Int = keyIndex[key] ?: throw NxsError("ERR_KEY_NOT_FOUND", key)

    fun record(i: Int): NxsObject {
        if (i < 0 || i >= recordCount) {
            throw NxsError("ERR_OUT_OF_BOUNDS", "record $i out of [0, $recordCount)")
        }
        val entryOff = tailStart + i * 10 + 2
        val absOff = readU64(entryOff).toInt()
        return NxsObject(this, absOff)
    }

    // ── Internal: Little-endian reads ─────────────────────────────────────────

    internal fun readU16(off: Int): Int =
        (data[off].toInt() and 0xFF) or
            ((data[off + 1].toInt() and 0xFF) shl 8)

    internal fun readU32(off: Int): Long =
        ((data[off ].toInt() and 0xFF).toLong()) or
            ((data[off + 1].toInt() and 0xFF).toLong() shl 8) or
            ((data[off + 2].toInt() and 0xFF).toLong() shl 16) or
            ((data[off + 3].toInt() and 0xFF).toLong() shl 24)

    internal fun readU64(off: Int): Long {
        val lo = readU32(off)
        val hi = readU32(off + 4)
        return lo or (hi shl 32)
    }

    internal fun readI64(off: Int): Long = readU64(off)

    internal fun readF64(off: Int): Double = java.lang.Double.longBitsToDouble(readU64(off))

    internal fun readByte(off: Int): Int = data[off].toInt() and 0xFF

    internal fun size(): Int = data.size

    internal fun readStr(off: Int): String {
        val len = readU32(off).toInt()
        return String(data, off + 4, len, Charsets.UTF_8)
    }

    // ── Bulk reducers ─────────────────────────────────────────────────────────

    fun sumF64(key: String): Double {
        val s = slot(key)
        var sum = 0.0
        for (i in 0 until recordCount) {
            val abs = readU64(tailStart + i * 10 + 2).toInt()
            val off = scanOffset(abs, s)
            if (off >= 0) sum += readF64(off)
        }
        return sum
    }

    fun sumI64(key: String): Long {
        val s = slot(key)
        var sum = 0L
        for (i in 0 until recordCount) {
            val abs = readU64(tailStart + i * 10 + 2).toInt()
            val off = scanOffset(abs, s)
            if (off >= 0) sum += readI64(off)
        }
        return sum
    }

    fun minF64(key: String): Double? {
        val s = slot(key)
        var m: Double? = null
        for (i in 0 until recordCount) {
            val abs = readU64(tailStart + i * 10 + 2).toInt()
            val off = scanOffset(abs, s)
            if (off < 0) continue
            val v = readF64(off)
            m = if (m == null || v < m) v else m
        }
        return m
    }

    fun maxF64(key: String): Double? {
        val s = slot(key)
        var m: Double? = null
        for (i in 0 until recordCount) {
            val abs = readU64(tailStart + i * 10 + 2).toInt()
            val off = scanOffset(abs, s)
            if (off < 0) continue
            val v = readF64(off)
            m = if (m == null || v > m) v else m
        }
        return m
    }

    // ── DictHash validation ───────────────────────────────────────────────────

    private fun murmur3Hash64(
        data: ByteArray,
        offset: Int,
        length: Int,
    ): Long {
        val c1 = -0xAE502812AA7333L // 0xFF51AFD7ED558CCD as signed Long
        val c2 = -0x3B314601E57A13ADL // 0xC4CEB9FE1A85EC53 as signed Long
        var h = -0x6C97E29DAACEC567L // 0x93681D6255313A99 as signed Long
        var p = offset
        val end = offset + length
        while (p < end) {
            var k = 0L
            for (i in 0 until 8) if (p + i < end) k = k or ((data[p + i].toLong() and 0xFF) shl (i * 8))
            k *= c1
            k = k xor (k ushr 33)
            h = h xor k
            h *= c2
            h = h xor (h ushr 33)
            p += 8
        }
        h = h xor length.toLong()
        h = h xor (h ushr 33)
        h *= c1
        h = h xor (h ushr 33)
        return h
    }

    // Returns absolute offset of slot's value in object at objOffset, or -1.
    internal fun scanOffset(
        objOffset: Int,
        slot: Int,
    ): Int {
        var p = objOffset + 8
        var cur = 0
        var tableIdx = 0
        var b = 0
        while (true) {
            if (p >= data.size) return -1
            b = readByte(p++)
            val bits = b and 0x7F
            for (i in 0 until 7) {
                if (cur == slot) {
                    if ((bits shr i) and 1 == 0) return -1
                    while (b and 0x80 != 0) {
                        b = readByte(p++)
                    }
                    val ot = p + tableIdx * 2
                    if (ot + 2 > data.size) return -1
                    return objOffset + readU16(ot)
                }
                if (cur < slot && (bits shr i) and 1 == 1) tableIdx++
                cur++
            }
            if (b and 0x80 == 0) return -1
        }
    }
}

// ── Object ────────────────────────────────────────────────────────────────────

class NxsObject(private val reader: NxsReader, private val offset: Int) {
    private var staged = false
    private var bitmaskStart = 0
    private var offsetTableStart = 0

    private fun locateBitmask() {
        if (staged) return
        if (offset + 8 > reader.size()) throw NxsError("ERR_OUT_OF_BOUNDS", "object header")
        if (reader.readU32(offset).toInt() and -1 != 0x4E59584F.toInt()) {
            val m = reader.readU32(offset).toInt()
            if (m != 0x4E59584F.toInt()) throw NxsError("ERR_BAD_MAGIC", "object at $offset")
        }
        var p = offset + 8
        bitmaskStart = p
        while (p < reader.size() && reader.readByte(p) and 0x80 != 0) p++
        if (p >= reader.size()) throw NxsError("ERR_OUT_OF_BOUNDS", "bitmask")
        p++
        offsetTableStart = p
        staged = true
    }

    private fun resolveSlot(slot: Int): Int {
        locateBitmask()
        var p = bitmaskStart
        var cur = 0
        var tableIdx = 0
        var b = 0
        while (true) {
            if (p >= reader.size()) return -1
            b = reader.readByte(p++)
            val bits = b and 0x7F
            for (i in 0 until 7) {
                if (cur == slot) {
                    if ((bits shr i) and 1 == 0) return -1
                    while (b and 0x80 != 0) {
                        b = reader.readByte(p++)
                    }
                    val ot = offsetTableStart + tableIdx * 2
                    if (ot + 2 > reader.size()) return -1
                    return offset + reader.readU16(ot)
                }
                if (cur < slot && (bits shr i) and 1 == 1) tableIdx++
                cur++
            }
            if (b and 0x80 == 0) return -1
        }
    }

    fun getI64(key: String) = getI64BySlot(reader.slot(key))

    fun getF64(key: String) = getF64BySlot(reader.slot(key))

    fun getBool(key: String) = getBoolBySlot(reader.slot(key))

    fun getStr(key: String) = getStrBySlot(reader.slot(key))

    // ── Nullable accessors (for query predicates) ─────────────────────────────

    /** Returns the I64 value for [key], or null if the field is absent. */
    fun tryGetI64(key: String): Long? {
        val slot = reader.keyIndex[key] ?: return null
        val off = resolveSlot(slot)
        return if (off < 0) null else reader.readI64(off)
    }

    /** Returns the F64 value for [key], or null if the field is absent. */
    fun tryGetF64(key: String): Double? {
        val slot = reader.keyIndex[key] ?: return null
        val off = resolveSlot(slot)
        return if (off < 0) null else reader.readF64(off)
    }

    /** Returns the Bool value for [key], or null if the field is absent. */
    fun tryGetBool(key: String): Boolean? {
        val slot = reader.keyIndex[key] ?: return null
        val off = resolveSlot(slot)
        return if (off < 0) null else reader.readByte(off) != 0
    }

    /** Returns the Str value for [key], or null if the field is absent. */
    fun tryGetStr(key: String): String? {
        val slot = reader.keyIndex[key] ?: return null
        val off = resolveSlot(slot)
        return if (off < 0) null else reader.readStr(off)
    }

    fun getI64BySlot(slot: Int): Long {
        val off = resolveSlot(slot)
        if (off < 0) throw NxsError("ERR_FIELD_ABSENT", "slot $slot")
        return reader.readI64(off)
    }

    fun getF64BySlot(slot: Int): Double {
        val off = resolveSlot(slot)
        if (off < 0) throw NxsError("ERR_FIELD_ABSENT", "slot $slot")
        return reader.readF64(off)
    }

    fun getBoolBySlot(slot: Int): Boolean {
        val off = resolveSlot(slot)
        if (off < 0) throw NxsError("ERR_FIELD_ABSENT", "slot $slot")
        return reader.readByte(off) != 0
    }

    fun getStrBySlot(slot: Int): String {
        val off = resolveSlot(slot)
        if (off < 0) throw NxsError("ERR_FIELD_ABSENT", "slot $slot")
        return reader.readStr(off)
    }
}
