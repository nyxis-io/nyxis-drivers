package nxs

import org.json.JSONArray
import java.io.File

fun benchMs(
    label: String,
    baseline: Double = 0.0,
    runs: Int = 5,
    body: () -> Unit,
): Double {
    var best = Double.MAX_VALUE
    repeat(runs) {
        val t0 = System.nanoTime()
        body()
        val ms = (System.nanoTime() - t0) / 1_000_000.0
        if (ms < best) best = ms
    }
    val rel = if (baseline > 0) "  %.1fx faster".format(baseline / best) else ""
    println("  │  %-28s  %7.2f ms%s".format(label, best, rel))
    return best
}

// JSON column scan via org.json
fun jsonSumScore(jsonBytes: ByteArray): Double {
    val arr = JSONArray(String(jsonBytes, Charsets.UTF_8))
    var sum = 0.0
    for (i in 0 until arr.length()) sum += arr.getJSONObject(i).getDouble("score")
    return sum
}

// CSV raw scan — score is column 6 (0-based)
fun csvSumScore(csvBytes: ByteArray): Double {
    var sum = 0.0
    var line = 0
    var p = 0
    val size = csvBytes.size
    while (p < size) {
        var rowEnd = p
        while (rowEnd < size && csvBytes[rowEnd] != '\n'.code.toByte()) rowEnd++
        if (line > 0) {
            var col = p
            var c = 0
            while (c < 6 && col < rowEnd) {
                while (col < rowEnd && csvBytes[col] != ','.code.toByte()) col++
                col++
                c++
            }
            if (c == 6 && col < rowEnd) {
                var end = col
                while (end < rowEnd && csvBytes[end] != ','.code.toByte() && csvBytes[end] != '\r'.code.toByte()) end++
                sum += String(csvBytes, col, end - col, Charsets.UTF_8).toDoubleOrNull() ?: 0.0
            }
        }
        line++
        p = rowEnd + 1
    }
    return sum
}

fun main(args: Array<String>) = runBench(args)

fun runBench(args: Array<String>) {
    val dir = args.firstOrNull() ?: "../js/fixtures"
    val nxbFile = File("$dir/records_1000000.nxb")
    val jsonFile = File("$dir/records_1000000.json")
    val csvFile = File("$dir/records_1000000.csv")

    if (!nxbFile.exists()) {
        println("fixture not found: ${nxbFile.path}")
        println("generate: cargo run --release --bin gen_fixtures -- js/fixtures 1000000")
        return
    }

    val nxbBytes = nxbFile.readBytes()
    val jsonBytes = jsonFile.readBytes()
    val csvBytes = csvFile.readBytes()

    val r = NxsReader(nxbBytes)
    println("\nNXS Kotlin Benchmark — ${r.recordCount} records")
    println(
        "  .nxb %.2f MB   .json %.2f MB   .csv %.2f MB\n"
            .format(nxbBytes.size / 1e6, jsonBytes.size / 1e6, csvBytes.size / 1e6),
    )

    // Warm-up JIT
    repeat(2) {
        r.sumF64("score")
        r.sumI64("id")
    }

    println("  ┌─ sum(score) ─────────────────────────────────────────────────────────┐")
    val jsonMs = benchMs("JSON parse + loop") { jsonSumScore(jsonBytes) }
    benchMs("CSV raw scan", baseline = jsonMs) { csvSumScore(csvBytes) }
    benchMs("NXS sumF64", baseline = jsonMs) { r.sumF64("score") }
    println("  └──────────────────────────────────────────────────────────────────────┘\n")

    println("  ┌─ sum(id) ────────────────────────────────────────────────────────────┐")
    benchMs("NXS sumI64") { r.sumI64("id") }
    println("  └──────────────────────────────────────────────────────────────────────┘\n")

    println("  ┌─ random access ×1000 ────────────────────────────────────────────────┐")
    benchMs("NXS record(k).getF64") {
        for (i in 0 until 1000) {
            r.record(i * 997 % r.recordCount).getF64("score")
        }
    }
    println("  └──────────────────────────────────────────────────────────────────────┘\n")
}
