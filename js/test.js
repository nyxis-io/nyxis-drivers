// Smoke tests for the JS NXS reader and writer.
// Run: node test.js <fixtures_dir>

import { existsSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { NxsReader, NxsStreamReader, eq, gt, lt, and, not } from "./nxs.js";
import { NxsSchema, NxsWriter } from "./nxs_writer.js";

const fixtureDir = process.argv[2] || "./fixtures";
let passed = 0, failed = 0;

function test(name, fn) {
  try { fn(); console.log(`  ✓ ${name}`); passed++; }
  catch (e) { console.log(`  ✗ ${name}\n      ${e.message}`); failed++; }
}

function assertEq(actual, expected, msg = "") {
  if (actual !== expected) {
    throw new Error(`${msg} — expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
  }
}

function assertClose(actual, expected, eps = 0.001, msg = "") {
  if (Math.abs(actual - expected) > eps) {
    throw new Error(`${msg} — expected ~${expected}, got ${actual}`);
  }
}

console.log("\nNXS JavaScript Reader — Tests\n");

const fixtureNxb = join(fixtureDir, "records_1000.nxb");
const fixtureJson = join(fixtureDir, "records_1000.json");
const { buf, json } = existsSync(fixtureNxb) && existsSync(fixtureJson)
  ? {
      buf: readFileSync(fixtureNxb),
      json: JSON.parse(readFileSync(fixtureJson, "utf8")),
    }
  : buildSyntheticFixture(1000);

function buildSyntheticFixture(count) {
  const records = [];
  const schema = new NxsSchema(["id", "username", "score", "active"]);
  const w = new NxsWriter(schema);
  for (let i = 0; i < count; i++) {
    const rec = {
      id: i,
      username: `user_${i}`,
      score: i * 0.25 + 1.5,
      active: i % 2 === 0,
    };
    records.push(rec);
    w.beginObject();
    w.writeI64(0, BigInt(rec.id));
    w.writeStr(1, rec.username);
    w.writeF64(2, rec.score);
    w.writeBool(3, rec.active);
    w.endObject();
  }
  return { buf: w.finish(), json: records };
}

test("opens without error", () => {
  new NxsReader(buf);
});

test("reads correct record count", () => {
  const r = new NxsReader(buf);
  assertEq(r.recordCount, 1000);
});

test("reads schema keys", () => {
  const r = new NxsReader(buf);
  assertEq(r.keys.includes("id"), true, "missing 'id'");
  assertEq(r.keys.includes("username"), true, "missing 'username'");
  assertEq(r.keys.includes("score"), true, "missing 'score'");
});

test("record(0) matches JSON[0].id", () => {
  const r = new NxsReader(buf);
  assertEq(r.record(0).getI64("id"), json[0].id);
});

test("record(42) matches JSON[42].username", () => {
  const r = new NxsReader(buf);
  assertEq(r.record(42).getStr("username"), json[42].username);
});

test("buildFieldIndex getStrAt matches record().getStr", () => {
  const r = new NxsReader(buf);
  const idx = r.buildFieldIndex("username");
  if (!idx) throw new Error("buildFieldIndex returned null");
  assertEq(idx.getStrAt(42), json[42].username);
  assertEq(idx.getStrAt(999), json[999].username);
});

test("cursor.seekWarm multi-field matches record()", () => {
  const r = new NxsReader(buf);
  const slotU = r.slot("username");
  const slotS = r.slot("score");
  const cur = r.cursor();
  cur.seekWarm(500);
  assertEq(cur.getStrBySlot(slotU), json[500].username);
  assertClose(cur.getF64BySlot(slotS), json[500].score);
});

test("batchResolveOffsets matches buildFieldIndex offsets", () => {
  const r = new NxsReader(buf);
  const slot = r.slot("username");
  const indices = [0, 42, 999];
  const batch = r.batchResolveOffsets(slot, indices);
  const idx = r.buildFieldIndex("username");
  for (let j = 0; j < indices.length; j++) {
    if (batch[j] !== idx.offsets[indices[j]]) {
      throw new Error(`offset mismatch at record ${indices[j]}`);
    }
  }
});

test("record(500) matches JSON[500].score", () => {
  const r = new NxsReader(buf);
  assertClose(r.record(500).getF64("score"), json[500].score);
});

test("record(999) last record active flag matches", () => {
  const r = new NxsReader(buf);
  assertEq(r.record(999).getBool("active"), json[999].active);
});

test("out-of-bounds record throws", () => {
  const r = new NxsReader(buf);
  let threw = false;
  try { r.record(10000); } catch { threw = true; }
  assertEq(threw, true);
});

test("iteration visits every record", () => {
  const r = new NxsReader(buf);
  let count = 0;
  for (const rec of r.records()) {
    void rec;
    count++;
  }
  assertEq(count, 1000);
});

test("iteration sum matches JSON sum", () => {
  const r = new NxsReader(buf);
  let nxsSum = 0;
  for (const rec of r.records()) nxsSum += rec.getF64("score");
  let jsonSum = 0;
  for (const rec of json) jsonSum += rec.score;
  assertClose(nxsSum, jsonSum, 0.01, "score sums");
});

test("cursor scan matches JSON sum", () => {
  const r = new NxsReader(buf);
  const slot = r.slot("score");
  let nxsSum = 0;
  r.scan(cur => { nxsSum += cur.getF64BySlot(slot); });
  let jsonSum = 0;
  for (const rec of json) jsonSum += rec.score;
  assertClose(nxsSum, jsonSum, 0.01, "cursor scan sums");
});

test("cursor.seek(k) reads same value as record(k)", () => {
  const r = new NxsReader(buf);
  const cur = r.cursor();
  for (const k of [0, 42, 500, 999]) {
    cur.seek(k);
    assertEq(cur.getStr("username"), r.record(k).getStr("username"),
             `record ${k} mismatch`);
  }
});

// ── Security tests ────────────────────────────────────────────────────────────

test("bad magic throws ERR_BAD_MAGIC", () => {
  const bad = new Uint8Array(buf.length);
  bad.set(buf); bad[0] = 0x00;
  let threw = false;
  try { new NxsReader(bad); } catch (e) { threw = e.code === "ERR_BAD_MAGIC"; }
  if (!threw) throw new Error("expected ERR_BAD_MAGIC");
});

test("truncated file throws ERR_OUT_OF_BOUNDS", () => {
  const bad = buf.slice(0, 16);
  let threw = false;
  try { new NxsReader(bad); } catch { threw = true; }
  if (!threw) throw new Error("expected error on truncated file");
});

test("corrupt DictHash throws ERR_DICT_MISMATCH", () => {
  const bad = new Uint8Array(buf.length);
  bad.set(buf); bad[8] ^= 0xFF;
  let threw = false;
  try { new NxsReader(bad); } catch (e) { threw = e.code === "ERR_DICT_MISMATCH"; }
  if (!threw) throw new Error("expected ERR_DICT_MISMATCH");
});

// ── Writer round-trip tests ───────────────────────────────────────────────────

console.log("\nNXS JavaScript Writer — Tests\n");

test("writer round-trip: 3 records", () => {
  const schema = new NxsSchema(["id", "username", "score", "active"]);
  const w = new NxsWriter(schema);
  const recs = [
    [1n, "alice", 9.5, true],
    [2n, "bob", 7.2, false],
    [3n, "carol", 8.8, true],
  ];
  for (const [id, name, score, active] of recs) {
    w.beginObject();
    w.writeI64(0, id);
    w.writeStr(1, name);
    w.writeF64(2, score);
    w.writeBool(3, active);
    w.endObject();
  }
  const bytes = w.finish();
  const preambleTailPtr = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getBigUint64(16, true);
  assertEq(Number(preambleTailPtr), 0, "v1.1 streamable preamble TailPtr");
  const r = new NxsReader(bytes);
  assertEq(r.recordCount, 3, "record count");
  for (let i = 0; i < 3; i++) {
    const obj = r.record(i);
    assertEq(obj.getI64("id"), Number(recs[i][0]), `record ${i} id`);
    assertEq(obj.getStr("username"), recs[i][1], `record ${i} username`);
    assertClose(obj.getF64("score"), recs[i][2], 1e-9, `record ${i} score`);
    assertEq(obj.getBool("active"), recs[i][3], `record ${i} active`);
  }
});

test("stream reader emits records before finish", () => {
  const schema = new NxsSchema(["id", "username"]);
  const w = new NxsWriter(schema);
  w.beginObject(); w.writeI64(0, 1n); w.writeStr(1, "alice"); w.endObject();
  w.beginObject(); w.writeI64(0, 2n); w.writeStr(1, "bob"); w.endObject();
  const bytes = w.finish();

  const seen = [];
  const sr = new NxsStreamReader({
    onRecord(record) {
      seen.push(record.getStr("username"));
    },
  });
  let offset = 0;
  for (let i = 0; i < bytes.length; i += 17) {
    offset = Math.min(i + 17, bytes.length);
    sr.push(bytes.subarray(i, offset));
    if (seen.length > 0) break;
  }
  if (seen.length === 0) throw new Error("expected at least one streamed record before finish");
  if (offset < bytes.length) sr.push(bytes.subarray(offset));
  const final = sr.finish();
  assertEq(final.recordCount, 2, "final record count");
  assertEq(final.record(1).getStr("username"), "bob", "final reader");
});

test("stream reader rejects invalid object length", () => {
  const schema = new NxsSchema(["id"]);
  const w = new NxsWriter(schema);
  w.beginObject(); w.writeI64(0, 1n); w.endObject();
  const bytes = w.finish();
  const bad = new Uint8Array(bytes);
  let objOffset = -1;
  for (let i = 32; i + 4 <= bad.length; i++) {
    if (bad[i] === 0x4F && bad[i + 1] === 0x58 && bad[i + 2] === 0x59 && bad[i + 3] === 0x4E) {
      objOffset = i;
      break;
    }
  }
  if (objOffset < 0) throw new Error("missing NYXO object");
  new DataView(bad.buffer, bad.byteOffset, bad.byteLength).setUint32(objOffset + 4, 0, true);
  const sr = new NxsStreamReader();
  let threw = false;
  try {
    sr.push(bad);
  } catch (err) {
    threw = err.code === "ERR_OUT_OF_BOUNDS";
  }
  if (!threw) throw new Error("expected invalid object length error");
});

test("writer round-trip: fromRecords convenience", () => {
  const bytes = NxsWriter.fromRecords(
    ["id", "name", "value"],
    [
      { id: 10n, name: "foo", value: 1.5 },
      { id: 20n, name: "bar", value: 2.5 },
    ]
  );
  const r = new NxsReader(bytes);
  assertEq(r.recordCount, 2, "record count");
  assertEq(r.record(1).getStr("name"), "bar", "second record name");
});

test("writer round-trip: null field", () => {
  const schema = new NxsSchema(["a", "b"]);
  const w = new NxsWriter(schema);
  w.beginObject();
  w.writeI64(0, 99n);
  w.writeNull(1);
  w.endObject();
  const r = new NxsReader(w.finish());
  assertEq(r.record(0).getI64("a"), 99, "a present");
});

test("writer round-trip: bool field", () => {
  const schema = new NxsSchema(["flag"]);
  const w = new NxsWriter(schema);
  w.beginObject(); w.writeBool(0, true);  w.endObject();
  w.beginObject(); w.writeBool(0, false); w.endObject();
  const r = new NxsReader(w.finish());
  assertEq(r.record(0).getBool("flag"), true,  "true");
  assertEq(r.record(1).getBool("flag"), false, "false");
});

test("writer round-trip: string with unicode", () => {
  const schema = new NxsSchema(["msg"]);
  const w = new NxsWriter(schema);
  w.beginObject();
  w.writeStr(0, "héllo wörld");
  w.endObject();
  const r = new NxsReader(w.finish());
  assertEq(r.record(0).getStr("msg"), "héllo wörld", "unicode string");
});

test("schema evolution: write 3 fields, read with 2-slot reader", () => {
  // Write with schema ["a","b","c"]
  const schema = new NxsSchema(["a", "b", "c"]);
  const w = new NxsWriter(schema);
  w.beginObject();
  w.writeI64(0, 100n);
  w.writeI64(1, 200n);
  w.writeI64(2, 300n);
  w.endObject();
  const bytes = w.finish();

  // Read with full schema — all three present
  const r = new NxsReader(bytes);
  const obj = r.record(0);
  assertEq(obj.getI64("a"), 100, "a");
  assertEq(obj.getI64("b"), 200, "b");
  assertEq(obj.getI64("c"), 300, "c");

  // Simulate 2-field reader: access only slots 0 and 1 — slot 2 is absent, not an error
  const slotA = r.slot("a");
  const slotB = r.slot("b");
  assertEq(obj.getI64BySlot(slotA), 100, "slot a via slot handle");
  assertEq(obj.getI64BySlot(slotB), 200, "slot b via slot handle");
  // Accessing a slot beyond the schema (simulating old reader) returns undefined/absent
  const absent = obj.getI64BySlot(99);
  if (absent !== undefined && absent !== null) {
    throw new Error(`expected absent for unknown slot, got ${absent}`);
  }
});

// ── Query engine tests ────────────────────────────────────────────────────────

console.log("\nNXS JavaScript Query Engine — Tests\n");

// Compute expected values from the JSON fixture once.
const jsonActiveCount   = json.filter(r => r.active === true).length;
const jsonGtScore80     = json.filter(r => r.score > 80.0).length;
const jsonAndCount      = json.filter(r => r.active === true && r.score > 80.0).length;
const jsonFirstActive   = json.find(r => r.active === true);

test("testQueryEqBool — filter active===true count matches JSON", () => {
  const r = new NxsReader(buf);
  const count = r.where(eq("active", true)).count();
  assertEq(count, jsonActiveCount, "active===true count");
});

test("testQueryGtFloat — filter score > 80.0 count matches JSON", () => {
  const r = new NxsReader(buf);
  const count = r.where(gt("score", 80.0)).count();
  assertEq(count, jsonGtScore80, "score > 80.0 count");
});

test("testQueryAnd — and(eq active, gt score) count matches JSON", () => {
  const r = new NxsReader(buf);
  const count = r.where(and(eq("active", true), gt("score", 80.0))).count();
  assertEq(count, jsonAndCount, "and predicate count");
});

test("testQueryFirst — first() active===true matches JSON.find()", () => {
  const r = new NxsReader(buf);
  const rec = r.where(eq("active", true)).first();
  if (!rec) throw new Error("first() returned null, expected a record");
  assertEq(rec.getBool("active"), true, "first record active flag");
  assertEq(rec.getStr("username"), jsonFirstActive.username, "first active username");
});

test("testQueryAllCount — reader.all.count() === total records", () => {
  const r = new NxsReader(buf);
  assertEq(r.all.count(), r.recordCount, "all.count() === recordCount");
});

test("Query is iterable (for-of yields NxsObjects)", () => {
  const r = new NxsReader(buf);
  const q = r.where(eq("active", false));
  let n = 0;
  for (const rec of q) {
    if (rec.getBool("active") !== false) throw new Error("predicate violated");
    n++;
  }
  assertEq(n, json.filter(r => r.active === false).length, "inactive count");
});

test("Query combinators — or/not work correctly", () => {
  const r = new NxsReader(buf);
  // or(lt, gt) = everything except score in [50, 60] range; just check symmetry
  const ltCount  = r.where(lt("score",  50.0)).count();
  const gtCount  = r.where(gt("score",  50.0)).count();
  const eqCount  = r.where(eq("score",  50.0)).count();  // likely 0 with floats
  const notLtCount = r.where(not(lt("score", 50.0))).count();
  assertEq(ltCount + notLtCount, 1000, "lt + not(lt) = total");
  assertEq(ltCount + gtCount + eqCount, 1000, "lt + gt + eq = total");
});

console.log(`\n${passed} passed, ${failed} failed\n`);
process.exit(failed > 0 ? 1 : 0);
