// NXS v1.3 compact row decode — mirrors nyxis/rust/src/compact.rs (read path).

import { byteAt } from "./sparse_bytes.js";

export const FLAG_DENSE_FRAMES = 0x0010;
export const FLAG_PACKED_BOOLS = 0x0020;
export const FLAG_NARROW_CELLS = 0x0040;
export const FLAG_DELTA_TAIL = 0x0080;
export const FLAG_DENSE_WIRE_REORDER = 0x0100;
export const FLAG_V13_COMPACT_MASK =
  FLAG_DENSE_FRAMES | FLAG_PACKED_BOOLS | FLAG_NARROW_CELLS | FLAG_DELTA_TAIL | FLAG_DENSE_WIRE_REORDER;

export const RECORD_HDR_DENSE = 0x01;
const FIELD_ATTR_PROMOTED = 0x01;
const FIELD_ATTR_U16_LEN = 0x02;
const MAGIC_OBJ = 0x4e59584f;

const SIGIL_INT = 0x3d;
const SIGIL_FLOAT = 0x7e;
const SIGIL_BOOL = 0x3f;
const SIGIL_KEYWORD = 0x24;
const SIGIL_STR = 0x22;
const SIGIL_TIME = 0x40;

const _scratchBuf = new ArrayBuffer(8);
const _scratchU8 = new Uint8Array(_scratchBuf);
const _scratchF32 = new Float32Array(_scratchBuf);
const _scratchF64 = new Float64Array(_scratchBuf);
const _utf8Decoder = new TextDecoder("utf-8");

function rdU16(bytes, off) {
  return byteAt(bytes, off) | (byteAt(bytes, off + 1) << 8);
}

function rdU32(bytes, off) {
  return (
    byteAt(bytes, off) |
    (byteAt(bytes, off + 1) << 8) |
    (byteAt(bytes, off + 2) << 16) |
    (byteAt(bytes, off + 3) << 24)
  ) >>> 0;
}

function alignTo(pos, align) {
  if (!align) return pos;
  return (pos + align - 1) & ~(align - 1);
}

function isVarSigil(sig) {
  return sig === SIGIL_STR || sig === 0x3c;
}

function decodeUtf8(bytes, offset, length) {
  const end = offset + length;
  let i = offset;
  while (i < end) {
    if (byteAt(bytes, i) & 0x80) {
      return _utf8Decoder.decode(bytes.subarray(offset, end));
    }
    i++;
  }
  if (length < 1024) {
    return String.fromCharCode.apply(null, bytes.subarray(offset, end));
  }
  let s = "";
  for (let p = offset; p < end; p += 4096) {
    s += String.fromCharCode.apply(null, bytes.subarray(p, Math.min(p + 4096, end)));
  }
  return s;
}

/** @typedef {{ keys: string[], sigils: number[], widths: number[], fieldAttrs: number[], valuePool: string[] }} ExtendedSchema */

/**
 * @param {Uint8Array} data
 * @param {number} pos
 * @param {number} flags
 * @returns {{ schema: ExtendedSchema, end: number }}
 */
export function parseExtendedSchema(data, pos, flags) {
  const keyCount = rdU16(data, pos);
  pos += 2;
  const sigils = [];
  for (let i = 0; i < keyCount; i++) sigils.push(byteAt(data, pos + i));
  pos += keyCount;

  const keys = [];
  for (let i = 0; i < keyCount; i++) {
    let end = pos;
    while (end < data.length && byteAt(data, end) !== 0) end++;
    keys.push(decodeUtf8(data, pos, end - pos));
    pos = end + 1;
  }
  if (pos % 8 !== 0) pos = alignTo(pos, 8);

  const widths = new Array(keyCount).fill(0);
  if (flags & FLAG_NARROW_CELLS) {
    for (let i = 0; i < keyCount; i++) widths[i] = byteAt(data, pos + i);
    pos += keyCount;
  }

  const fieldAttrs = new Array(keyCount).fill(0);
  if (flags & FLAG_V13_COMPACT_MASK) {
    for (let i = 0; i < keyCount; i++) fieldAttrs[i] = byteAt(data, pos + i);
    pos += keyCount;
  }

  const valuePool = [];
  if (pos + 2 <= data.length) {
    const valueCount = rdU16(data, pos);
    pos += 2;
    for (let i = 0; i < valueCount; i++) {
      let end = pos;
      while (end < data.length && byteAt(data, end) !== 0) end++;
      valuePool.push(decodeUtf8(data, pos, end - pos));
      pos = end + 1;
    }
    if (valueCount > 0 && pos % 8 !== 0) pos = alignTo(pos, 8);
  }

  return {
    schema: { keys, sigils, widths, fieldAttrs, valuePool },
    end: pos,
  };
}

function isPromoted(schema, slot) {
  return (schema.fieldAttrs[slot] & FIELD_ATTR_PROMOTED) !== 0;
}

function isU16Len(schema, slot) {
  return (schema.fieldAttrs[slot] & FIELD_ATTR_U16_LEN) !== 0;
}

function strLenPrefix(schema, slot) {
  if (isPromoted(schema, slot)) return 0;
  if (isU16Len(schema, slot)) return 2;
  return 4;
}

function cellWidth(schema, slot) {
  if (isPromoted(schema, slot) || schema.sigils[slot] === SIGIL_KEYWORD) return 2;
  const w = schema.widths[slot] || 0;
  return w === 0 ? 8 : w;
}

function boolSlots(schema) {
  const out = [];
  for (let i = 0; i < schema.sigils.length; i++) {
    if (schema.sigils[i] === SIGIL_BOOL) out.push(i);
  }
  return out;
}

function denseCellAlignWidth(fi, schema, plan) {
  if (plan.packedBools && plan.boolSlots.includes(fi)) return plan.boolWordBytes();
  const sig = schema.sigils[fi];
  if (isVarSigil(sig)) return 0;
  if (isPromoted(schema, fi) || sig === SIGIL_KEYWORD) return 2;
  if (sig === SIGIL_INT || sig === SIGIL_FLOAT) {
    return plan.narrow ? cellWidth(schema, fi) : 8;
  }
  if (sig === SIGIL_TIME) return 8;
  if (sig === SIGIL_BOOL && !plan.packedBools) return 8;
  if (sig === 0x5e) return 1;
  return 8;
}

function denseWireOrder(schema, plan) {
  if (!plan.denseWireReorder) {
    return schema.keys.map((_, i) => i);
  }
  const fixed = [];
  const vars = [];
  for (let fi = 0; fi < schema.keys.length; fi++) {
    const sig = schema.sigils[fi];
    if (isVarSigil(sig)) {
      vars.push(fi);
      continue;
    }
    if (plan.packedBools && plan.boolSlots.includes(fi)) {
      if (plan.firstBool === fi) fixed.push([plan.boolWordBytes(), fi]);
      continue;
    }
    fixed.push([denseCellAlignWidth(fi, schema, plan), fi]);
  }
  fixed.sort((a, b) => b[0] - a[0] || a[1] - b[1]);
  return fixed.map(([, s]) => s).concat(vars);
}

function advancePastBoolWord(pos, plan) {
  const bw = plan.boolWordBytes();
  return alignTo(pos, bw) + bw;
}

function advanceDensePastCellFixed(pos, fi, schema, plan) {
  const sig = schema.sigils[fi];
  const w = plan.narrow ? cellWidth(schema, fi) : 8;
  if (plan.packedBools && plan.boolSlots.includes(fi)) {
    return advancePastBoolWord(pos, plan);
  }
  if ((sig === SIGIL_INT || sig === SIGIL_FLOAT || sig === SIGIL_BOOL) && (!plan.packedBools || sig !== SIGIL_BOOL)) {
    return alignTo(pos, w) + w;
  }
  if (sig === SIGIL_KEYWORD) return alignTo(pos, 2) + 2;
  return alignTo(pos, 8) + 8;
}

function precomputeDenseFixedOffsets(schema, plan) {
  const n = schema.keys.length;
  const out = new Array(n).fill(null);
  let pos = 0;
  let varStart = null;
  for (const fi of plan.wireOrder) {
    if (plan.packedBools && plan.boolSlots.includes(fi)) {
      if (plan.firstBool === fi) {
        const bw = plan.boolWordBytes();
        const base = alignTo(pos, bw);
        for (let idx = 0; idx < plan.boolSlots.length; idx++) {
          out[plan.boolSlots[idx]] = base + Math.floor(idx / 8);
        }
        pos = base + bw;
      }
      continue;
    }
    const sig = schema.sigils[fi];
    if (isVarSigil(sig)) {
      varStart = pos;
      break;
    }
    const w = plan.narrow ? cellWidth(schema, fi) : 8;
    const off =
      isPromoted(schema, fi) || sig === SIGIL_KEYWORD ? alignTo(pos, 2) : alignTo(pos, w);
    out[fi] = off;
    pos = advanceDensePastCellFixed(pos, fi, schema, plan);
  }
  return { denseFixedBodyOffsets: out, denseVarBodyStart: varStart };
}

export class RowCellPlan {
  /** @param {ExtendedSchema} schema @param {number} flags */
  constructor(schema, flags) {
    this.packedBools = (flags & FLAG_PACKED_BOOLS) !== 0;
    this.narrow = (flags & FLAG_NARROW_CELLS) !== 0;
    this.denseAllowed = (flags & FLAG_DENSE_FRAMES) !== 0;
    this.denseWireReorder = (flags & FLAG_DENSE_WIRE_REORDER) !== 0;
    this.boolSlots = boolSlots(schema);
    this.firstBool = this.boolSlots.length ? this.boolSlots[0] : null;
    this.wireOrder = denseWireOrder(schema, this);
    if (this.denseWireReorder && this.denseAllowed) {
      const { denseFixedBodyOffsets, denseVarBodyStart } = precomputeDenseFixedOffsets(schema, this);
      this.denseFixedBodyOffsets = denseFixedBodyOffsets;
      this.denseVarBodyStart = denseVarBodyStart;
    } else {
      this.denseFixedBodyOffsets = null;
      this.denseVarBodyStart = null;
    }
  }

  boolWordBytes() {
    if (!this.packedBools || !this.boolSlots.length) return 0;
    return Math.max(1, Math.ceil(this.boolSlots.length / 8));
  }

  denseWireOrder(_schema) {
    return this.wireOrder;
  }
}

export function readStrCellLen(data, off, prefixLen) {
  if (prefixLen === 2) return rdU16(data, off);
  if (prefixLen === 4) return rdU32(data, off);
  return -1;
}

function advancePastStrCell(pos, payloadLen, prefixLen) {
  const cellBytes = prefixLen + payloadLen;
  const pad = (8 - (cellBytes % 8)) % 8;
  return pos + cellBytes + pad;
}

function advanceDensePastCell(data, bodyBase, pos, fi, schema, plan) {
  const sig = schema.sigils[fi];
  const w = plan.narrow ? cellWidth(schema, fi) : 8;
  if (plan.packedBools && plan.boolSlots.includes(fi)) {
    return advancePastBoolWord(pos, plan);
  }
  if ((sig === SIGIL_INT || sig === SIGIL_FLOAT || sig === SIGIL_BOOL) && (!plan.packedBools || sig !== SIGIL_BOOL)) {
    return alignTo(pos, w) + w;
  }
  if (sig === SIGIL_STR || sig === 0x3c) {
    if (isPromoted(schema, fi)) return alignTo(pos, 2) + 2;
    const prefix = strLenPrefix(schema, fi);
    const abs = bodyBase + pos;
    const len = readStrCellLen(data, abs, prefix);
    return advancePastStrCell(pos, len, prefix);
  }
  if (sig === SIGIL_KEYWORD) return alignTo(pos, 2) + 2;
  return alignTo(pos, 8) + 8;
}

export function decodeIntCell(data, offset, width) {
  switch (width) {
    case 1:
      return (byteAt(data, offset) << 24) >> 24;
    case 2: {
      const v = rdU16(data, offset);
      return v > 0x7fff ? v - 0x10000 : v;
    }
    case 4:
      return rdU32(data, offset) | 0;
    case 8: {
      const lo = rdU32(data, offset);
      const hi = rdU32(data, offset + 4) | 0;
      return hi * 0x100000000 + lo;
    }
    default:
      return undefined;
  }
}

export function decodeF64Cell(data, offset, width) {
  if (width === 4) {
    _scratchU8[0] = byteAt(data, offset);
    _scratchU8[1] = byteAt(data, offset + 1);
    _scratchU8[2] = byteAt(data, offset + 2);
    _scratchU8[3] = byteAt(data, offset + 3);
    return _scratchF32[0];
  }
  if (width === 8) {
    _scratchU8[0] = byteAt(data, offset);
    _scratchU8[1] = byteAt(data, offset + 1);
    _scratchU8[2] = byteAt(data, offset + 2);
    _scratchU8[3] = byteAt(data, offset + 3);
    _scratchU8[4] = byteAt(data, offset + 4);
    _scratchU8[5] = byteAt(data, offset + 5);
    _scratchU8[6] = byteAt(data, offset + 6);
    _scratchU8[7] = byteAt(data, offset + 7);
    return _scratchF64[0];
  }
  return undefined;
}

export function isDenseRecord(data, offset) {
  if (offset + 9 > data.length) return false;
  if (rdU32(data, offset) !== MAGIC_OBJ) return false;
  return (byteAt(data, offset + 8) & RECORD_HDR_DENSE) !== 0;
}

function denseFieldOffset(data, objOffset, slot, schema, plan) {
  const bodyBase = objOffset + 9;
  if (plan.denseFixedBodyOffsets) {
    const bodyRel = plan.denseFixedBodyOffsets[slot];
    if (bodyRel != null) return bodyBase + bodyRel;
    if (plan.denseVarBodyStart != null) {
      let pos = plan.denseVarBodyStart;
      for (const fi of plan.denseWireOrder(schema)) {
        if (!isVarSigil(schema.sigils[fi])) continue;
        if (fi === slot) return bodyBase + pos;
        pos = advanceDensePastCell(data, bodyBase, pos, fi, schema, plan);
      }
      return null;
    }
  }
  let pos = 0;
  for (const fi of plan.denseWireOrder(schema)) {
    if (plan.packedBools && plan.boolSlots.includes(fi)) {
      if (plan.boolSlots.includes(slot) && fi === plan.firstBool) {
        const byte = Math.floor(plan.boolSlots.indexOf(slot) / 8);
        return bodyBase + alignTo(pos, plan.boolWordBytes()) + byte;
      }
      if (fi === plan.firstBool) pos = advancePastBoolWord(pos, plan);
      continue;
    }
    const sig = schema.sigils[fi];
    const w = plan.narrow ? cellWidth(schema, fi) : 8;
    if (fi === slot) {
      const off =
        isPromoted(schema, fi) || sig === SIGIL_KEYWORD
          ? alignTo(pos, 2)
          : isVarSigil(sig)
            ? pos
            : alignTo(pos, w);
      return bodyBase + off;
    }
    pos = advanceDensePastCell(data, bodyBase, pos, fi, schema, plan);
  }
  return null;
}

function resolveSlotV12(data, objOffset, slot) {
  let p = objOffset + 8;
  let cur = 0;
  let tableIdx = 0;
  let found = false;
  let b;
  for (;;) {
    b = byteAt(data, p++);
    const bits = b & 0x7f;
    for (let bit = 0; bit < 7; bit++) {
      if (cur === slot) {
        if (((bits >> bit) & 1) === 0) return null;
        found = true;
      } else if (cur < slot && ((bits >> bit) & 1)) {
        tableIdx++;
      }
      cur++;
    }
    if (found && (b & 0x80) === 0) break;
    if (cur > slot && found) break;
    if ((b & 0x80) === 0) return null;
  }
  while (b & 0x80) b = byteAt(data, p++);
  const rel = rdU16(data, p + tableIdx * 2);
  return objOffset + rel;
}

function sparseOffsetTableSlots(presentSlots, plan) {
  if (!plan.packedBools) return presentSlots.slice();
  const out = [];
  let boolWordAdded = false;
  for (const fi of presentSlots) {
    if (plan.boolSlots.includes(fi)) {
      if (!boolWordAdded) {
        out.push(fi);
        boolWordAdded = true;
      }
    } else {
      out.push(fi);
    }
  }
  return out;
}

function resolveSlotV13Sparse(data, objOffset, slot, schema, plan) {
  let p = objOffset + 9;
  let cur = 0;
  let slotPresent = false;
  const presentSlots = [];
  const fieldCount = schema.keys.length;

  let done = false;
  while (!done) {
    const b = byteAt(data, p++);
    const bits = b & 0x7f;
    for (let bit = 0; bit < 7; bit++) {
      if ((bits >> bit) & 1) {
        presentSlots.push(cur);
        if (cur === slot) slotPresent = true;
      }
      cur++;
      if (cur >= fieldCount) {
        done = true;
        break;
      }
    }
    if ((b & 0x80) === 0) break;
  }

  if (!slotPresent) return null;

  const otSlots = sparseOffsetTableSlots(presentSlots, plan);
  const tableBase = p;
  if (plan.packedBools && plan.boolSlots.includes(slot)) {
    const boolTableFi = otSlots.find((fi) => plan.boolSlots.includes(fi));
    const tableIdx = otSlots.indexOf(boolTableFi);
    const rel = rdU16(data, tableBase + tableIdx * 2);
    const base = objOffset + rel;
    const bitInWord = plan.boolSlots.indexOf(slot);
    return base + Math.floor(bitInWord / 8);
  }
  const tableIdx = otSlots.indexOf(slot);
  const rel = rdU16(data, tableBase + tableIdx * 2);
  return objOffset + rel;
}

/**
 * @returns {number | null} absolute byte offset of field value
 */
export function resolveFieldOffset(data, objOffset, slot, schema, plan, denseFrames) {
  if (denseFrames) {
    const hdr = byteAt(data, objOffset + 8);
    if (hdr & RECORD_HDR_DENSE) {
      return denseFieldOffset(data, objOffset, slot, schema, plan);
    }
    return resolveSlotV13Sparse(data, objOffset, slot, schema, plan);
  }
  return resolveSlotV12(data, objOffset, slot);
}

export function readPackedBool(data, objOffset, slot, schema, plan) {
  const off = resolveFieldOffset(data, objOffset, slot, schema, plan, true);
  if (off == null) return undefined;
  const bitPos = plan.boolSlots.indexOf(slot);
  const b = byteAt(data, off);
  return ((b >> (bitPos % 8)) & 1) === 1;
}

/**
 * @typedef {{ tailPtr: number, recordCount: number, blockSize: number, singleKeyId: boolean, anchorsOff: number, deltasOff: number }} DeltaTailLayout
 */

export function parseDeltaTailLayout(data, tailPtr) {
  const recordCount = rdU32(data, tailPtr);
  const blockSize = rdU32(data, tailPtr + 4);
  const tiFlags = rdU16(data, tailPtr + 8);
  const anchorCount = rdU16(data, tailPtr + 10);
  const anchorsOff = tailPtr + alignTo(12, 8);
  const deltasOff = anchorsOff + anchorCount * 8;
  const singleKeyId = (tiFlags & 0x0001) !== 0;
  return {
    tailPtr,
    recordCount,
    blockSize,
    singleKeyId,
    anchorsOff,
    deltasOff,
  };
}

export function deltaRecordOffset(data, layout, index) {
  if (index >= layout.recordCount) return null;
  const a = Math.max(layout.blockSize, 1);
  const anchorIdx = Math.floor(index / a);
  const anchorOff = layout.anchorsOff + anchorIdx * 8;
  const lo = rdU32(data, anchorOff);
  const hi = rdU32(data, anchorOff + 4);
  const anchor = hi * 0x100000000 + lo;
  const deltaOff = layout.deltasOff + index * 4;
  const delta = rdU32(data, deltaOff);
  return anchor + delta;
}

export function fieldCellWidth(schema, slot) {
  return cellWidth(schema, slot);
}

export function materialiseStrAt(data, off, slot, schema) {
  if (isPromoted(schema, slot)) {
    const idx = rdU16(data, off);
    return schema.valuePool[idx];
  }
  const prefix = strLenPrefix(schema, slot);
  const len = readStrCellLen(data, off, prefix);
  return decodeUtf8(data, off + prefix, len);
}
