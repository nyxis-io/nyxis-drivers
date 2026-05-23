// Prefetch unit tests — run: node test/prefetch.test.js

import {
  coalescePageIndices,
  PageCache,
  InFlightMap,
  DEFAULT_PAGE_SIZE,
} from "../prefetch.js";
import { NxsReader } from "../nxs.js";
import { NxsSchema, NxsWriter } from "../nxs_writer.js";

let passed = 0;
let failed = 0;

function test(name, fn) {
  try {
    fn();
    console.log(`  ✓ ${name}`);
    passed++;
  } catch (e) {
    console.log(`  ✗ ${name}\n      ${e.message}`);
    failed++;
  }
}

async function testAsync(name, fn) {
  try {
    await fn();
    console.log(`  ✓ ${name}`);
    passed++;
  } catch (e) {
    console.log(`  ✗ ${name}\n      ${e.message}`);
    failed++;
  }
}

function buildRecords(n) {
  const schema = new NxsSchema(["id", "username", "score", "active"]);
  const w = new NxsWriter(schema);
  for (let i = 0; i < n; i++) {
    w.beginObject();
    w.writeI64(0, i);
    w.writeStr(1, `user_${i}`);
    w.writeF64(2, i * 0.25);
    w.writeBool(3, i % 2 === 0);
    w.endObject();
  }
  return w.finish();
}

console.log("\nNXS Prefetch — Tests\n");

test("coalescePageIndices [3,4,6,7,12] gap=1 → 3 ranges", () => {
  const r = coalescePageIndices([3, 4, 6, 7, 12], 1, DEFAULT_PAGE_SIZE);
  if (r.length !== 3) throw new Error(`expected 3 ranges, got ${r.length}`);
  if (r[0].pageStart !== 3 || r[0].pageEnd !== 4) throw new Error("range 0");
  if (r[1].pageStart !== 6 || r[1].pageEnd !== 7) throw new Error("range 1");
  if (r[2].pageStart !== 12 || r[2].pageEnd !== 12) throw new Error("range 2");
});

test("PageCache LRU evicts at max_pages", () => {
  const c = new PageCache(2, 64);
  c.set(0, new Uint8Array(64));
  c.set(1, new Uint8Array(64));
  c.get(0);
  c.set(2, new Uint8Array(64));
  if (c.has(1)) throw new Error("page 1 should be evicted");
  if (!c.has(0) || !c.has(2)) throw new Error("pages 0 and 2 should remain");
});

await testAsync("InFlightMap dedupes concurrent page loads", async () => {
  const m = new InFlightMap();
  let fetches = 0;
  const p = new Promise((resolve) => {
    setTimeout(() => {
      fetches++;
      resolve(new Uint8Array(8));
    }, 10);
  });
  m.set(3, p);
  const a = m.get(3);
  const b = m.get(3);
  await Promise.all([a, b]);
  if (fetches !== 1) throw new Error(`expected 1 fetch, got ${fetches}`);
});

await testAsync("prefetch_viewport uses ≤3 coalesced fetchRange calls for 50 records", async () => {
  const buf = buildRecords(60);
  const ranges = [];
  const reader = new NxsReader(buf, {
    maxPages: 64,
    coalesceGapPages: 1,
    fetchRange: async (start, len) => {
      ranges.push({ start, len });
      return buf.subarray(start, start + len);
    },
  });
  await reader.prefetch_viewport(0, 49);
  if (ranges.length > 3) {
    throw new Error(`expected ≤3 fetches, got ${ranges.length}: ${JSON.stringify(ranges)}`);
  }
  const stats = reader.cache_stats();
  if (stats.fetches_issued !== ranges.length) {
    throw new Error("fetches_issued mismatch");
  }
});

await testAsync("prefetch_viewport_basic — records readable after prefetch", async () => {
  const buf = buildRecords(55);
  const reader = new NxsReader(buf, { fetchRange: (s, l) => Promise.resolve(buf.subarray(s, s + l)) });
  await reader.prefetch_viewport(0, 49);
  const obj = reader.record(49);
  if (obj.getI64("id") !== 49) throw new Error("record 49 id mismatch");
});

await testAsync("prefetch_memory_eviction", async () => {
  const buf = buildRecords(20);
  const reader = new NxsReader(buf, { maxPages: 2, pageSize: 256, coalesceGapPages: 0 });
  await reader.prefetch_viewport(0, 0);
  await reader.prefetch_viewport(19, 19);
  const stats = reader.cache_stats();
  if (stats.pages_cached > 2) throw new Error(`cache grew past max: ${stats.pages_cached}`);
});

await testAsync("prefetch_deduplication — parallel viewport same page", async () => {
  const buf = buildRecords(10);
  let calls = 0;
  const reader = new NxsReader(buf, {
    maxPages: 8,
    fetchRange: async (s, l) => {
      calls++;
      await new Promise((r) => setTimeout(r, 5));
      return buf.subarray(s, s + l);
    },
  });
  await Promise.all([
    reader.prefetch_viewport(0, 4),
    reader.prefetch_viewport(0, 4),
  ]);
  if (calls > 3) throw new Error(`too many fetches: ${calls}`);
});

await testAsync("NxsReader.open requires fetch", async () => {
  const orig = globalThis.fetch;
  globalThis.fetch = undefined;
  try {
    let threw = false;
    await NxsReader.open("http://example.com/x.nxb").catch(() => {
      threw = true;
    });
    if (!threw) throw new Error("expected open to fail without fetch");
  } finally {
    globalThis.fetch = orig;
  }
});

console.log(`\n${passed} passed, ${failed} failed\n`);
process.exit(failed > 0 ? 1 : 0);
