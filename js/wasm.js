// Optional WASM accelerator for NXS reducers.
// Works in both Node.js and browsers (ES modules).
//
// Usage (browser):
//   const wasm = await loadWasm("./wasm/nxs_reducers.wasm");
//   const buf  = new Uint8Array(await (await fetch("data.nxb")).arrayBuffer());
//   const r    = new NxsReader(buf);
//   r.useWasm(wasm);          // copies bytes into WASM memory
//   r.sumF64("score");
//
// Usage (Node.js):
//   import { loadWasm, readNxbIntoWasm } from "./wasm.js";
//   const wasm = await loadWasm();                     // default path resolved via import.meta.url
//   const buf  = readNxbIntoWasm(wasm, "data.nxb");    // zero-copy fast path
//   const r    = new NxsReader(buf);
//   r.useWasm(wasm);                                    // no-op (already resident)
//   r.sumF64("score");

export class NxsWasm {
  constructor(instance, memory, dataBase) {
    this.instance = instance;
    this.memory = memory;
    this.dataBase = dataBase;
    this.fns = instance.exports;
    this.bytes = new Uint8Array(memory.buffer);
    this.loadedBytes = 0;
  }

  allocBuffer(n) {
    this._ensureCapacity(n);
    this.loadedBytes = n;
    return new Uint8Array(this.memory.buffer, this.dataBase, n);
  }

  _ensureCapacity(n) {
    const end = this.dataBase + n;
    const have = this.memory.buffer.byteLength;
    if (end > have) {
      const extraPages = Math.ceil((end - have) / 65536);
      this.memory.grow(extraPages);
    }
    this.bytes = new Uint8Array(this.memory.buffer);
  }

  loadPayload(nxbBytes) {
    if (this._sharesMemory(nxbBytes)) {
      this.loadedBytes = nxbBytes.byteLength;
      return;
    }
    this._ensureCapacity(nxbBytes.byteLength);
    this.bytes.set(nxbBytes, this.dataBase);
    this.loadedBytes = nxbBytes.byteLength;
  }

  _sharesMemory(nxbBytes) {
    return nxbBytes.buffer === this.memory.buffer
        && nxbBytes.byteOffset === this.dataBase;
  }
}

/**
 * Load the WASM module. Works in Node.js or browsers.
 *
 * @param {string | URL} [wasmUrl] — URL or path to nxs_reducers.wasm. When
 *   running in Node with no argument, resolves relative to this module.
 *   When running in a browser, defaults to "./wasm/nxs_reducers.wasm".
 * @param {object} [opts]
 * @param {number} [opts.initialPages=1024] — initial memory pages (64 KB each)
 */
export async function loadWasm(wasmUrl, opts = {}) {
  const initialPages = opts.initialPages ?? 1024;
  const memory = new WebAssembly.Memory({ initial: initialPages, maximum: 65536 });

  const wasmBytes = await _fetchWasmBytes(wasmUrl);
  const mod = await WebAssembly.instantiate(wasmBytes, { env: { memory } });

  return new NxsWasm(mod.instance, memory, 65536);
}

async function _fetchWasmBytes(wasmUrl) {
  const isNode =
    typeof process !== "undefined" &&
    process.versions != null &&
    process.versions.node != null;

  if (isNode) {
    const { readFileSync } = await import("node:fs");
    const { fileURLToPath } = await import("node:url");
    const { dirname, join } = await import("node:path");
    let path = wasmUrl;
    if (path === undefined) {
      const here = dirname(fileURLToPath(import.meta.url));
      path = join(here, "wasm/nxs_reducers.wasm");
    } else if (path instanceof URL) {
      path = fileURLToPath(path);
    } else if (typeof path === "string" && path.startsWith("file:")) {
      path = fileURLToPath(path);
    }
    return readFileSync(path);
  }

  const url =
    wasmUrl === undefined
      ? "./wasm/nxs_reducers.wasm"
      : wasmUrl instanceof URL
        ? wasmUrl.href
        : wasmUrl;
  const res = await fetch(url);
  if (!res.ok) throw new Error(`failed to load wasm: ${res.status}`);
  const buf = await res.arrayBuffer();
  return new Uint8Array(buf);
}

/**
 * WASM-accelerated WAL span encoder.
 *
 * Encodes the canonical 10-field span struct into a raw NYXO record using
 * the `encode_span` function compiled into nxs_reducers.wasm.  No JS struct
 * packing, no BigInt arithmetic, no per-call allocation — everything runs in
 * native WASM code.
 *
 * Layout of the input region (72 bytes at `_fieldsPtr`):
 *   [  0.. 7]  i64  trace_id_hi
 *   [  8..15]  i64  trace_id_lo
 *   [ 16..23]  i64  span_id
 *   [ 24..31]  i64  parent_span_id
 *   [ 32..35]  u32  name_ptr   (abs WASM addr)
 *   [ 36..39]  u32  name_len
 *   [ 40..43]  u32  service_ptr
 *   [ 44..47]  u32  service_len
 *   [ 48..55]  i64  start_time_ns
 *   [ 56..63]  i64  duration_ns
 *   [ 64..71]  i64  status_code
 *
 * Usage:
 *   const wasm = await loadWasm();
 *   const enc  = new WasmSpanWriter(wasm, 256);  // reserve 256 B for output
 *   const out  = enc.encode({ trace_id_hi: 1n, ... });  // Uint8Array view
 */
export class WasmSpanWriter {
  /**
   * @param {NxsWasm} wasm
   * @param {number} [maxRecordBytes=256] — upper bound on output record size
   */
  constructor(wasm, maxRecordBytes = 256) {
    this._wasm = wasm;
    this._encode_span = wasm.fns.encode_span;
    if (!this._encode_span) throw new Error("encode_span not found in WASM exports");

    // Reserve a contiguous region in WASM memory:
    //   [fields (72 B)] [string pool (maxRecordBytes B)] [output (maxRecordBytes B)]
    const FIELDS_SIZE  = 72;
    const base = wasm.dataBase + (wasm.loadedBytes || 0);

    // Align to 8
    const aligned = (base + 7) & ~7;
    wasm._ensureCapacity(aligned - wasm.dataBase + FIELDS_SIZE + maxRecordBytes * 2 + 512);

    this._fieldsPtr  = aligned;
    this._strPoolPtr = aligned + FIELDS_SIZE;
    this._outPtr     = aligned + FIELDS_SIZE + maxRecordBytes;
    this._maxRecord  = maxRecordBytes;

    this._mem  = wasm.bytes;         // kept in sync via _refreshView()
    this._view = new DataView(wasm.memory.buffer);
    this._enc  = new TextEncoder();
  }

  _refreshView() {
    if (this._view.buffer !== this._wasm.memory.buffer) {
      this._mem  = this._wasm.bytes;
      this._view = new DataView(this._wasm.memory.buffer);
    }
  }

  /**
   * Encode one span.  Returns a Uint8Array view into WASM memory valid until
   * the next encode() call (zero-copy for WAL append path).
   *
   * @param {object} sp — span fields (i64 fields as BigInt or safe integer)
   * @returns {Uint8Array}
   */
  encode(sp) {
    this._refreshView();
    const v = this._view;
    const p = this._fieldsPtr;

    const writeI64 = (off, val) => {
      if (typeof val === "bigint") {
        const u = BigInt.asUintN(64, val);
        v.setUint32(p + off,     Number(u & 0xFFFFFFFFn), true);
        v.setUint32(p + off + 4, Number(u >> 32n),        true);
      } else {
        v.setUint32(p + off,     val >>> 0,                             true);
        v.setUint32(p + off + 4, Math.floor(val / 4294967296) >>> 0,   true);
      }
    };

    writeI64( 0, sp.trace_id_hi);
    writeI64( 8, sp.trace_id_lo);
    writeI64(16, sp.span_id);
    writeI64(24, sp.parent_span_id);

    // Encode strings into the string pool
    let strOff = 0;
    const nameBytes = this._enc.encodeInto(sp.name,    this._mem.subarray(this._strPoolPtr + strOff)).written;
    const namePtr   = this._strPoolPtr + strOff; strOff += nameBytes;
    const svcBytes  = this._enc.encodeInto(sp.service, this._mem.subarray(this._strPoolPtr + strOff)).written;
    const svcPtr    = this._strPoolPtr + strOff;

    v.setUint32(p + 32, namePtr,   true);
    v.setUint32(p + 36, nameBytes, true);
    v.setUint32(p + 40, svcPtr,    true);
    v.setUint32(p + 44, svcBytes,  true);

    writeI64(48, sp.start_time_ns);
    writeI64(56, sp.duration_ns);
    writeI64(64, sp.status_code);

    const written = this._encode_span(this._outPtr, p);
    return this._mem.subarray(this._outPtr, this._outPtr + written);
  }
}

/**
 * Node-only convenience: open an .nxb file and read it directly into WASM
 * memory. Returns a Uint8Array view suitable for `new NxsReader(...)`.
 *
 * Throws in browsers (use `fetch` + `arrayBuffer` + `new NxsReader` there).
 */
export async function readNxbIntoWasm(wasm, path) {
  if (typeof process === "undefined" || !process.versions?.node) {
    throw new Error("readNxbIntoWasm is Node-only; use fetch() in browsers");
  }
  const { openSync, fstatSync, readSync, closeSync } = await import("node:fs");
  const fd = openSync(path, "r");
  try {
    const size = fstatSync(fd).size;
    const buf = wasm.allocBuffer(size);
    readSync(fd, buf, 0, size, 0);
    return buf;
  } finally {
    closeSync(fd);
  }
}
