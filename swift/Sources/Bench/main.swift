// NXS Swift reader benchmark — NXS vs JSONSerialization vs raw CSV scan
// Run: swift run -c release nxs-bench <fixtures_dir>
import Foundation
import NXS

let dir = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "../js/fixtures"
let nxbURL  = URL(fileURLWithPath: "\(dir)/records_1000000.nxb")
let jsonURL = URL(fileURLWithPath: "\(dir)/records_1000000.json")
let csvURL  = URL(fileURLWithPath: "\(dir)/records_1000000.csv")

guard let nxbData  = try? Data(contentsOf: nxbURL),
      let jsonData = try? Data(contentsOf: jsonURL),
      let csvData  = try? Data(contentsOf: csvURL)
else {
    print("fixtures not found in \(dir)")
    print("generate: cargo run --release --bin gen_fixtures -- js/fixtures 1000000")
    exit(1)
}

let reader = try! NXSReader(nxbData)
let RUNS = 5

func benchMs(_ label: String, baseline: Double = 0, _ body: () -> Void) -> Double {
    var best = Double.infinity
    for _ in 0..<RUNS {
        let t0 = Date()
        body()
        let ms = Date().timeIntervalSince(t0) * 1000
        if ms < best { best = ms }
    }
    let rel = baseline > 0 ? String(format: "  %.1fx faster", baseline / best) : ""
    print("  │  \(label.padding(toLength: 28, withPad: " ", startingAt: 0))  \(String(format: "%7.2f", best)) ms\(rel)")
    return best
}

// ── JSON column scan — Foundation JSONSerialization ───────────────────────────
func jsonSumScore() -> Double {
    guard let arr = try? JSONSerialization.jsonObject(with: jsonData) as? [[String: Any]] else { return 0 }
    return arr.reduce(0.0) { $0 + (($1["score"] as? NSNumber)?.doubleValue ?? 0) }
}

// ── CSV column scan — raw bytes, score is column 6 ────────────────────────────
func csvSumScore() -> Double {
    var sum = 0.0
    csvData.withUnsafeBytes { ptr in
        let base = ptr.baseAddress!.assumingMemoryBound(to: UInt8.self)
        let size = ptr.count
        var p = 0; var line = 0
        while p < size {
            var rowEnd = p
            while rowEnd < size && base[rowEnd] != UInt8(ascii: "\n") { rowEnd += 1 }
            if line > 0 {
                var col = p; var c = 0
                while c < 6 && col < rowEnd {
                    while col < rowEnd && base[col] != UInt8(ascii: ",") { col += 1 }
                    col += 1; c += 1
                }
                if c == 6 && col < rowEnd {
                    // parse float manually
                    var v = 0.0; var frac = 0.0; var div = 1.0; var hasFrac = false
                    var neg = false
                    if col < rowEnd && base[col] == UInt8(ascii: "-") { neg = true; col += 1 }
                    while col < rowEnd && base[col] != UInt8(ascii: ",") && base[col] != UInt8(ascii: "\r") {
                        let ch = base[col]
                        if ch == UInt8(ascii: ".") { hasFrac = true }
                        else if ch >= 48 && ch <= 57 {
                            if hasFrac { div *= 10; frac = frac * 10 + Double(ch - 48) }
                            else       { v = v * 10 + Double(ch - 48) }
                        }
                        col += 1
                    }
                    sum += (neg ? -1 : 1) * (v + frac / div)
                }
            }
            line += 1; p = rowEnd + 1
        }
    }
    return sum
}

print("NXS Swift Benchmark — \(reader.recordCount) records")
print("  .nxb \(String(format: "%.2f", Double(nxbData.count)/1e6)) MB   .json \(String(format: "%.2f", Double(jsonData.count)/1e6)) MB   .csv \(String(format: "%.2f", Double(csvData.count)/1e6)) MB\n")

print("  ┌─ sum(score) ─────────────────────────────────────────────────────────┐")
let jsonMs = benchMs("JSONSerialization + loop") { _ = jsonSumScore() }
let csvMs  = benchMs("CSV raw scan", baseline: jsonMs, { _ = csvSumScore() })
let nxsMs  = benchMs("NXS sumF64", baseline: jsonMs, { _ = try! reader.sumF64("score") })
print("  └──────────────────────────────────────────────────────────────────────┘\n")

print("  ┌─ sum(id) ────────────────────────────────────────────────────────────┐")
_ = benchMs("NXS sumI64") { _ = try! reader.sumI64("id") }
print("  └──────────────────────────────────────────────────────────────────────┘\n")

print("  ┌─ random access ×1000 ────────────────────────────────────────────────┐")
_ = benchMs("NXS record(k).getF64") {
    for i in 0..<1000 {
        let obj = try! reader.record(i * 997 % reader.recordCount)
        _ = try! obj.getF64("score")
    }
}
print("  └──────────────────────────────────────────────────────────────────────┘\n")
