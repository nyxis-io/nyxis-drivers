// Parity tests for the WASM reducers vs the pure-JS implementation.
// Run: node test_wasm.js [fixtures_dir]

import { readFileSync } from "node:fs";
import { join } from "node:path";
import { NxsReader } from "./nxs.js";
import { loadWasm, readNxbIntoWasm } from "./wasm.js";

const fixtureDir = process.argv[2] ?? "./fixtures";
const nxb = readFileSync(join(fixtureDir, "records_1000.nxb"));
const json = JSON.parse(readFileSync(join(fixtureDir, "records_1000.json"), "utf8"));

let passed = 0, failed = 0;
function test(name, fn) {
  try { fn(); console.log(`  ✓ ${name}`); passed++; }
  catch (e) { console.log(`  ✗ ${name}\n      ${e.message}`); failed++; }
}
const close = (a, b, eps = 0.001) => Math.abs(a - b) <= eps;

console.log("\nWASM reducer parity\n");

const wasm = await loadWasm();

const jsReader = new NxsReader(nxb);
const wasmReader = new NxsReader(nxb);
wasmReader.useWasm(wasm);

const expectedSum = json.reduce((a, r) => a + r.score, 0);
let expectedMin = Infinity, expectedMax = -Infinity;
for (const r of json) {
  if (r.score < expectedMin) expectedMin = r.score;
  if (r.score > expectedMax) expectedMax = r.score;
}
const expectedAgeSum = json.reduce((a, r) => a + r.age, 0);

test("WASM loads", () => {
  if (!wasm.fns.sum_f64) throw new Error("no sum_f64 export");
});

test("sumF64 WASM == JS", () => {
  const js = jsReader.sumF64("score");
  const w = wasmReader.sumF64("score");
  if (!close(js, w)) throw new Error(`js=${js} wasm=${w}`);
});

test("sumF64 WASM matches JSON sum", () => {
  if (!close(wasmReader.sumF64("score"), expectedSum))
    throw new Error("mismatch");
});

test("minF64 WASM matches", () => {
  if (!close(wasmReader.minF64("score"), expectedMin))
    throw new Error(`got ${wasmReader.minF64("score")}, want ${expectedMin}`);
});

test("maxF64 WASM matches", () => {
  if (!close(wasmReader.maxF64("score"), expectedMax))
    throw new Error(`got ${wasmReader.maxF64("score")}, want ${expectedMax}`);
});

test("sumI64 WASM matches", () => {
  const w = wasmReader.sumI64("age");
  if (w !== expectedAgeSum) throw new Error(`got ${w}, want ${expectedAgeSum}`);
});

test("WASM fallback still works when not attached", () => {
  const r = new NxsReader(nxb);
  if (!close(r.sumF64("score"), expectedSum)) throw new Error("fallback mismatch");
});

test("buildFieldIndex WASM matches JS", () => {
  if (!wasm.fns.build_field_index) throw new Error("no build_field_index export");
  const jsIdx = jsReader.buildFieldIndex("username");
  const wasmIdx = wasmReader.buildFieldIndex("username");
  if (jsIdx.getStrAt(42) !== wasmIdx.getStrAt(42)) {
    throw new Error(`42: js=${jsIdx.getStrAt(42)} wasm=${wasmIdx.getStrAt(42)}`);
  }
  if (jsIdx.offsets[500] !== wasmIdx.offsets[500]) {
    throw new Error(`offset[500] js=${jsIdx.offsets[500]} wasm=${wasmIdx.offsets[500]}`);
  }
});

test("batchResolveOffsets WASM matches JS", () => {
  if (!wasm.fns.batch_resolve_offsets) throw new Error("no batch_resolve_offsets export");
  const slot = jsReader.slot("username");
  const indices = [1, 42, 999];
  const jsOff = jsReader.batchResolveOffsets(slot, indices);
  const wasmOff = wasmReader.batchResolveOffsets(slot, indices);
  for (let j = 0; j < indices.length; j++) {
    if (jsOff[j] !== wasmOff[j]) throw new Error(`j=${j}: js=${jsOff[j]} wasm=${wasmOff[j]}`);
  }
});

// async test — execute outside the `test()` harness which assumes sync fns
{
  const wasm2 = wasm;
  try {
    const buf = await readNxbIntoWasm(wasm2, join(fixtureDir, "records_1000.nxb"));
    const r = new NxsReader(buf);
    if (buf.buffer !== wasm2.memory.buffer) throw new Error("not WASM-backed");
    if (buf.byteOffset !== wasm2.dataBase) throw new Error("not at dataBase");
    r.useWasm(wasm2);
    if (!close(r.sumF64("score"), expectedSum)) throw new Error("sum mismatch");
    console.log("  ✓ zero-copy: readNxbIntoWasm + useWasm skips payload copy");
    passed++;
  } catch (e) {
    console.log(`  ✗ zero-copy: readNxbIntoWasm + useWasm skips payload copy\n      ${e.message}`);
    failed++;
  }
}

console.log(`\n${passed} passed, ${failed} failed\n`);
process.exit(failed > 0 ? 1 : 0);
