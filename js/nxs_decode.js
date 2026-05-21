/**
 * Decode `.nxb` wire bytes to human-readable `.nxs`-style text for debugging.
 * Used by the Nyxis DevTools extension and any audit/review tooling.
 */
import { NxsReader, NxsObject, NxsError, WIRE_SIGILS } from "./nxs.js";

export const NXS_MAGIC_FILE = 0x4E595842; // NYXB

const MAGIC_OBJ = 0x4E59584F;
const MAGIC_LIST = 0x4E59584C;

export function isNxbBuffer(bytes) {
  if (!bytes || bytes.byteLength < 4) return false;
  const u8 = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  return new DataView(u8.buffer, u8.byteOffset, u8.byteLength).getUint32(0, true) === NXS_MAGIC_FILE;
}

function isNxsObject(v) {
  return v && typeof v === "object" && typeof v.get === "function" && typeof v.toObject === "function";
}

function escapeStr(s) {
  return s
    .replace(/\\/g, "\\\\")
    .replace(/"/g, '\\"')
    .replace(/\n/g, "\\n")
    .replace(/\r/g, "\\r")
    .replace(/\t/g, "\\t");
}

function formatDecodedValue(v, sigil) {
  if (v === null || v === undefined) return "^";
  if (isNxsObject(v)) return null; // caller formats block
  if (Array.isArray(v)) {
    return `[${v.map(item => formatDecodedValue(item, null)).join(", ")}]`;
  }
  if (typeof v === "boolean") return v ? "?true" : "?false";
  if (typeof v === "bigint") return `=${v}`;
  if (typeof v === "number") {
    if (sigil === WIRE_SIGILS.float) return `~${v}`;
    if (sigil === WIRE_SIGILS.time) {
      const ms = Math.round(v / 1_000_000);
      const d = new Date(ms);
      if (Number.isFinite(d.getTime())) {
        return `@${d.toISOString().slice(0, 10)}`;
      }
    }
    return Number.isInteger(v) ? `=${v}` : `~${v}`;
  }
  if (typeof v === "string") {
    if (sigil === WIRE_SIGILS.keyword) return `$${v}`;
    return `"${escapeStr(v)}"`;
  }
  return `"${escapeStr(String(v))}"`;
}

function isPrintableAscii(s) {
  if (typeof s !== "string" || s.length === 0 || s.length > 4096) return false;
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    if (c < 0x20 || c > 0x7e) return false;
  }
  return true;
}

function readFieldValue(obj, reader, slot, sigil) {
  const off = obj._resolveSlot(slot);
  if (off < 0) return { v: undefined, sigil };

  const bytes = reader.bytes;
  const view = reader.view;
  if (off + 4 <= bytes.length) {
    const magic = view.getUint32(off, true);
    if (magic === MAGIC_OBJ) {
      return { v: new NxsObject(reader, off), sigil: null };
    }
    if (magic === MAGIC_LIST) {
      return { v: obj._decodeList(off), sigil: null };
    }
  }

  if (sigil === WIRE_SIGILS.bool) {
    return { v: obj.getBoolBySlot(slot), sigil };
  }
  if (sigil === WIRE_SIGILS.float) {
    const f64 = obj.getF64BySlot(slot);
    if (f64 !== undefined) return { v: f64, sigil };
  }
  if (sigil === WIRE_SIGILS.int || sigil === WIRE_SIGILS.time) {
    const i64 = obj.getI64BySlot(slot);
    if (i64 !== undefined) return { v: Number(i64), sigil };
  }
  if (sigil === WIRE_SIGILS.str || sigil === WIRE_SIGILS.keyword) {
    const str = obj.getStrBySlot(slot);
    if (str !== undefined) return { v: str, sigil };
  }

  const str = obj.getStrBySlot(slot);
  if (isPrintableAscii(str)) {
    return { v: str, sigil: WIRE_SIGILS.str };
  }

  const f64 = obj.getF64BySlot(slot);
  if (f64 !== undefined && Number.isFinite(f64) && Math.abs(f64) < 1e15) {
    return { v: f64, sigil: WIRE_SIGILS.float };
  }

  const i64 = obj.getI64BySlot(slot);
  if (i64 !== undefined && Number.isFinite(Number(i64)) && Math.abs(Number(i64)) < 9e15) {
    return { v: Number(i64), sigil: sigil === WIRE_SIGILS.time ? WIRE_SIGILS.time : WIRE_SIGILS.int };
  }

  const bool = obj.getBoolBySlot(slot);
  if (bool === true || bool === false) {
    return { v: bool, sigil: WIRE_SIGILS.bool };
  }

  if (str !== undefined && str !== "") {
    return { v: str, sigil: WIRE_SIGILS.str };
  }

  return { v: undefined, sigil };
}

function presentSlotIndices(obj, reader) {
  obj._buildRankCache();
  const out = [];
  for (let slot = 0; slot < reader.keys.length; slot++) {
    if (obj._present[slot]) out.push(slot);
  }
  return out;
}

function formatObjectBlock(name, obj, reader, indent) {
  const pad = "  ".repeat(indent);
  const lines = [`${pad}${name} {`];

  for (const slot of presentSlotIndices(obj, reader)) {
    const key = reader.keys[slot];
    if (key === name) continue;
    const sigil = reader.keySigils[slot];
    const { v, sigil: wireSigil } = readFieldValue(obj, reader, slot, sigil);
    if (isNxsObject(v)) {
      lines.push(formatObjectBlock(key, v, reader, indent + 1));
    } else {
      lines.push(`${pad}  ${key}: ${formatDecodedValue(v, wireSigil ?? sigil)}`);
    }
  }

  lines.push(`${pad}}`);
  return lines.join("\n");
}

function formatRecord(reader, index) {
  const obj = reader.record(index);
  const presentSlots = presentSlotIndices(obj, reader);

  if (presentSlots.length === 1) {
    const off = obj._resolveSlot(presentSlots[0]);
    if (off >= 0 && off + 4 <= reader.bytes.length) {
      const magic = reader.view.getUint32(off, true);
      if (magic === MAGIC_OBJ) {
        const nested = new NxsObject(reader, off);
        return formatObjectBlock(reader.keys[presentSlots[0]], nested, reader, 0);
      }
    }
  }

  const blockName = reader.recordCount > 1 ? `record_${index}` : (reader.keys[presentSlots[0]] ?? "record");
  return formatObjectBlock(blockName, obj, reader, 0);
}

/**
 * Decode an NXB buffer to `.nxs`-style source text.
 * @param {ArrayBuffer|Uint8Array} input
 * @returns {string}
 */
export function decodeToNxs(input) {
  const bytes = input instanceof Uint8Array
    ? input
    : new Uint8Array(input);
  if (!isNxbBuffer(bytes)) {
    throw new NxsError("ERR_BAD_MAGIC", "not an NXB file (expected NYXB preamble)");
  }

  const reader = new NxsReader(bytes);
  const lines = [
    `# Nyxis decode — NYXB v${reader.version >> 8}.${reader.version & 0xff}, ${reader.recordCount} record(s)`,
    `# schema keys (${reader.keys.length}): ${reader.keys.join(", ")}`,
    "",
  ];

  for (let i = 0; i < reader.recordCount; i++) {
    lines.push(formatRecord(reader, i));
    if (i + 1 < reader.recordCount) lines.push("");
  }

  return lines.join("\n");
}

/** Alias for extension / docs. */
export const NyxisJsSDK = {
  decodeToNxs,
  isNxbBuffer,
  NxsReader,
};
