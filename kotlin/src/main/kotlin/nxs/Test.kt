// NXS Kotlin reader smoke tests
// Run: gradle run --args="<fixtures_dir>"
package nxs

import java.io.File
import kotlin.math.abs

fun main(args: Array<String>) {
    val dir = args.firstOrNull() ?: "../js/fixtures"
    val nxbFile = File("$dir/records_1000.nxb")
    val jsonFile = File("$dir/records_1000.json")

    if (!nxbFile.exists()) {
        println("fixtures not found at $dir")
        println("generate them: cargo run --release --bin gen_fixtures -- js/fixtures")
        return
    }

    val nxbData = nxbFile.readBytes()

    @Suppress("UNCHECKED_CAST")
    val jsonList =
        org.json.JSONArray(jsonFile.readText()).let { arr ->
            (0 until arr.length()).map { arr.getJSONObject(it) }
        }

    var passed = 0
    var failed = 0

    fun check(
        name: String,
        expr: Boolean,
    ) {
        if (expr) {
            println("  ✓ $name")
            passed++
        } else {
            println("  ✗ $name")
            failed++
        }
    }

    println("\nNXS Kotlin Reader — Tests\n")

    val r = NxsReader(nxbData)
    check("opens without error", true)
    check("reads correct record count", r.recordCount == 1000)
    check(
        "reads schema keys",
        r.keys.containsAll(listOf("id", "username", "email", "score", "active")),
    )

    val obj0 = r.record(0)
    check(
        "record(0) id matches JSON",
        obj0.getI64("id") == jsonList[0].getLong("id"),
    )

    val obj42 = r.record(42)
    check(
        "record(42) username matches JSON",
        obj42.getStr("username") == jsonList[42].getString("username"),
    )

    val obj500 = r.record(500)
    check(
        "record(500) score close to JSON",
        abs(obj500.getF64("score") - jsonList[500].getDouble("score")) < 0.001,
    )

    val obj999 = r.record(999)
    check(
        "record(999) active matches JSON",
        obj999.getBool("active") == jsonList[999].getBoolean("active"),
    )

    var threw = false
    try {
        r.record(10000)
    } catch (e: NxsError) {
        threw = true
    }
    check("out-of-bounds throws NxsError", threw)

    val sumNXS = r.sumF64("score")
    val sumJSON = jsonList.sumOf { it.getDouble("score") }
    check("sum_f64 matches JSON sum", abs(sumNXS - sumJSON) < 0.01)

    check("sum_i64(id) positive", r.sumI64("id") > 0)

    val mn = r.minF64("score")
    val mx = r.maxF64("score")
    check("min_f64 <= max_f64", mn != null && mx != null && mn <= mx)

    // ── Writer round-trip tests ────────────────────────────────────────────

    println("\nNXS Kotlin Writer — Tests\n")

    // 3-record round-trip
    run {
        val schema = NxsSchema(listOf("id", "username", "score", "active"))
        val w = NxsWriter(schema)
        val recs =
            listOf(
                Triple(Triple(1L, "alice", 9.5), true, Unit),
                Triple(Triple(2L, "bob", 7.2), false, Unit),
                Triple(Triple(3L, "carol", 8.8), true, Unit),
            )
        for ((kv, active, _) in recs) {
            val (id, name, score) = kv
            w.beginObject()
            w.writeI64(0, id)
            w.writeStr(1, name)
            w.writeF64(2, score)
            w.writeBool(3, active)
            w.endObject()
        }
        val rt = NxsReader(w.finish())
        check("writer round-trip: record count", rt.recordCount == 3)
        check("writer round-trip: record(0) id", rt.record(0).getI64("id") == 1L)
        check("writer round-trip: record(1) username", rt.record(1).getStr("username") == "bob")
        check("writer round-trip: record(2) score", abs(rt.record(2).getF64("score") - 8.8) < 1e-9)
        check("writer round-trip: record(0) active", rt.record(0).getBool("active") == true)
        check("writer round-trip: record(1) active", rt.record(1).getBool("active") == false)
    }

    // fromRecords convenience
    run {
        val bytes2 =
            NxsWriter.fromRecords(
                listOf("id", "name", "value"),
                listOf(
                    mapOf("id" to 10L, "name" to "foo", "value" to 1.5),
                    mapOf("id" to 20L, "name" to "bar", "value" to 2.5),
                ),
            )
        val rt2 = NxsReader(bytes2)
        check("writer fromRecords: record count", rt2.recordCount == 2)
        check("writer fromRecords: record(1) name", rt2.record(1).getStr("name") == "bar")
    }

    // null field
    run {
        val wn = NxsWriter(NxsSchema(listOf("a", "b")))
        wn.beginObject()
        wn.writeI64(0, 99)
        wn.writeNull(1)
        wn.endObject()
        val rtn = NxsReader(wn.finish())
        check("writer null field: a == 99", rtn.record(0).getI64("a") == 99L)
    }

    // bool fields
    run {
        val wb = NxsWriter(NxsSchema(listOf("flag")))
        wb.beginObject()
        wb.writeBool(0, true)
        wb.endObject()
        wb.beginObject()
        wb.writeBool(0, false)
        wb.endObject()
        val rtb = NxsReader(wb.finish())
        check("writer bool: record(0) true", rtb.record(0).getBool("flag") == true)
        check("writer bool: record(1) false", rtb.record(1).getBool("flag") == false)
    }

    // unicode string
    run {
        val wu = NxsWriter(NxsSchema(listOf("msg")))
        wu.beginObject()
        wu.writeStr(0, "héllo wörld")
        wu.endObject()
        val rtu = NxsReader(wu.finish())
        check("writer unicode string", rtu.record(0).getStr("msg") == "héllo wörld")
    }

    // many fields (>7 — multi-byte bitmask)
    run {
        val keys = (0..8).map { "f$it" }
        val wm = NxsWriter(NxsSchema(keys))
        wm.beginObject()
        keys.indices.forEach { i -> wm.writeI64(i, (i * 100).toLong()) }
        wm.endObject()
        val rtm = NxsReader(wm.finish())
        val allOk = keys.indices.all { i -> rtm.record(0).getI64(keys[i]) == (i * 100).toLong() }
        check("writer many fields (multi-byte bitmask)", allOk)
    }

    // ── Query engine tests ─────────────────────────────────────────────────

    println("\nNXS Kotlin Query Engine — Tests\n")

    // testQueryEqBool — filter active==true, compare count against manual loop
    run {
        val queryCount = r.where(eq("active", true)).count()
        var loopCount = 0
        for (i in 0 until r.recordCount) {
            if (r.record(i).getBool("active")) loopCount++
        }
        check("testQueryEqBool: where(eq active==true) count matches loop", queryCount == loopCount)
    }

    // testQueryGtFloat — filter score > 80.0
    run {
        val queryCount = r.where(gt("score", 80.0)).count()
        var loopCount = 0
        for (i in 0 until r.recordCount) {
            if (r.record(i).getF64("score") > 80.0) loopCount++
        }
        check("testQueryGtFloat: where(gt score > 80.0) count matches loop", queryCount == loopCount)
    }

    // testQueryAnd — eq("active", true) and gt("score", 80.0)
    run {
        val pred = eq("active", true) and gt("score", 80.0)
        val queryCount = r.where(pred).count()
        var loopCount = 0
        for (i in 0 until r.recordCount) {
            val rec = r.record(i)
            if (rec.getBool("active") && rec.getF64("score") > 80.0) loopCount++
        }
        check("testQueryAnd: active==true AND score>80 count matches loop", queryCount == loopCount)
    }

    // testQueryFirst — first() username matches record(0) username when eq by id
    run {
        val firstRec = r.where(eq("id", jsonList[0].getLong("id"))).first()
        check(
            "testQueryFirst: first() username matches JSON[0]",
            firstRec != null && firstRec.getStr("username") == jsonList[0].getString("username"),
        )
    }

    // testQueryAllCount — reader.all.count() == recordCount
    run {
        check("testQueryAllCount: all.count() == recordCount", r.all.count() == r.recordCount)
    }

    println("\n$passed passed, $failed failed\n")
    if (failed > 0) System.exit(1)
}
