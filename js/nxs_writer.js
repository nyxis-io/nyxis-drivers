// NXS Writer — direct-to-buffer .nxb emitter for JavaScript.
//
// Mirrors the Rust NxsWriter API exactly:
//   NxsSchema  — precompile keys once; share across many NxsWriter instances.
//   NxsWriter  — slot-based hot path; no per-field Map lookups.
//
// Usage:
//   import { NxsSchema, NxsWriter } from "./nxs_writer.js";
//
//   const schema = new NxsSchema(["id", "username", "score"]);
//   const w = new NxsWriter(schema);
//   w.beginObject();
//   w.writeI64(0, 42n);        // slot index, BigInt for i64
//   w.writeStr(1, "alice");
//   w.writeF64(2, 9.5);
//   w.endObject();
//   const bytes = w.finish(); // Uint8Array

const MAGIC_FILE   = 0x4E595842; // NYXB
const MAGIC_OBJ    = 0x4E59584F; // NYXO
const MAGIC_LIST   = 0x4E59584C; // NYXL
const MAGIC_FOOTER = 0x2153584E; // NXS!
const VERSION      = 0x0101;
const FLAG_SCHEMA_EMBEDDED = 0x0002;

// ── MurmurHash3-64 ───────────────────────────────────────────────────────────

function murmur3_64(bytes, offset, length) {
  const C1 = 0xFF51AFD7ED558CCDn;
  const C2 = 0xC4CEB9FE1A85EC53n;
  const MASK = 0xFFFFFFFFFFFFFFFFn;
  let h = 0x93681D6255313A99n;
  const end = offset + length;
  for (let p = offset; p < end; p += 8) {
    let k = 0n;
    for (let i = 0; i < 8 && p + i < end; i++) {
      k |= BigInt(bytes[p + i]) << BigInt(i * 8);
    }
    k = (k * C1) & MASK;
    k ^= k >> 33n;
    h ^= k;
    h = (h * C2) & MASK;
    h ^= h >> 33n;
  }
  h ^= BigInt(length);
  h ^= h >> 33n;
  h = (h * C1) & MASK;
  h ^= h >> 33n;
  return h;
}

// ── Scratch buffer for f64 LE encoding ───────────────────────────────────────

const _scratchBuf = new ArrayBuffer(8);
const _scratchU8  = new Uint8Array(_scratchBuf);
const _scratchF64 = new Float64Array(_scratchBuf);

function encodeF64LE(v) {
  _scratchF64[0] = v;
  return _scratchU8.slice(0, 8);
}

// ── Schema ───────────────────────────────────────────────────────────────────

export class NxsSchema {
  /**
   * @param {string[]} keys
   */
  constructor(keys) {
    this.keys = keys;
    // LEB128 bitmask byte count for keys.length bits.
    // Same formula as Rust: integer (len + 6) / 7, minimum 1.
    // Math.floor mirrors Rust integer division exactly.
    this.bitmaskBytes = Math.max(1, Math.floor((keys.length + 6) / 7));
  }

  get length() { return this.keys.length; }
}

// ── Writer ───────────────────────────────────────────────────────────────────

// Sigil constants
const SIGIL_STR    = 0x22; // '"' — string / var-length
const SIGIL_I64    = 0x69; // 'i'
const SIGIL_F64    = 0x64; // 'd'
const SIGIL_BOOL   = 0x62; // 'b'
const SIGIL_NULL   = 0x6E; // 'n'
const SIGIL_BINARY = 0x42; // 'B'

export class NxsWriter {
  /**
   * @param {NxsSchema} schema
   */
  constructor(schema) {
    this.schema = schema;
    this._buf = new Uint8Array(4096);  // pre-allocated, grows by doubling
    this._view = new DataView(this._buf.buffer);
    this._len = 0;
    this._frames = [];       // stack of open object frames
    this._recordOffsets = []; // record start positions in data sector
    // Sigil per slot: default str/var-length; updated on each typed write
    this._slotSigils = new Uint8Array(schema.length).fill(SIGIL_STR);
  }

  // ── Buffer growth ────────────────────────────────────────────────────────

  _grow(need) {
    if (this._len + need <= this._buf.length) return;
    let newSize = this._buf.length * 2;
    while (this._len + need > newSize) newSize *= 2;
    const newBuf = new Uint8Array(newSize);
    newBuf.set(this._buf.subarray(0, this._len));
    this._buf = newBuf;
    this._view = new DataView(this._buf.buffer);
  }

  // ── Frame management ────────────────────────────────────────────────────

  beginObject() {
    const schema = this.schema;

    // Track top-level record offsets for the tail-index
    if (this._frames.length === 0) {
      this._recordOffsets.push(this._len);
    }

    const start = this._len;

    // Build initial bitmask with LEB128 continuation bits pre-set
    const bitmask = new Uint8Array(schema.bitmaskBytes);
    for (let i = 0; i < schema.bitmaskBytes - 1; i++) {
      bitmask[i] = 0x80;
    }
    // last byte has no continuation bit

    this._frames.push({
      start,
      bitmask,
      offsetTable: [],    // relative offsets from object start, in write order
      slotOffsets: [],    // [{slot, bufOff}] for sort path
      lastSlot: -1,
      needsSort: false,
    });

    // Write placeholder Magic (4) + Length (4) = 8 bytes
    this._writeU32(MAGIC_OBJ);
    this._writeU32(0); // length placeholder — back-patched in endObject

    // Reserve bitmask space
    this._writeBytes(bitmask);

    // Reserve offset-table space: one u16 per schema key (upper bound)
    const otReserve = new Uint8Array(schema.length * 2);
    this._writeBytes(otReserve);

    // Align data area to 8 from the object start
    while ((this._len - start) % 8 !== 0) {
      this._writeByte(0);
    }
  }

  endObject() {
    const frame = this._frames.pop();
    if (!frame) throw new Error("endObject without beginObject");

    // With a single contiguous buffer, back-patching is direct — no merge needed.
    const totalData = this._buf;

    const totalLen = this._len - frame.start;

    // Back-patch Length at frame.start + 4
    const lenOff = frame.start + 4;
    totalData[lenOff]     = totalLen & 0xFF;
    totalData[lenOff + 1] = (totalLen >>> 8) & 0xFF;
    totalData[lenOff + 2] = (totalLen >>> 16) & 0xFF;
    totalData[lenOff + 3] = (totalLen >>> 24) & 0xFF;

    // Back-patch bitmask at frame.start + 8
    const bmOff = frame.start + 8;
    for (let i = 0; i < frame.bitmask.length; i++) {
      totalData[bmOff + i] = frame.bitmask[i];
    }

    // Back-patch offset table starting at frame.start + 8 + bitmask.length
    const otStart = bmOff + frame.bitmask.length;
    const presentCount = frame.offsetTable.length;

    if (!frame.needsSort) {
      // Fast path: fields in slot order
      for (let i = 0; i < presentCount; i++) {
        const rel = frame.offsetTable[i];
        totalData[otStart + i * 2]     = rel & 0xFF;
        totalData[otStart + i * 2 + 1] = (rel >>> 8) & 0xFF;
      }
    } else {
      // Slow path: sort by slot, write offsets in slot order
      const pairs = frame.slotOffsets.slice().sort((a, b) => a.slot - b.slot);
      for (let i = 0; i < pairs.length; i++) {
        const rel = pairs[i].bufOff - frame.start;
        totalData[otStart + i * 2]     = rel & 0xFF;
        totalData[otStart + i * 2 + 1] = (rel >>> 8) & 0xFF;
      }
    }

    // Zero unused offset-table slots (deterministic output)
    const usedBytes = presentCount * 2;
    const reservedBytes = this.schema.length * 2;
    for (let i = usedBytes; i < reservedBytes; i++) {
      totalData[otStart + i] = 0;
    }
  }

  /**
   * Finish writing and return the complete .nxb file as a Uint8Array.
   */
  finish() {
    if (this._frames.length !== 0) throw new Error("unclosed objects");

    const schemaBytes = buildSchema(this.schema.keys, this._slotSigils);
    const dictHash    = murmur3_64(schemaBytes, 0, schemaBytes.length);

    // data_start_abs = 32 (preamble) + schemaBytes.length
    const dataStartAbs = 32 + schemaBytes.length;

    // Materialise data sector
    const dataSector = this._materializeAll();

    const tailPtr = dataStartAbs + dataSector.length;
    const tail    = buildTailIndexRecords(dataStartAbs, this._recordOffsets, tailPtr);

    const total = 32 + schemaBytes.length + dataSector.length + tail.length;
    const out = new Uint8Array(total);
    let p = 0;

    // Preamble (32 bytes)
    out[p++] = MAGIC_FILE & 0xFF;
    out[p++] = (MAGIC_FILE >>> 8) & 0xFF;
    out[p++] = (MAGIC_FILE >>> 16) & 0xFF;
    out[p++] = (MAGIC_FILE >>> 24) & 0xFF;

    out[p++] = VERSION & 0xFF;
    out[p++] = (VERSION >>> 8) & 0xFF;

    out[p++] = FLAG_SCHEMA_EMBEDDED & 0xFF;
    out[p++] = (FLAG_SCHEMA_EMBEDDED >>> 8) & 0xFF;

    // DictHash — 8 bytes little-endian BigInt (one-time, not hot path)
    for (let i = 0; i < 8; i++) {
      out[p++] = Number((dictHash >> BigInt(i * 8)) & 0xFFn);
    }

    // TailPtr=0 marks a streamable v1.1 file; the final tail pointer lives in the footer.
    {
      for (let i = 0; i < 8; i++) out[p++] = 0;
    }

    // Reserved 8 bytes
    p += 8;

    // Schema
    out.set(schemaBytes, p); p += schemaBytes.length;

    // Data sector
    out.set(dataSector, p); p += dataSector.length;

    // Tail-index
    out.set(tail, p);

    return out;
  }

  // ── Typed write methods ──────────────────────────────────────────────────

  writeI64(slot, v) {
    this._slotSigils[slot] = SIGIL_I64;
    this._markSlot(slot);
    let lo, hi;
    if (typeof v === "bigint") {
      const u = BigInt.asUintN(64, v);
      lo = Number(u & 0xFFFFFFFFn);
      hi = Number(u >> 32n);
    } else {
      // number path: handle signed 32-bit split correctly
      lo = v >>> 0;
      hi = Math.floor(v / 4294967296) >>> 0;
    }
    this._writeI64Parts(lo, hi);
  }

  writeF64(slot, v) {
    this._slotSigils[slot] = SIGIL_F64;
    this._markSlot(slot);
    this._writeBytes(encodeF64LE(v));
  }

  writeBool(slot, v) {
    this._slotSigils[slot] = SIGIL_BOOL;
    this._markSlot(slot);
    this._grow(8);
    this._buf[this._len] = v ? 0x01 : 0x00;
    this._buf.fill(0, this._len + 1, this._len + 8);
    this._len += 8;
  }

  writeTime(slot, unixNs) {
    this.writeI64(slot, unixNs);
  }

  writeNull(slot) {
    this._slotSigils[slot] = SIGIL_NULL;
    this._markSlot(slot);
    this._grow(8);
    this._buf.fill(0, this._len, this._len + 8);
    this._len += 8;
  }

  writeStr(slot, v) {
    this._slotSigils[slot] = SIGIL_STR;
    this._markSlot(slot);
    const bytes = encodeUtf8(v);
    const len = bytes.length;
    const used = (4 + len) % 8;
    const pad  = used === 0 ? 0 : (8 - used);
    this._grow(4 + len + pad);
    this._view.setUint32(this._len, len, true); this._len += 4;
    this._buf.set(bytes, this._len); this._len += len;
    if (pad > 0) { this._buf.fill(0, this._len, this._len + pad); this._len += pad; }
  }

  writeBytes(slot, data) {
    this._slotSigils[slot] = SIGIL_BINARY;
    this._markSlot(slot);
    const len = data.length;
    const used = (4 + len) % 8;
    const pad  = used === 0 ? 0 : (8 - used);
    this._grow(4 + len + pad);
    this._view.setUint32(this._len, len, true); this._len += 4;
    this._buf.set(data, this._len); this._len += len;
    if (pad > 0) { this._buf.fill(0, this._len, this._len + pad); this._len += pad; }
  }

  writeListI64(slot, values) {
    this._markSlot(slot); // list is var-length — keep SIGIL_STR default
    const total = 16 + values.length * 8;
    this._writeU32(MAGIC_LIST);
    this._writeU32(total);
    this._writeByte(0x3D); // SIGIL_INT '='
    this._writeU32(values.length);
    this._writeByte(0); this._writeByte(0); this._writeByte(0); // padding
    for (const v of values) {
      let lo, hi;
      if (typeof v === "bigint") {
        const u = BigInt.asUintN(64, v);
        lo = Number(u & 0xFFFFFFFFn);
        hi = Number(u >> 32n);
      } else {
        lo = v >>> 0;
        hi = Math.floor(v / 4294967296) >>> 0;
      }
      this._writeI64Parts(lo, hi);
    }
  }

  writeListF64(slot, values) {
    this._markSlot(slot);
    const total = 16 + values.length * 8;
    this._writeU32(MAGIC_LIST);
    this._writeU32(total);
    this._writeByte(0x7E); // SIGIL_FLOAT '~'
    this._writeU32(values.length);
    this._writeByte(0); this._writeByte(0); this._writeByte(0);
    for (const v of values) {
      this._writeBytes(encodeF64LE(v));
    }
  }

  // ── Convenience: write from record objects ───────────────────────────────

  /**
   * Write multiple records from plain JS objects.
   * @param {string[]} keys
   * @param {object[]} records
   * @returns {Uint8Array}
   */
  static fromRecords(keys, records) {
    const schema = new NxsSchema(keys);
    const w = new NxsWriter(schema);
    for (const rec of records) {
      w.beginObject();
      for (let i = 0; i < keys.length; i++) {
        const key = keys[i];
        if (!(key in rec)) continue;
        const val = rec[key];
        if (val === null || val === undefined) {
          w.writeNull(i);
        } else if (typeof val === "bigint") {
          w.writeI64(i, val);
        } else if (typeof val === "number") {
          if (Number.isSafeInteger(val)) {
            w.writeI64(i, val);
          } else {
            w.writeF64(i, val);
          }
        } else if (typeof val === "boolean") {
          w.writeBool(i, val);
        } else if (typeof val === "string") {
          w.writeStr(i, val);
        }
      }
      w.endObject();
    }
    return w.finish();
  }

  // ── Reset for WAL reuse ──────────────────────────────────────────────────

  reset() {
    this._len = 0;
    this._frames = [];
    this._recordOffsets = [];
    this._slotSigils.fill(SIGIL_STR);
  }

  // ── Private helpers ──────────────────────────────────────────────────────

  _markSlot(slot) {
    const frame = this._frames[this._frames.length - 1];
    if (!frame) throw new Error("no active object");

    // Set bitmask bit (slot index → byte/bit in LEB128 encoding)
    const byteIdx = Math.floor(slot / 7);
    const bitIdx  = slot % 7;
    frame.bitmask[byteIdx] |= (1 << bitIdx);

    // Record relative offset from object start
    const rel = this._len - frame.start;

    if (slot < frame.lastSlot) {
      frame.needsSort = true;
    }
    frame.lastSlot = slot;

    frame.offsetTable.push(rel);
    frame.slotOffsets.push({ slot, bufOff: this._len });
  }

  _writeByte(b) {
    this._grow(1);
    this._buf[this._len++] = b & 0xFF;
  }

  _writeU32(v) {
    this._grow(4);
    this._view.setUint32(this._len, v, true);
    this._len += 4;
  }

  _writeI64Parts(lo, hi) {
    this._grow(8);
    this._view.setUint32(this._len, lo, true);
    this._view.setUint32(this._len + 4, hi, true);
    this._len += 8;
  }

  _writeBytes(bytes) {
    if (bytes.length === 0) return;
    this._grow(bytes.length);
    this._buf.set(bytes, this._len);
    this._len += bytes.length;
  }

  /**
   * Return a view of the written bytes (no merge needed — single contiguous buffer).
   */
  _materialize() {
    return this._buf.subarray(0, this._len);
  }

  /**
   * Materialise and return the final data-sector bytes.
   */
  _materializeAll() {
    return this._materialize();
  }
}

// ── Helpers ──────────────────────────────────────────────────────────────────

const _utf8Encoder = new TextEncoder();

function encodeUtf8(s) {
  return _utf8Encoder.encode(s);
}

function buildSchema(keys, sigils) {
  const keyCount = keys.length;
  // Encode all keys as UTF-8 bytes
  const encoded = keys.map(k => _utf8Encoder.encode(k));

  // Size: u16 + keyCount bytes (type manifest) + null-terminated strings
  let size = 2 + keyCount; // KeyCount u16 + TypeManifest
  for (const e of encoded) size += e.length + 1; // string + null terminator
  // Pad to 8-byte boundary
  const padded = size + ((8 - (size % 8)) % 8);

  const buf = new Uint8Array(padded);
  let p = 0;

  // KeyCount u16 LE
  buf[p++] = keyCount & 0xFF;
  buf[p++] = (keyCount >>> 8) & 0xFF;

  // TypeManifest: emit per-slot sigil
  for (let i = 0; i < keyCount; i++) buf[p++] = sigils[i];

  // StringPool
  for (const e of encoded) {
    buf.set(e, p); p += e.length;
    buf[p++] = 0x00; // null terminator
  }
  // Remaining bytes are already zero (padding)

  return buf;
}

function buildTailIndexRecords(dataStartAbs, recordOffsets, tailPtr) {
  const n = recordOffsets.length;
  // EntryCount(4) + N * [KeyID(2) + AbsOff(8)] + FooterTailPtr(8) + MagicFooter(4)
  const buf = new Uint8Array(4 + n * 10 + 12);
  let p = 0;

  // EntryCount
  buf[p++] = n & 0xFF;
  buf[p++] = (n >>> 8) & 0xFF;
  buf[p++] = (n >>> 16) & 0xFF;
  buf[p++] = (n >>> 24) & 0xFF;

  for (let i = 0; i < n; i++) {
    // KeyID = record index (u16)
    buf[p++] = i & 0xFF;
    buf[p++] = (i >>> 8) & 0xFF;
    // AbsoluteOffset (u64) = dataStartAbs + relOff
    const abs = dataStartAbs + recordOffsets[i];
    // Write as 8-byte LE number (safe up to 2^53)
    const lo = abs >>> 0;
    const hi = Math.floor(abs / 0x100000000);
    buf[p++] = lo & 0xFF;
    buf[p++] = (lo >>> 8) & 0xFF;
    buf[p++] = (lo >>> 16) & 0xFF;
    buf[p++] = (lo >>> 24) & 0xFF;
    buf[p++] = hi & 0xFF;
    buf[p++] = (hi >>> 8) & 0xFF;
    buf[p++] = (hi >>> 16) & 0xFF;
    buf[p++] = (hi >>> 24) & 0xFF;
  }

  {
    const lo = tailPtr >>> 0;
    const hi = Math.floor(tailPtr / 0x100000000);
    buf[p++] = lo & 0xFF;
    buf[p++] = (lo >>> 8) & 0xFF;
    buf[p++] = (lo >>> 16) & 0xFF;
    buf[p++] = (lo >>> 24) & 0xFF;
    buf[p++] = hi & 0xFF;
    buf[p++] = (hi >>> 8) & 0xFF;
    buf[p++] = (hi >>> 16) & 0xFF;
    buf[p++] = (hi >>> 24) & 0xFF;
  }

  // MagicFooter
  buf[p++] = MAGIC_FOOTER & 0xFF;
  buf[p++] = (MAGIC_FOOTER >>> 8) & 0xFF;
  buf[p++] = (MAGIC_FOOTER >>> 16) & 0xFF;
  buf[p++] = (MAGIC_FOOTER >>> 24) & 0xFF;

  return buf;
}
