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
const expectedMin = Math.min(...json.map(r => r.score));
const expectedMax = Math.max(...json.map(r => r.score));
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
