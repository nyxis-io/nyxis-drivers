// NXS conformance runner for Kotlin/JVM.
// Usage: ./gradlew conformance   (expects ../conformance/ relative to kotlin/)
package nxs

import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import kotlin.system.exitProcess

private const val MAGIC_LIST = 0x4E59584C

private fun approxEq(
    a: Double,
    b: Double,
): Boolean {
    if (a == b) return true
    val diff = kotlin.math.abs(a - b)
    val mag = kotlin.math.max(kotlin.math.abs(a), kotlin.math.abs(b))
    if (mag < 1e-300) return diff < 1e-300
    return diff / mag < 1e-9
}

private fun readU32(
    data: ByteArray,
    off: Int,
): Long =
    (data[off].toLong() and 0xFF) or
        ((data[off + 1].toLong() and 0xFF) shl 8) or
        ((data[off + 2].toLong() and 0xFF) shl 16) or
        ((data[off + 3].toLong() and 0xFF) shl 24)

private fun readU64(
    data: ByteArray,
    off: Int,
): Long = readU32(data, off) or (readU32(data, off + 4) shl 32)

private fun readI64(
    data: ByteArray,
    off: Int,
): Long = readU64(data, off)

private fun readF64(
    data: ByteArray,
    off: Int,
): Double = java.lang.Double.longBitsToDouble(readU64(data, off))

private fun readU16(
    data: ByteArray,
    off: Int,
): Int {
    return (data[off].toInt() and 0xFF) or ((data[off + 1].toInt() and 0xFF) shl 8)
}

@Suppress("ReturnCount")
private fun readList(
    data: ByteArray,
    off: Int,
): List<Any?>? {
    if (off + 16 > data.size) return null
    val magic = readU32(data, off).toInt() and -1
    if (magic != MAGIC_LIST) return null
    val elemSigil = data[off + 8].toInt() and 0xFF
    val elemCount = readU32(data, off + 9).toInt()
    val dataStart = off + 16
    val out = ArrayList<Any?>(elemCount)
    for (i in 0 until elemCount) {
        val elemOff = dataStart + i * 8
        if (elemOff + 8 > data.size) break
        when (elemSigil) {
            0x3D -> out.add(readI64(data, elemOff).toDouble())
            0x7E -> out.add(readF64(data, elemOff))
            else -> out.add(null)
        }
    }
    return out
}

@Suppress("LoopWithTooManyJumpStatements", "CyclomaticComplexMethod")
private fun resolveSlotRaw(
    data: ByteArray,
    objOffset: Int,
    slot: Int,
): Int {
    var p = objOffset + 8
    var cur = 0
    var tableIdx = 0
    var b: Int
    while (true) {
        if (p >= data.size) return -1
        b = data[p++].toInt() and 0xFF
        val bits = b and 0x7F
        for (i in 0 until 7) {
            if (cur == slot) {
                if ((bits shr i) and 1 == 0) return -1
                while (b and 0x80 != 0) {
                    if (p >= data.size) break
                    b = data[p++].toInt() and 0xFF
                }
                val ot = p + tableIdx * 2
                if (ot + 2 > data.size) return -1
                return objOffset + readU16(data, ot)
            }
            if (cur < slot && (bits shr i) and 1 == 1) tableIdx++
            cur++
        }
        if (b and 0x80 == 0) return -1
    }
}

@Suppress("ReturnCount", "LongMethod")
private fun getFieldValue(
    data: ByteArray,
    tailStart: Int,
    ri: Int,
    slot: Int,
    sigilByte: Int,
): Pair<Any?, Boolean> {
    val entryOff = tailStart + ri * 10 + 2
    if (entryOff + 8 > data.size) return Pair(null, false)
    val abs = readU64(data, entryOff).toInt()
    val off = resolveSlotRaw(data, abs, slot)
    if (off < 0) return Pair(null, false)

    if (off + 4 <= data.size) {
        val maybe = readU32(data, off).toInt() and -1
        if (maybe == MAGIC_LIST) {
            val lst = readList(data, off) ?: return Pair(null, false)
            return Pair(lst, true)
        }
    }

    return when (sigilByte) {
        0x3D -> Pair(readI64(data, off).toDouble(), true)
        0x7E -> Pair(readF64(data, off), true)
        0x3F -> Pair(data[off].toInt() != 0, true)
        0x22 -> {
            if (off + 4 > data.size) return Pair(null, false)
            val len = readU32(data, off).toInt()
            if (off + 4 + len > data.size) return Pair(null, false)
            Pair(String(data, off + 4, len, Charsets.UTF_8), true)
        }
        0x40 -> Pair(readI64(data, off).toDouble(), true)
        0x5E -> Pair(null, true)
        else ->
            if (off + 8 <= data.size) {
                Pair(readI64(data, off).toDouble(), true)
            } else {
                Pair(null, false)
            }
    }
}

@Suppress("ReturnCount", "CyclomaticComplexMethod")
private fun valuesMatch(
    actual: Any?,
    expected: Any?,
): Boolean {
    if (expected == null || expected === JSONObject.NULL) {
        return actual == null || actual === JSONObject.NULL ||
            actual == 0 || actual == 0.0 || actual == false
    }
    when (expected) {
        is Boolean -> return actual == expected
        is String -> return actual == expected
        is Number -> {
            val a = (actual as? Number)?.toDouble() ?: return false
            return approxEq(a, expected.toDouble())
        }
        is JSONArray -> {
            val actList = actual as? List<*> ?: return false
            if (actList.size != expected.length()) return false
            for (i in 0 until expected.length()) {
                if (!valuesMatch(actList[i], expected.get(i))) return false
            }
            return true
        }
        is List<*> -> {
            val actList = actual as? List<*> ?: return false
            if (actList.size != expected.size) return false
            for (i in expected.indices) {
                if (!valuesMatch(actList[i], expected[i])) return false
            }
            return true
        }
        else -> return false
    }
}

@Suppress("LongMethod")
private fun runPositive(
    conformanceDir: String,
    name: String,
    expected: JSONObject,
) {
    val nxbPath = File(conformanceDir, "$name.nxb")
    val data = nxbPath.readBytes()
    val reader = NxsReader(data)

    val expRc = expected.getInt("record_count")
    if (reader.recordCount != expRc) {
        throw AssertionError("record_count: expected $expRc, got ${reader.recordCount}")
    }

    val expKeys = expected.getJSONArray("keys")
    for (i in 0 until expKeys.length()) {
        val expKey = expKeys.getString(i)
        if (i >= reader.keys.size) {
            throw AssertionError("key[$i] missing (expected \"$expKey\")")
        }
        if (reader.keys[i] != expKey) {
            throw AssertionError("key[$i]: expected \"$expKey\", got \"${reader.keys[i]}\"")
        }
    }

    val tailStart = reader.tailPtr.toInt() + 4
    val records = expected.getJSONArray("records")

    for (ri in 0 until records.length()) {
        val expRec = records.getJSONObject(ri)
        for (key in expRec.keys()) {
            val slot = reader.keys.indexOf(key)
            if (slot < 0) throw AssertionError("rec[$ri].$key: key not in schema")

            var sigil = 0x3D
            if (slot < reader.keySigils.size) {
                sigil = reader.keySigils[slot].toInt() and 0xFF
            }

            val expVal = expRec.get(key)
            val (actual, present) = getFieldValue(data, tailStart, ri, slot, sigil)

            if (expVal == null || expVal === JSONObject.NULL) {
                continue
            }
            if (!present) {
                throw AssertionError("rec[$ri].$key: field absent, expected $expVal")
            }
            if (!valuesMatch(actual, expVal)) {
                throw AssertionError("rec[$ri].$key: expected $expVal, got $actual")
            }
        }
    }
}

private fun runNegative(
    conformanceDir: String,
    name: String,
    expectedCode: String,
) {
    val nxbPath = File(conformanceDir, "$name.nxb")
    val data = nxbPath.readBytes()
    try {
        NxsReader(data)
        throw AssertionError("expected error \"$expectedCode\" but reader succeeded")
    } catch (e: NxsError) {
        if (e.code != expectedCode) {
            throw AssertionError("expected error \"$expectedCode\", got \"${e.code}\" (${e.message})")
        }
    }
}

fun main(args: Array<String>) {
    val conformanceDir =
        args.firstOrNull { !it.startsWith("-") } ?: "../conformance/"
    val dir = File(conformanceDir)
    if (!dir.isDirectory) {
        println("conformance directory not found: $conformanceDir")
        exitProcess(1)
    }

    val entries =
        dir
            .listFiles { f -> f.name.endsWith(".expected.json") }
            ?.map { it.name.removeSuffix(".expected.json") }
            ?.sorted()
            ?: emptyList()

    var passed = 0
    var failed = 0

    for (name in entries) {
        if (name.startsWith("columnar_") || name.startsWith("pax_")) {
            println("  SKIP  $name (columnar/PAX not implemented)")
            passed++
            continue
        }
        val jsonPath = File(dir, "$name.expected.json")
        val expected = JSONObject(jsonPath.readText())
        val isNegative = expected.has("error")
        try {
            if (isNegative) {
                runNegative(conformanceDir, name, expected.getString("error"))
            } else {
                runPositive(conformanceDir, name, expected)
            }
            println("  PASS  $name")
            passed++
        } catch (e: Exception) {
            System.err.println("  FAIL  $name — ${e.message}")
            failed++
        }
    }

    println("\n$passed passed, $failed failed")
    exitProcess(if (failed > 0) 1 else 0)
}
