# NXS — JavaScript

Zero-copy `.nxb` reader for Node.js and the browser. Single ES module file, no dependencies, no build step.

## Requirements

Node.js 18+ or any modern browser. No npm install required.

## Read a file

```js
import { NxsReader } from "./nxs.js";

// Node.js
import { readFileSync } from "node:fs";
const reader = new NxsReader(readFileSync("data.nxb"));

// Browser
const reader = new NxsReader(new Uint8Array(await fetch("data.nxb").then(r => r.arrayBuffer())));

console.log(reader.recordCount);       // instant — read from tail-index, no parse pass
const obj = reader.record(42);         // O(1) seek
console.log(obj.getStr("username"));
console.log(obj.getF64("score"));
console.log(obj.getBool("active"));
```

## Columnar scan

```js
const sum = reader.sumF64("score");
const min = reader.minF64("score");
const max = reader.maxF64("score");
```

## Slot handles (hot path)

Resolve a key name to a slot index once, reuse it across every record:

```js
const slot = reader.slot("score");
for (let i = 0; i < reader.recordCount; i++) {
    const v = reader.record(i).getF64BySlot(slot);
}
```

## Optional: WASM-accelerated reducers

```js
import { loadWasm } from "./wasm.js";

const wasm = await loadWasm("./wasm/nxs_reducers.wasm");
reader.useWasm(wasm);
const sum = reader.sumF64("score");   // ~1.3× faster at 1M records
```

Build the WASM module from source (in the core repo):

```bash
bash nyxis/bench/wasm/build.sh
```

## WAL ingestion (high-throughput span encoding)

`WasmSpanWriter` encodes a fixed 9-field span directly into WASM memory with no JS heap allocation:

```js
import { loadWasm, WasmSpanWriter } from "./wasm.js";

const wasm = await loadWasm("./wasm/nxs_reducers.wasm");
const writer = new WasmSpanWriter(wasm);

writer.encode(traceIdHi, traceIdLo, spanId, parentSpanId,
              name, service, startTimeNs, durationNs, statusCode);
// returns a zero-copy Uint8Array view of the encoded NYXO record (~280 ns/span)
```

See the WAL demo in the core repo: `nyxis/demo/wal.html` (served at `/demo/wal.html`).

## Web Workers / SharedArrayBuffer

```js
// main thread — use nyxis docker compose (COOP/COEP headers); WASM binary in nyxis/bench/wasm/
const wasm = await loadWasm("/bench/wasm/nxs_reducers.wasm");
const buf = wasm.allocBuffer(nxbBytes.length);
buf.set(nxbBytes);   // copy once into WASM memory

for (let i = 0; i < 4; i++) {
    new Worker("./nxs_worker.js", { type: "module" })
        .postMessage({ buffer: wasm.memory.buffer, size: buf.length });
}
// Workers share the buffer — 0 bytes copied between threads
```

## Browser demos

Demos and benchmarks live in **`nyxis-io/nyxis`** (`demo/`, `bench/`), not in this driver package.

```bash
cd nyxis && docker compose up
```

| Demo | URL | Description |
| :--- | :--- | :--- |
| Benchmark | http://localhost:8000/bench/bench.html | NXS vs JSON vs CSV, up to 14M records |
| Ticker | http://localhost:8000/demo/ticker.html | 60 FPS in-place byte patch vs full JSON re-parse |
| Workers | http://localhost:8000/demo/workers.html | 4 workers, SharedArrayBuffer, 0 bytes copied |
| Explorer | http://localhost:8000/demo/explorer.html | 10M-line log explorer with virtual scroll |
| WAL | http://localhost:8000/demo/wal.html | WAL ingestion — 5 encoders — live cross-language chart |

## Write a file

```js
import { NxsSchema, NxsWriter } from "./nxs_writer.js";

const schema = new NxsSchema(["id", "username", "score", "active"]);
const w = new NxsWriter(schema);

w.beginObject();
w.writeI64(0, 42n);
w.writeStr(1, "alice");
w.writeF64(2, 9.5);
w.writeBool(3, true);
w.endObject();

const bytes = w.finish();   // Uint8Array

// Convenience: write from an array of objects
const bytes2 = NxsWriter.fromRecords(
    ["id", "username", "score"],
    [{ id: 1n, username: "bob", score: 8.2 }]
);
```

## Tests

```bash
node test.js
```

## Files

| File | Purpose |
| :--- | :--- |
| `nxs.js` | Pure-JS reader (Node + browser) |
| `nxs_writer.js` | Pure-JS writer (Node + browser) |
| `wasm.js` | WASM loader and zero-copy Node helper |
| `nxs_worker.js` | Web Worker that runs reducers on a shared buffer |
| `json_worker.js` | JSON baseline worker for benchmark comparison |
| `wasm/nxs_reducers.c` | C source for the WASM reducer module |
| `wasm/build.sh` | Compiles `nxs_reducers.c` → `nxs_reducers.wasm` via Emscripten |

## Query engine

```js
import { NxsReader, eq, gt, lt, and, or, not } from './nxs.js';

const r = new NxsReader(buffer);

// Count matching records
const n = r.where(and(eq("active", true), gt("score", 80.0))).count();

// Iterate — yields NxsObject instances
for (const obj of r.where(eq("active", true))) {
  console.log(obj.getStr("username"));
}

// First match or null
const first = r.where(gt("score", 99.0)).first();

// All records
for (const obj of r.all) { ... }
```

### Predicate factories

| Factory | Matches |
|---------|---------|
| `eq(key, value)` | equality — boolean, number, string, bigint |
| `gt(key, v)` / `lt(key, v)` | numeric > / < |
| `gte(key, v)` / `lte(key, v)` | numeric >= / <= |
| `and(...preds)` / `or(...preds)` / `not(pred)` | combinators |

`Query` implements `[Symbol.iterator]` — use with `for…of` or spread.
Getter type is resolved once at factory call time, not per record.

---

For the format specification see [`SPEC.md`](../SPEC.md). For cross-language examples see [`GETTING_STARTED.md`](../GETTING_STARTED.md).
