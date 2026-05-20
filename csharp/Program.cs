// NXS C# reader smoke tests + optional bench + conformance runner
// Run: dotnet run -- <fixtures_dir>
//      dotnet run -- <fixtures_dir> --bench
//      dotnet run -- --conformance <conformance_dir>
using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using System.Text.Json.Nodes;
using Nxs;
using Nxs.Conformance;

// Conformance mode
if (args.Length > 0 && args[0] == "--conformance")
{
    string confDir = args.Length > 1 ? args[1] : "../conformance";
    return ConformanceRunner.Run(new[] { confDir });
}

string dir = args.Length > 0 ? args[0] : "../js/fixtures";
string nxbPath = Path.Combine(dir, "records_1000.nxb");
string jsonPath = Path.Combine(dir, "records_1000.json");

if (!File.Exists(nxbPath))
{
    Console.WriteLine($"fixtures not found at {dir}");
    Console.WriteLine("generate them: cargo run --release --bin gen_fixtures -- js/fixtures");
    return 1;
}

byte[] nxbData = File.ReadAllBytes(nxbPath);
var jsonArr = JsonNode.Parse(File.ReadAllText(jsonPath))!.AsArray();

int passed = 0, failed = 0;

void Check(string name, bool expr)
{
    if (expr) { Console.WriteLine($"  ✓ {name}"); passed++; }
    else { Console.WriteLine($"  ✗ {name}"); failed++; }
}

Console.WriteLine("\nNXS C# Reader — Tests\n");

var r = new NxsReader(nxbData);
Check("opens without error", true);
Check("reads correct record count", r.RecordCount == 1000);
Check("reads schema keys",
    Array.IndexOf(r.Keys, "id") >= 0 &&
    Array.IndexOf(r.Keys, "username") >= 0 &&
    Array.IndexOf(r.Keys, "score") >= 0);

var obj0 = r.Record(0);
Check("record(0) id matches JSON",
    obj0.GetI64("id") == jsonArr[0]!["id"]!.GetValue<long>());

var obj42 = r.Record(42);
Check("record(42) username matches JSON",
    obj42.GetStr("username") == jsonArr[42]!["username"]!.GetValue<string>());

var obj500 = r.Record(500);
Check("record(500) score close to JSON",
    Math.Abs(obj500.GetF64("score") - jsonArr[500]!["score"]!.GetValue<double>()) < 0.001);

var obj999 = r.Record(999);
Check("record(999) active matches JSON",
    obj999.GetBool("active") == jsonArr[999]!["active"]!.GetValue<bool>());

bool threw = false;
try { r.Record(10000); } catch (NxsException) { threw = true; }
Check("out-of-bounds throws NxsException", threw);

double sumNXS = r.SumF64("score");
double sumJSON = 0;
foreach (var rec in jsonArr) sumJSON += rec!["score"]!.GetValue<double>();
Check("sum_f64 matches JSON sum", Math.Abs(sumNXS - sumJSON) < 0.01);

Check("sum_i64(id) positive", r.SumI64("id") > 0);

double? mn = r.MinF64("score"), mx = r.MaxF64("score");
Check("min_f64 <= max_f64", mn.HasValue && mx.HasValue && mn.Value <= mx.Value);

// ── Writer round-trip tests ────────────────────────────────────────────────

Console.WriteLine("\nNXS C# Writer — Tests\n");

// 3-record round-trip
{
    var schema = new NxsSchema(["id", "username", "score", "active"]);
    var w = new NxsWriter(schema);
    (long id, string name, double score, bool active)[] recs =
        [(1L, "alice", 9.5, true), (2L, "bob", 7.2, false), (3L, "carol", 8.8, true)];
    foreach (var (id, name, score, active) in recs)
    {
        w.BeginObject();
        w.WriteI64(0, id); w.WriteStr(1, name); w.WriteF64(2, score); w.WriteBool(3, active);
        w.EndObject();
    }
    var rt = new NxsReader(w.Finish());
    Check("writer round-trip: record count", rt.RecordCount == 3);
    Check("writer round-trip: record(0) id", rt.Record(0).GetI64("id") == 1L);
    Check("writer round-trip: record(1) username", rt.Record(1).GetStr("username") == "bob");
    Check("writer round-trip: record(2) score", Math.Abs(rt.Record(2).GetF64("score") - 8.8) < 1e-9);
    Check("writer round-trip: record(0) active", rt.Record(0).GetBool("active") == true);
    Check("writer round-trip: record(1) active", rt.Record(1).GetBool("active") == false);
}

// fromRecords convenience
{
    var bytes2 = NxsWriter.FromRecords(
        ["id", "name", "value"],
        [new Dictionary<string, object?> { ["id"] = 10L, ["name"] = "foo", ["value"] = 1.5 },
         new Dictionary<string, object?> { ["id"] = 20L, ["name"] = "bar", ["value"] = 2.5 }]);
    var rt2 = new NxsReader(bytes2);
    Check("writer fromRecords: record count", rt2.RecordCount == 2);
    Check("writer fromRecords: record(1) name", rt2.Record(1).GetStr("name") == "bar");
}

// null field
{
    var wn = new NxsWriter(new NxsSchema(["a", "b"]));
    wn.BeginObject(); wn.WriteI64(0, 99L); wn.WriteNull(1); wn.EndObject();
    var rtn = new NxsReader(wn.Finish());
    Check("writer null field: a == 99", rtn.Record(0).GetI64("a") == 99L);
}

// bool fields
{
    var wb = new NxsWriter(new NxsSchema(["flag"]));
    wb.BeginObject(); wb.WriteBool(0, true); wb.EndObject();
    wb.BeginObject(); wb.WriteBool(0, false); wb.EndObject();
    var rtb = new NxsReader(wb.Finish());
    Check("writer bool: record(0) true", rtb.Record(0).GetBool("flag") == true);
    Check("writer bool: record(1) false", rtb.Record(1).GetBool("flag") == false);
}

// unicode string
{
    var wu = new NxsWriter(new NxsSchema(["msg"]));
    wu.BeginObject(); wu.WriteStr(0, "héllo wörld"); wu.EndObject();
    var rtu = new NxsReader(wu.Finish());
    Check("writer unicode string", rtu.Record(0).GetStr("msg") == "héllo wörld");
}

// many fields (>7 — multi-byte bitmask)
{
    string[] keys = ["f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8"];
    var wm = new NxsWriter(new NxsSchema(keys));
    wm.BeginObject();
    for (int i = 0; i < keys.Length; i++) wm.WriteI64(i, (long)(i * 100));
    wm.EndObject();
    var rtm = new NxsReader(wm.Finish());
    bool allOk = true;
    for (int i = 0; i < keys.Length; i++)
        if (rtm.Record(0).GetI64(keys[i]) != (long)(i * 100)) { allOk = false; break; }
    Check("writer many fields (multi-byte bitmask)", allOk);
}

// ── Query engine tests ─────────────────────────────────────────────────────

Console.WriteLine("\nNXS C# Query Engine — Tests\n");

// Pre-compute expected values from the JSON fixture so assertions are exact.
int jsonActiveCount = 0;
int jsonScoreGt5Count = 0;
int jsonActiveAndScoreGt5Count = 0;
string jsonFirstActiveUsername = "";
foreach (var jrec in jsonArr)
{
    bool active = jrec!["active"]!.GetValue<bool>();
    double score = jrec!["score"]!.GetValue<double>();
    if (active) { jsonActiveCount++; if (jsonFirstActiveUsername == "") jsonFirstActiveUsername = jrec!["username"]!.GetValue<string>(); }
    if (score > 5.0) jsonScoreGt5Count++;
    if (active && score > 5.0) jsonActiveAndScoreGt5Count++;
}

// Test 1 — filter active == true, count
{
    int got = r.Where(Pred.Eq("active", (object)true)).Count();
    Check("query Eq(active,true).Count() matches JSON", got == jsonActiveCount);
}

// Test 2 — filter score > 5.0, count
{
    int got = r.Where(Pred.Gt("score", 5.0)).Count();
    Check("query Gt(score,5.0).Count() matches JSON", got == jsonScoreGt5Count);
}

// Test 3 — And(Eq active, Gt score)
{
    int got = r.Where(Pred.And(Pred.Eq("active", (object)true), Pred.Gt("score", 5.0))).Count();
    Check("query And(Eq active, Gt score).Count() matches JSON", got == jsonActiveAndScoreGt5Count);
}

// Test 4 — First() returns the correct username for the first active record
{
    var first = r.Where(Pred.Eq("active", (object)true)).First();
    Check("query First() on active==true returns correct username",
        first is not null && first.GetStr("username") == jsonFirstActiveUsername);
}

// Test 5 — All().Count() == RecordCount
{
    int got = r.All().Count();
    Check("query All().Count() == RecordCount", got == r.RecordCount);
}

Console.WriteLine($"\n{passed} passed, {failed} failed\n");

if (args.Length > 1 && args[1] == "--bench")
    Bench.Run(dir);

return failed > 0 ? 1 : 0;
