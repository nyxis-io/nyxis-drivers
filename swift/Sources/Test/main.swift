// NXS Swift reader smoke tests
// Run: swift run nxs-test <fixtures_dir>
import Foundation
import NXS

let fixtureDir = CommandLine.arguments.count > 1
    ? CommandLine.arguments[1]
    : "../js/fixtures"

var passed = 0, failed = 0

func check(_ name: String, _ expr: Bool) {
    if expr { print("  ✓ \(name)"); passed += 1 }
    else     { print("  ✗ \(name)"); failed += 1 }
}
func checkThrows(_ name: String, _ body: () throws -> Void) {
    do { try body(); print("  ✗ \(name) — expected throw"); failed += 1 }
    catch { print("  ✓ \(name)"); passed += 1 }
}

print("\nNXS Swift Reader — Tests\n")

let nxbURL  = URL(fileURLWithPath: "\(fixtureDir)/records_1000.nxb")
let jsonURL = URL(fileURLWithPath: "\(fixtureDir)/records_1000.json")

guard let nxbData = try? Data(contentsOf: nxbURL) else {
    print("fixtures not found at \(fixtureDir)")
    print("generate them: cargo run --release --bin gen_fixtures -- js/fixtures")
    exit(1)
}
let jsonData = try! Data(contentsOf: jsonURL)
let json = try! JSONSerialization.jsonObject(with: jsonData) as! [[String: Any]]

do {
    let r = try NXSReader(nxbData)
    check("opens without error", true)
    check("reads correct record count", r.recordCount == 1000)
    check("reads schema keys", r.keys.contains("id") && r.keys.contains("username") && r.keys.contains("score"))

    let obj0 = try r.record(0)
    let id0 = try obj0.getI64("id")
    check("record(0) id matches JSON", id0 == (json[0]["id"] as! NSNumber).int64Value)

    let obj42 = try r.record(42)
    let u42 = try obj42.getStr("username")
    check("record(42) username matches JSON", u42 == (json[42]["username"] as! String))

    let obj500 = try r.record(500)
    let s500 = try obj500.getF64("score")
    let js500 = (json[500]["score"] as! NSNumber).doubleValue
    check("record(500) score close to JSON", abs(s500 - js500) < 0.001)

    let obj999 = try r.record(999)
    let a999 = try obj999.getBool("active")
    check("record(999) active matches JSON", a999 == (json[999]["active"] as! Bool))

    checkThrows("out-of-bounds record throws") { _ = try r.record(10000) }

    let sumNXS = try r.sumF64("score")
    let sumJSON = json.reduce(0.0) { $0 + ($1["score"] as! NSNumber).doubleValue }
    check("sum_f64 matches JSON sum", abs(sumNXS - sumJSON) < 0.01)

    let sumId = try r.sumI64("id")
    check("sum_i64(id) positive", sumId > 0)

    if let mn = try r.minF64("score"), let mx = try r.maxF64("score") {
        check("min_f64 <= max_f64", mn <= mx)
    }

} catch {
    print("  ✗ fatal: \(error)")
    failed += 1
}

// ── Query engine tests ─────────────────────────────────────────────────────

print("\nNXS Swift Query Engine — Tests\n")

do {
    let r = try NXSReader(nxbData)

    // 1. filter active == true, count
    let activeCount = r.where(eq("active", true)).count()
    let activeJSON  = json.filter { ($0["active"] as! Bool) == true }.count
    check("query: eq(active,true) count matches JSON", activeCount == activeJSON)

    // 2. filter score > 80.0, count
    let highCount = r.where(gt("score", 80.0)).count()
    let highJSON  = json.filter { ($0["score"] as! NSNumber).doubleValue > 80.0 }.count
    check("query: gt(score,80) count matches JSON", highCount == highJSON)

    // 3. and(eq active, gt score)
    let combo = r.where(and(eq("active", true), gt("score", 80.0))).count()
    let comboJSON = json.filter {
        ($0["active"] as! Bool) == true && ($0["score"] as! NSNumber).doubleValue > 80.0
    }.count
    check("query: and(active==true, score>80) count matches JSON", combo == comboJSON)

    // 4. first() username check
    if let firstActive = r.where(eq("active", true)).first(),
       let firstJSON = json.first(where: { ($0["active"] as! Bool) == true }) {
        let username = try firstActive.getStr("username")
        check("query: first(active==true).username matches JSON", username == (firstJSON["username"] as! String))
    } else {
        check("query: first(active==true) found", false)
    }

    // 5. reader.all.count() == recordCount
    check("query: all.count() == recordCount", r.all.count() == r.recordCount)

} catch {
    print("  ✗ query fatal: \(error)")
    failed += 1
}

// ── Writer round-trip tests ────────────────────────────────────────────────

print("\nNXS Swift Writer — Tests\n")

do {
    // 3-record round-trip
    let schema = NXSSchema(keys: ["id", "username", "score", "active"])
    let w = NXSWriter(schema: schema)
    let recs: [(Int64, String, Double, Bool)] = [(1, "alice", 9.5, true), (2, "bob", 7.2, false), (3, "carol", 8.8, true)]
    for (id, name, score, active) in recs {
        w.beginObject()
        w.writeI64(slot: 0, value: id)
        w.writeStr(slot: 1, value: name)
        w.writeF64(slot: 2, value: score)
        w.writeBool(slot: 3, value: active)
        w.endObject()
    }
    let rt = try NXSReader(Data(w.finish()))
    check("writer round-trip: record count", rt.recordCount == 3)
    let o0 = try rt.record(0)
    check("writer round-trip: record(0) id", (try? o0.getI64("id")) == 1)
    let o1 = try rt.record(1)
    check("writer round-trip: record(1) username", (try? o1.getStr("username")) == "bob")
    let o2 = try rt.record(2)
    check("writer round-trip: record(2) score", abs(((try? o2.getF64("score")) ?? 0) - 8.8) < 1e-9)
    check("writer round-trip: record(0) active", (try? o0.getBool("active")) == true)
    check("writer round-trip: record(1) active", (try? o1.getBool("active")) == false)

    // fromRecords convenience
    let bytes2 = NXSWriter.fromRecords(keys: ["id", "name", "value"],
        records: [["id": 10, "name": "foo", "value": 1.5], ["id": 20, "name": "bar", "value": 2.5]])
    let rt2 = try NXSReader(Data(bytes2))
    check("writer fromRecords: record count", rt2.recordCount == 2)
    check("writer fromRecords: record(1) name", (try? (try rt2.record(1)).getStr("name")) == "bar")

    // null field
    let wn = NXSWriter(schema: NXSSchema(keys: ["a", "b"]))
    wn.beginObject(); wn.writeI64(slot: 0, value: 99); wn.writeNull(slot: 1); wn.endObject()
    let rtn = try NXSReader(Data(wn.finish()))
    check("writer null field: a == 99", (try? (try rtn.record(0)).getI64("a")) == 99)

    // bool fields
    let wb = NXSWriter(schema: NXSSchema(keys: ["flag"]))
    wb.beginObject(); wb.writeBool(slot: 0, value: true);  wb.endObject()
    wb.beginObject(); wb.writeBool(slot: 0, value: false); wb.endObject()
    let rtb = try NXSReader(Data(wb.finish()))
    check("writer bool: record(0) true",  (try? (try rtb.record(0)).getBool("flag")) == true)
    check("writer bool: record(1) false", (try? (try rtb.record(1)).getBool("flag")) == false)

    // unicode string
    let wu = NXSWriter(schema: NXSSchema(keys: ["msg"]))
    wu.beginObject(); wu.writeStr(slot: 0, value: "héllo wörld"); wu.endObject()
    let rtu = try NXSReader(Data(wu.finish()))
    check("writer unicode string", (try? (try rtu.record(0)).getStr("msg")) == "héllo wörld")

    // many fields (>7 — multi-byte bitmask)
    let manyKeys = (0..<9).map { "f\($0)" }
    let wm = NXSWriter(schema: NXSSchema(keys: manyKeys))
    wm.beginObject()
    for i in 0..<9 { wm.writeI64(slot: i, value: Int64(i * 100)) }
    wm.endObject()
    let rtm = try NXSReader(Data(wm.finish()))
    let o0m = try rtm.record(0)
    let manyOk = (0..<9).allSatisfy { i in (try? o0m.getI64(manyKeys[i])) == Int64(i * 100) }
    check("writer many fields (multi-byte bitmask)", manyOk)

} catch {
    print("  ✗ writer fatal: \(error)")
    failed += 1
}

print("\n\(passed) passed, \(failed) failed\n")
exit(failed > 0 ? 1 : 0)
