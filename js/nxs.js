// NXS Reader — zero-copy .nxb parser for JavaScript
// Implements the Nyxis v1.1 binary wire format spec.
//
// Usage:
//   import { NxsReader } from "./nxs.js";
//   const buf = await fetch("data.nxb").then(r => r.arrayBuffer());
//   const reader = new NxsReader(buf);
//   console.log(reader.recordCount);           // how many top-level records
//   const obj = reader.record(42);              // O(1) jump to record #42
//   console.log(obj.get("username"));           // decode field only on access
//
// The reader does NOT materialize the whole file. Each call to `record()`
// returns a lightweight view; `.get(key)` decodes a single field on demand.
//
// Adaptive prefetch (optional): prefetch_viewport, cache_stats — see prefetch.js.

import { AccessPatternDetector, PATTERN_SEQUENTIAL } from "./pattern.js";
import { SparseBytes, byteAt } from "./sparse_bytes.js";
import {
  FLAG_V13_COMPACT_MASK,
  FLAG_DELTA_TAIL,
  FLAG_DENSE_FRAMES,
  parseExtendedSchema,
  RowCellPlan,
  parseDeltaTailLayout,
  deltaRecordOffset,
  resolveFieldOffset,
  decodeIntCell,
  decodeF64Cell,
  readPackedBool,
  materialiseStrAt,
  fieldCellWidth,
} from "./compact.js";
import {
  HINT_UNKNOWN,
  PageCache,
  InFlightMap,
  coalescePageIndices,
  clampRanges,
  pageIndicesForViewport,
  initialStrategy,
  rowDataSector,
  DEFAULT_MAX_PAGES,
  DEFAULT_PAGE_SIZE,
  DEFAULT_COALESCE_GAP_PAGES,
  DEFAULT_PREFETCH_DEPTH,
  UPGRADE_SEQUENTIAL_THRESHOLD,
  EAGER_THRESHOLD_MB,
} from "./prefetch.js";

export {
  HINT_UNKNOWN,
  HINT_SEQUENTIAL,
  HINT_RANDOM,
  HINT_FULL,
  HINT_PARTIAL,
} from "./prefetch.js";
export { SparseBytes } from "./sparse_bytes.js";

const MAGIC_FILE   = 0x4E595842; // NYXB
const MAGIC_OBJ    = 0x4E59584F; // NYXO
const MAGIC_LIST   = 0x4E59584C; // NYXL
const MAGIC_FOOTER = 0x2153584E; // NXS!

const FLAG_COLUMNAR = 0x0001;
const FLAG_PAX      = 0x0004;
const FLAG_SCHEMA_EMBEDDED = 0x0002;
const FOOTER_COL_BYTES = 20;
const FOOTER_PAX_BYTES = 28;
const MAGIC_PAGE     = 0x4E585350; // NYXP

const SIGIL_INT     = 0x3D; // =
const SIGIL_FLOAT   = 0x7E; // ~
const SIGIL_BOOL    = 0x3F; // ?
const SIGIL_KEYWORD = 0x24; // $
const SIGIL_STR     = 0x22; // "
const SIGIL_TIME    = 0x40; // @
const SIGIL_BINARY  = 0x3C; // <
const SIGIL_LINK    = 0x26; // &
const SIGIL_NULL    = 0x5E; // ^

/** Exposes all wire sigil bytes for spec parity (keeps otherwise-unused constants live). */
export const WIRE_SIGILS = Object.freeze({
  int: SIGIL_INT,
  float: SIGIL_FLOAT,
  bool: SIGIL_BOOL,
  keyword: SIGIL_KEYWORD,
  str: SIGIL_STR,
  time: SIGIL_TIME,
  binary: SIGIL_BINARY,
  link: SIGIL_LINK,
  null: SIGIL_NULL,
});

// ── Error types ─────────────────────────────────────────────────────────────

export class NxsError extends Error {
  constructor(code, msg) { super(`${code}: ${msg}`); this.code = code; }
}

// Shared TextDecoder for the unicode fallback path.
const _utf8Decoder = new TextDecoder("utf-8");

/**
 * TextDecoder.decode() refuses to accept a Uint8Array backed by a
 * SharedArrayBuffer. When we detect that case, copy the slice into a regular
 * ArrayBuffer before decoding. The cost is one tiny allocation on the fallback
 * path; worth it to make NXS usable from Web Workers that share their buffer.
 */
function _decodeMaybeShared(bytes, offset, end) {
  const view = bytes.subarray(offset, end);
  const buf = bytes._isSparseBytes ? view.buffer : bytes.buffer;
  // `buffer.constructor === SharedArrayBuffer` is the reliable check; older
  // engines don't even define the global, so guard the reference.
  if (typeof SharedArrayBuffer !== "undefined" && buf instanceof SharedArrayBuffer) {
    const copy = new Uint8Array(view.length);
    copy.set(view);
    return _utf8Decoder.decode(copy);
  }
  return _utf8Decoder.decode(view);
}

/**
 * Fast UTF-8 decode that specialises on the all-ASCII hot path (our string
 * data is typically usernames/emails/short codes). Scans for a high bit; if
 * none, uses String.fromCharCode which is ~10x faster than TextDecoder for
 * ASCII. Falls back to TextDecoder for anything with multibyte characters.
 */
function decodeUtf8Fast(bytes, offset, length) {
  // Quick ASCII scan — if any byte has high bit set, bail to TextDecoder.
  const end = offset + length;
  let i = offset;
  // Unroll 4-at-a-time for V8 to hoist
  const end4 = offset + (length & ~3);
  while (i < end4) {
    if ((byteAt(bytes, i) | byteAt(bytes, i + 1) | byteAt(bytes, i + 2) | byteAt(bytes, i + 3)) & 0x80) {
      return _decodeMaybeShared(bytes, offset, end);
    }
    i += 4;
  }
  while (i < end) {
    if (byteAt(bytes, i) & 0x80) {
      return _decodeMaybeShared(bytes, offset, end);
    }
    i++;
  }
  // All ASCII — build the string directly. For longer strings fromCharCode.apply
  // beats iteration; for short strings either is fine, so pick apply.
  if (length < 1024) {
    return String.fromCharCode.apply(null, bytes.subarray(offset, end));
  }
  // Very long strings: chunked apply to avoid argument-list size limits.
  let s = "";
  for (let p = offset; p < end; p += 4096) {
    s += String.fromCharCode.apply(null, bytes.subarray(p, Math.min(p + 4096, end)));
  }
  return s;
}

// ── Inline little-endian reads ───────────────────────────────────────────────
//
// DataView.getUint16/getFloat64 etc. cross a VM boundary on each call. For the
// hot loop, direct integer arithmetic on the Uint8Array inlines in V8, and
// Float64 reads through an aliased Float64Array are faster than DataView.
//
// An 8-byte scratch buffer is shared for float reads. Not thread-safe, but
// Node/the-browser runs one JS thread per isolate so that's fine.

const _scratchBuf = new ArrayBuffer(8);
const _scratchU8  = new Uint8Array(_scratchBuf);
const _scratchF64 = new Float64Array(_scratchBuf);

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

function nullBitmapBytesLen(n) {
  const raw = Math.ceil(n / 8);
  return (raw + 7) & ~7;
}

function isVarSigilByte(sig) {
  return sig === SIGIL_STR || sig === SIGIL_BINARY;
}

/** u32 offset table bytes for `rc` records: (rc+1)*4, or -1 on overflow. */
function varOffBytes(rc) {
  const rcU = rc >>> 0;
  const n = BigInt(rcU) + 1n;
  const off = n * 4n;
  if (off > BigInt(Number.MAX_SAFE_INTEGER)) return -1;
  return Number(off);
}

/** bytes required in offsets for index `recordIndex`: (recordIndex+2)*4, or -1. */
function varNeedBytes(recordIndex) {
  const ri = recordIndex >>> 0;
  const need = (BigInt(ri) + 2n) * 4n;
  if (need > BigInt(Number.MAX_SAFE_INTEGER)) return -1;
  return Number(need);
}

function fieldSectorLen(bytes, sectorOff, rc, sigil) {
  const rcU = rc >>> 0;
  const bmLen = nullBitmapBytesLen(rcU);
  if (!isVarSigilByte(sigil)) {
    const tail = BigInt(rcU) * 8n;
    if (tail > BigInt(Number.MAX_SAFE_INTEGER)) return -1;
    return bmLen + Number(tail);
  }
  const offBytes = varOffBytes(rcU);
  if (offBytes < 0) return -1;
  if (sectorOff + bmLen + offBytes > bytes.length) return -1;
  const end = rdU32(bytes, sectorOff + bmLen + rcU * 4);
  const total = bmLen + offBytes + end;
  if (sectorOff + total > bytes.length) return -1;
  return total;
}

function rdI64Safe(bytes, off) {
  // Two 32-bit reads combined into a JS number. Safe for |v| < 2^53.
  const lo = rdU32(bytes, off);
  const hi = rdU32(bytes, off + 4) | 0;
  return hi * 0x100000000 + lo;
}

function rdF64(bytes, off) {
  // Copy 8 bytes into the aliased Float64Array; read [0].
  _scratchU8[0] = byteAt(bytes, off);
  _scratchU8[1] = byteAt(bytes, off + 1);
  _scratchU8[2] = byteAt(bytes, off + 2);
  _scratchU8[3] = byteAt(bytes, off + 3);
  _scratchU8[4] = byteAt(bytes, off + 4);
  _scratchU8[5] = byteAt(bytes, off + 5);
  _scratchU8[6] = byteAt(bytes, off + 6);
  _scratchU8[7] = byteAt(bytes, off + 7);
  return _scratchF64[0];
}

function rdU64AsNumber(bytes, off) {
  // For offsets inside file size — safe up to 2^53.
  const lo = rdU32(bytes, off);
  const hi = rdU32(bytes, off + 4);
  return hi * 0x100000000 + lo;
}

// ── MurmurHash3-64 (schema integrity check) ──────────────────────────────────

function murmur3_64(bytes, offset, length) {
  const C1 = 0xFF51AFD7ED558CCDn;
  const C2 = 0xC4CEB9FE1A85EC53n;
  const MASK = 0xFFFFFFFFFFFFFFFFn;
  let h = 0x93681D6255313A99n;
  const end = offset + length;
  for (let p = offset; p < end; p += 8) {
    let k = 0n;
    for (let i = 0; i < 8 && p + i < end; i++) {
      k |= BigInt(byteAt(bytes, p + i)) << BigInt(i * 8);
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

// ── Main reader ─────────────────────────────────────────────────────────────

export class NxsReader {
  /**
   * @param {ArrayBuffer | Uint8Array} buffer — raw .nxb bytes
   * @param {object} [options] — prefetch: hint, maxPages, pageSize, coalesceGapPages, fetchRange
   */
  constructor(buffer, options = {}) {
    this._sparse = buffer instanceof SparseBytes ? buffer : null;
    this.bytes = this._sparse
      ? this._sparse.asIndexed()
      : buffer instanceof Uint8Array
        ? buffer
        : new Uint8Array(buffer);
    if (this._sparse) {
      this.view = new DataView(
        this._sparse._resident[0].data.buffer,
        this._sparse._resident[0].data.byteOffset,
        this._sparse._resident[0].data.byteLength,
      );
    } else {
      this.view = new DataView(this.bytes.buffer, this.bytes.byteOffset, this.bytes.byteLength);
    }

    if (this.bytes.length < 32) {
      throw new NxsError("ERR_OUT_OF_BOUNDS", "file too small");
    }

    // Preamble
    const magic = this.view.getUint32(0, true);
    if (magic !== MAGIC_FILE) {
      throw new NxsError("ERR_BAD_MAGIC", `expected 0x${MAGIC_FILE.toString(16)}, got 0x${magic.toString(16)}`);
    }

    this.version   = this.view.getUint16(4, true);
    this.flags     = this.view.getUint16(6, true);
    this._v13Compact = (this.flags & FLAG_V13_COMPACT_MASK) !== 0;
    this.extSchema = null;
    this.cellPlan = null;
    this._deltaTail = null;
    this._layout   = "row";
    this.dictHash  = this.view.getBigUint64(8, true);
    this.tailPtr   = Number(this.view.getBigUint64(16, true));
    // bytes 24-31 reserved

    // Footer check (sparse: footer lives in the tail resident span)
    const footer = this._sparse
      ? rdU32(this.bytes, this.bytes.length - 4)
      : this.view.getUint32(this.bytes.length - 4, true);
    if (footer !== MAGIC_FOOTER) {
      throw new NxsError("ERR_BAD_MAGIC", "footer magic mismatch");
    }
    const preambleTail = this.tailPtr;
    const layoutFlags = this.flags & (FLAG_COLUMNAR | FLAG_PAX);
    if (this.tailPtr === 0 && layoutFlags === 0) {
      if (this.bytes.length < 44) {
        throw new NxsError("ERR_OUT_OF_BOUNDS", "streamable footer missing tail pointer");
      }
      this.tailPtr = this._sparse
        ? Number(rdU64AsNumber(this.bytes, this.bytes.length - 12))
        : Number(this.view.getBigUint64(this.bytes.length - 12, true));
    }
    if (this._sparse) {
      const tailSpan = this._sparse._resident.find(
        (s) => this.tailPtr >= s.start && this.tailPtr < s.start + s.data.length,
      );
      if (!tailSpan) {
        throw new NxsError("ERR_OUT_OF_BOUNDS", "tail index not in resident span");
      }
      this._tailView = new DataView(
        tailSpan.data.buffer,
        tailSpan.data.byteOffset + (this.tailPtr - tailSpan.start),
        tailSpan.data.byteLength - (this.tailPtr - tailSpan.start),
      );
    }

    // Schema
    this.keys = [];
    this.keySigils = [];
    if (this.flags & FLAG_SCHEMA_EMBEDDED) {
      if (this._v13Compact) {
        const { schema, end } = parseExtendedSchema(this.bytes, 32, this.flags);
        this.extSchema = schema;
        this.keys = schema.keys;
        this.keySigils = schema.sigils;
        this.cellPlan = new RowCellPlan(schema, this.flags);
        this._schemaEnd = end;
        this.keyIndex = new Map();
        for (let i = 0; i < this.keys.length; i++) {
          this.keyIndex.set(this.keys[i], i);
        }
      } else {
        this._readSchema(32);
      }
      const computedHash = murmur3_64(this.bytes, 32, this._schemaEnd - 32);
      if (computedHash !== this.dictHash) {
        throw new NxsError("ERR_DICT_MISMATCH", "schema hash mismatch");
      }
    }

    this._colBufOff = [];
    this._colBufLen = [];
    this._colWarmed = new Set();
    this._colOverlay = new Map();
    this._colFetches = 0;

    if ((this.flags & FLAG_COLUMNAR) && (this.flags & FLAG_PAX)) {
      throw new NxsError("ERR_INVALID_FLAGS", "columnar and PAX both set");
    }
    if (this.flags & FLAG_COLUMNAR) {
      if (preambleTail === 0) {
        throw new NxsError("ERR_INCOMPATIBLE_FLAGS", "columnar requires sealed tail pointer");
      }
      this._layout = "columnar";
      this._readColumnarFooter();
    } else if (this.flags & FLAG_PAX) {
      this._layout = "pax";
      this._readPaxFooter();
    } else {
      this._layout = "row";
      if (this.flags & FLAG_DELTA_TAIL) {
        this._deltaTail = parseDeltaTailLayout(this.bytes, this.tailPtr);
        this.recordCount = this._deltaTail.recordCount;
        this._tailStart = this.tailPtr;
      } else {
        this._readTailIndex();
      }
      if (!this._sparse && this.recordCount > 0 && !this._deltaTail) {
        const tailSpanLen = 4 + this.recordCount * 10;
        if (this.tailPtr + tailSpanLen <= this.bytes.byteLength) {
          this._tailView = new DataView(
            this.bytes.buffer,
            this.bytes.byteOffset + this.tailPtr,
            tailSpanLen,
          );
        }
      }
    }

    const bytes = this.bytes;
    this._colFetchRange =
      options.fetchRange ??
      ((off, len) => bytes.subarray(off, off + len));
    this._initPrefetch(options);
    if (this._layout === "row" && this._prefetch.strategy === "eager") {
      this._startEagerBackground();
    }
  }

  /**
   * Prefetch one column buffer (columnar layout only; §7.4).
   * @param {string} key
   */
  prefetch_column(key) {
    if (this._layout !== "columnar") {
      throw new NxsError("ERR_LAYOUT", "prefetch_column requires columnar layout");
    }
    const slot = this._slotOf(key);
    if (slot < 0) throw new NxsError("ERR_KEY", `key ${key} not in schema`);
    if (this._colWarmed.has(slot)) return;
    const off = this._colBufOff[slot];
    const len = this._colBufLen[slot];
    let sector = this._colFetchRange(off, len);
    if (sector && typeof sector.then === "function") {
      throw new NxsError("ERR_UNSUPPORTED", "prefetch_column requires synchronous fetchRange");
    }
    if (!sector || typeof sector.byteLength !== "number") {
      throw new NxsError("ERR_INVALID", "fetchRange must return Uint8Array");
    }
    if (off + sector.byteLength > this.bytes.byteLength) {
      this._colOverlay.set(
        slot,
        sector instanceof Uint8Array ? sector : new Uint8Array(sector),
      );
    }
    this._colWarmed.add(slot);
    this._colFetches++;
  }

  _initPrefetch(options) {
    let maxPages = options.maxPages ?? DEFAULT_MAX_PAGES;
    if (typeof globalThis.navigator !== "undefined"
      && typeof globalThis.navigator.deviceMemory === "number"
      && globalThis.navigator.deviceMemory <= 2) {
      maxPages = Math.max(4, Math.floor(maxPages / 2));
    }
    const pageSize = options.pageSize ?? DEFAULT_PAGE_SIZE;
    const hint = options.hint ?? HINT_UNKNOWN;
    const abortController = new AbortController();
    if (options.signal) {
      if (options.signal.aborted) abortController.abort();
      else {
        options.signal.addEventListener("abort", () => abortController.abort(), { once: true });
      }
    }
    this._prefetch = {
      hint,
      pageSize,
      coalesceGapPages: options.coalesceGapPages ?? DEFAULT_COALESCE_GAP_PAGES,
      prefetchDepth: options.prefetchDepth ?? DEFAULT_PREFETCH_DEPTH,
      cache: new PageCache(maxPages, pageSize),
      inFlight: new InFlightMap(),
      fetchesIssued: 0,
      fetchRange: options.fetchRange ?? null,
      strategy: initialStrategy(hint, this.bytes.length),
      detector: new AccessPatternDetector(),
      eagerStarted: false,
      eagerComplete: false,
      eagerCancelled: false,
      eagerPromise: null,
      paused: false,
      destroyed: false,
      abortController,
      lifecycleHandlers: [],
    };
    if (options.autoLifecycle !== false && typeof globalThis.document !== "undefined") {
      this._wireLifecycle();
    }
  }

  /** Stop scheduling speculative prefetch (§8.1). */
  pausePrefetch() {
    if (this._prefetch) this._prefetch.paused = true;
  }

  /** Resume speculative prefetch after pause. */
  resumePrefetch() {
    if (this._prefetch) this._prefetch.paused = false;
  }

  _wireLifecycle() {
    const pf = this._prefetch;
    if (!pf || pf.lifecycleHandlers.length) return;
    const doc = globalThis.document;
    const win = globalThis.window;
    if (!doc || !win) return;
    const onVisibility = () => {
      if (doc.hidden) this.pausePrefetch();
      else this.resumePrefetch();
    };
    const onUnload = () => this.destroy();
    doc.addEventListener("visibilitychange", onVisibility);
    win.addEventListener("beforeunload", onUnload);
    pf.lifecycleHandlers.push(
      ["visibilitychange", onVisibility, doc],
      ["beforeunload", onUnload, win],
    );
  }

  _unwireLifecycle() {
    const pf = this._prefetch;
    if (!pf) return;
    for (const [event, fn, target] of pf.lifecycleHandlers) {
      target.removeEventListener(event, fn);
    }
    pf.lifecycleHandlers = [];
  }

  /** Cancel in-flight fetches and detach lifecycle hooks (§8.3). */
  destroy() {
    const pf = this._prefetch;
    if (!pf || pf.destroyed) return;
    pf.destroyed = true;
    pf.eagerCancelled = true;
    pf.abortController?.abort();
    this._unwireLifecycle();
  }

  _isEagerReady() {
    const pf = this._prefetch;
    return pf.strategy === "eager" && pf.eagerComplete;
  }

  _onAccess(index) {
    if (this._layout !== "row" || !this._prefetch) return;
    if (this.recordCount === 0) return;
    const pf = this._prefetch;
    if (pf.paused || pf.destroyed) return;
    pf.detector.observe(index);
    this._maybeUpgradeToEager();
    if (this._isEagerReady() || pf.strategy === "eager") return;
    const off = this._rowRecordOffset(index);
    if (off != null) {
      this._touchPage(Math.floor(off / pf.pageSize));
    }
    if (pf.strategy === "adaptive" && pf.detector.pattern() === PATTERN_SEQUENTIAL) {
      this._speculativePrefetch();
    }
  }

  _maybeUpgradeToEager() {
    const pf = this._prefetch;
    if (pf.paused || pf.destroyed || pf.strategy !== "adaptive") return;
    const det = pf.detector;
    if (det.pattern() !== PATTERN_SEQUENTIAL) return;
    if (det.sequentialRuns() < UPGRADE_SEQUENTIAL_THRESHOLD) return;
    const fileSizeMb = Math.floor(this.bytes.length / (1024 * 1024));
    if (fileSizeMb > EAGER_THRESHOLD_MB) return;
    pf.strategy = "eager";
    this._startEagerBackground();
  }

  _touchPage(pageIndex) {
    if (this._isEagerReady()) return;
    this._prefetch.cache.get(pageIndex);
  }

  _speculativePrefetch() {
    const pf = this._prefetch;
    if (pf.paused || pf.destroyed || pf.eagerCancelled) return;
    const predicted = pf.detector.predictNext(pf.prefetchDepth, this.recordCount);
    const pageSet = new Set();
    for (const idx of predicted) {
      const off = this._rowRecordOffset(idx);
      if (off != null) pageSet.add(Math.floor(off / pf.pageSize));
    }
    const missing = [...pageSet].filter((p) => !pf.cache.has(p) && !pf.inFlight.has(p));
    if (!missing.length) return;
    const ranges = clampRanges(
      coalescePageIndices(missing, pf.coalesceGapPages, pf.pageSize),
      this.bytes.length,
    );
    for (const range of ranges) {
      void this._startCoalescedRangeFetch(range).catch(() => {});
    }
  }

  _startEagerBackground() {
    const pf = this._prefetch;
    if (pf.eagerStarted) return;
    pf.eagerStarted = true;
    pf.eagerPromise = this._runEagerBackground();
  }

  async _runEagerBackground() {
    const pf = this._prefetch;
    const { byteStart, byteLength } = rowDataSector(this._tailStart, this.bytes.length);
    if (byteLength === 0) {
      pf.eagerComplete = true;
      return;
    }
    const end = Math.min(byteStart + byteLength, this.bytes.length);
    const pageSize = pf.pageSize;
    const firstPage = Math.floor(byteStart / pageSize);
    const lastPage = Math.floor((end - 1) / pageSize);
    const indices = [];
    for (let p = firstPage; p <= lastPage; p++) indices.push(p);
    const missing = indices.filter((p) => !pf.cache.has(p) && !pf.inFlight.has(p));
    if (!missing.length) {
      pf.eagerComplete = true;
      return;
    }
    const ranges = clampRanges(
      coalescePageIndices(missing, pf.coalesceGapPages, pageSize),
      this.bytes.length,
    );
    for (const range of ranges) {
      if (pf.eagerCancelled) return;
      await this._doCoalescedRangeFetch(range);
    }
    if (!pf.eagerCancelled) pf.eagerComplete = true;
  }

  /** Wait for in-progress eager / background prefetch (§8). */
  async warmup() {
    const p = this._prefetch?.eagerPromise;
    if (p) await p;
  }

  /** Cancel background eager prefetch and in-flight fetches (mirrors Rust Drop). */
  close() {
    this.destroy();
  }

  /** Absolute data-sector offset for row-layout record `i`. */
  _recordByteOffset(i) {
    if (this._tailView) {
      return Number(this._tailView.getBigUint64(4 + i * 10 + 2, true));
    }
    return rdU64AsNumber(this.bytes, this._tailStart + i * 10 + 2);
  }

  /**
   * Open a large remote `.nxb` via HTTP Range: header + tail resident, data sector on demand.
   * @param {string} url
   * @param {object} [options] — hint, maxPages, fetch, signal (fetchRange is built-in)
   */
  static async openRemote(url, options = {}) {
    const fetchImpl = options.fetch ?? globalThis.fetch;
    if (typeof fetchImpl !== "function") {
      throw new TypeError("NxsReader.openRemote requires fetch");
    }
    const head = await fetchImpl(url, { method: "HEAD", signal: options.signal });
    if (!head.ok) {
      throw new NxsError("ERR_OUT_OF_BOUNDS", `HEAD failed: ${head.status}`);
    }
    const fileSize = Number(head.headers.get("content-length"));
    if (!Number.isFinite(fileSize) || fileSize < 44) {
      throw new NxsError("ERR_OUT_OF_BOUNDS", "Content-Length required for openRemote");
    }

    const fetchRange = async (byteStart, byteLength, signal) => {
      const end = byteStart + byteLength - 1;
      const res = await fetchImpl(url, {
        headers: { Range: `bytes=${byteStart}-${end}` },
        signal: signal ?? options.signal,
      });
      if (!(res.status === 206 || (byteStart === 0 && res.ok))) {
        throw new NxsError("ERR_OUT_OF_BOUNDS", `range fetch failed: ${res.status}`);
      }
      return new Uint8Array(await res.arrayBuffer());
    };

    const probeLen = Math.min(4096, fileSize);
    const probe = await fetchRange(fileSize - probeLen, probeLen);
    const probeView = new DataView(probe.buffer, probe.byteOffset, probe.byteLength);
    if (probeView.getUint32(probe.byteLength - 4, true) !== MAGIC_FOOTER) {
      throw new NxsError("ERR_BAD_MAGIC", "footer magic mismatch");
    }
    let tailPtr = Number(probeView.getBigUint64(probe.byteLength - 12, true));
    const headerLen = Math.min(262144, tailPtr > 0 ? tailPtr : fileSize);
    const header = await fetchRange(0, headerLen);
    const hView = new DataView(header.buffer, header.byteOffset, header.byteLength);
    if (hView.getUint32(0, true) !== MAGIC_FILE) {
      throw new NxsError("ERR_BAD_MAGIC", "preamble");
    }
    const preambleTail = Number(hView.getBigUint64(16, true));
    if (preambleTail > 0) tailPtr = preambleTail;
    if (tailPtr <= 0 || tailPtr >= fileSize) {
      throw new NxsError("ERR_OUT_OF_BOUNDS", "invalid tail pointer");
    }
    const tail = await fetchRange(tailPtr, fileSize - tailPtr);
    const sparse = new SparseBytes(
      fileSize,
      [{ start: 0, data: header }, { start: tailPtr, data: tail }],
      fetchRange,
      options.pageSize,
    );
    return new NxsReader(sparse, { ...options, fetchRange });
  }

  /**
   * Open a `.nxb` from a URL (fetches the full file; use fetchRange option for tests).
   * @param {string} url
   * @param {object} [options] — hint, maxPages, fetch, signal, fetchRange
   */
  static async open(url, options = {}) {
    const fetchImpl = options.fetch ?? globalThis.fetch;
    if (typeof fetchImpl !== "function") {
      throw new TypeError("NxsReader.open requires fetch (browser/Node 18+) or options.fetch");
    }
    const res = await fetchImpl(url, { signal: options.signal });
    if (!res.ok) {
      throw new NxsError("ERR_OUT_OF_BOUNDS", `fetch failed: ${res.status}`);
    }
    const buf = await res.arrayBuffer();
    return new NxsReader(buf, options);
  }

  /**
   * Prefetch pages for records [startIndex, endIndex] (row layout). Resolves when cached.
   */
  async prefetch_viewport(startIndex, endIndex) {
    if (this._layout !== "row") return;
    if (startIndex < 0 || endIndex < startIndex || endIndex >= this.recordCount) {
      throw new NxsError(
        "ERR_OUT_OF_BOUNDS",
        `prefetch_viewport [${startIndex}, ${endIndex}] out of [0, ${this.recordCount})`,
      );
    }
    const { pageSize, cache, inFlight, coalesceGapPages } = this._prefetch;
    const indices = pageIndicesForViewport(
      startIndex,
      endIndex,
      pageSize,
      (i) => this._recordByteOffset(i),
    );
    const uniquePages = [...new Set(indices)];
    const missing = uniquePages.filter((p) => !cache.has(p) && !inFlight.has(p));

    if (missing.length) {
      const ranges = clampRanges(
        coalescePageIndices(missing, coalesceGapPages, pageSize),
        this.bytes.length,
      );
      const jobs = ranges.map((r) => this._startCoalescedRangeFetch(r));
      await Promise.all(jobs);
    }

    const pending = uniquePages
      .filter((p) => inFlight.has(p))
      .map((p) => inFlight.get(p));
    if (pending.length) await Promise.all(pending);

    cache.pinPages(uniquePages);
    cache.unpinAll();
  }

  /** Register in-flight page promises synchronously, then fetch the coalesced range. */
  _startCoalescedRangeFetch(range) {
    const { cache, inFlight } = this._prefetch;
    const pages = [];
    for (let p = range.pageStart; p <= range.pageEnd; p++) {
      if (!cache.has(p) && !inFlight.has(p)) pages.push(p);
    }
    if (!pages.length) {
      const waits = [];
      for (let p = range.pageStart; p <= range.pageEnd; p++) {
        const pending = inFlight.get(p);
        if (pending) waits.push(pending);
      }
      return Promise.all(waits);
    }

    const job = this._doCoalescedRangeFetch(range, { pinFetched: true });
    for (const p of pages) {
      inFlight.set(
        p,
        job.then(() => {
          const data = cache.get(p);
          if (!data) {
            throw new NxsError("ERR_OUT_OF_BOUNDS", `prefetch page ${p} missing after coalesced fetch`);
          }
          return data;
        }),
      );
    }
    return job;
  }

  async _doCoalescedRangeFetch(range, { pinFetched = false } = {}) {
    const { pageSize, cache } = this._prefetch;
    const blob = await this._fetchRangeBytes(range.byteStart, range.byteLength);
    if (this._sparse) {
      this._sparse.fillRange(range.byteStart, blob);
    }
    for (let p = range.pageStart; p <= range.pageEnd; p++) {
      if (cache.has(p)) continue;
      const pageOff = p * pageSize - range.byteStart;
      const pageLen = Math.min(pageSize, blob.byteLength - pageOff);
      if (pageLen <= 0) continue;
      cache.set(p, blob.subarray(pageOff, pageOff + pageLen), { pinned: pinFetched });
    }
  }

  async _fetchRangeBytes(byteStart, byteLength) {
    const pf = this._prefetch;
    if (pf.destroyed || pf.abortController?.signal.aborted) {
      throw new DOMException("Aborted", "AbortError");
    }
    pf.fetchesIssued++;
    const { fetchRange } = pf;
    if (fetchRange) {
      if (fetchRange.length >= 3) {
        return await fetchRange(byteStart, byteLength, pf.abortController.signal);
      }
      return await fetchRange(byteStart, byteLength);
    }
    return this.bytes.subarray(byteStart, byteStart + byteLength);
  }

  async _loadPage(pageIndex) {
    const { cache, inFlight, pageSize } = this._prefetch;
    const hit = cache.get(pageIndex);
    if (hit) return hit;
    const pending = inFlight.get(pageIndex);
    if (pending) {
      await pending;
      const data = cache.get(pageIndex);
      if (data) return data;
    }

    const job = (async () => {
      const byteStart = pageIndex * pageSize;
      const byteLength = Math.min(pageSize, this.bytes.length - byteStart);
      const data = await this._fetchRangeBytes(byteStart, byteLength);
      cache.set(pageIndex, data.slice());
      return data;
    })();
    inFlight.set(pageIndex, job);
    return job;
  }

  /** Diagnostic cache / prefetch counters. */
  cache_stats() {
    const s = this._prefetch.cache.stats();
    const out = {
      ...s,
      fetches_issued: this._prefetch.fetchesIssued,
      strategy: this._prefetch.strategy,
      pattern: this._prefetch.detector.pattern(),
      column_fetches_issued: this._colFetches,
    };
    return out;
  }

  /** `row` | `columnar` | `pax` */
  get layout() {
    return this._layout;
  }

  _readSchema(offset) {
    const keyCount = this.view.getUint16(offset, true);
    offset += 2;
    // TypeManifest
    for (let i = 0; i < keyCount; i++) {
      this.keySigils.push(byteAt(this.bytes, offset + i));
    }
    offset += keyCount;
    // StringPool — null-terminated UTF-8 strings
    for (let i = 0; i < keyCount; i++) {
      let end = offset;
      while (end < this.bytes.length && byteAt(this.bytes, end) !== 0) end++;
      this.keys.push(_decodeMaybeShared(this.bytes, offset, end));
      offset = end + 1;
    }
    // Build name→index map for O(1) lookup
    this.keyIndex = new Map();
    for (let i = 0; i < this.keys.length; i++) {
      this.keyIndex.set(this.keys[i], i);
    }
    // Schema ends at 8-byte alignment (caller doesn't need this)
    this._schemaEnd = (offset + 7) & ~7;
  }

  _readTailIndex() {
    if (this._tailView) {
      this.recordCount = this._tailView.getUint32(0, true);
      this._tailStart = this.tailPtr + 4;
      return;
    }
    let p = this.tailPtr;
    this.recordCount = this.view.getUint32(p, true);
    p += 4;
    // Record array: [KeyID u16][AbsoluteOffset u64] × N
    this._tailStart = p;
  }

  /** Row-layout absolute NYXO offset for record `index` (classic or delta tail-index). */
  _rowRecordOffset(index) {
    if (this._deltaTail) {
      return deltaRecordOffset(this.bytes, this._deltaTail, index);
    }
    return rdU64AsNumber(this.bytes, this._tailStart + index * 10 + 2);
  }

  _readColumnarFooter() {
    const fo = this.bytes.length - FOOTER_COL_BYTES;
    if (fo < 32) throw new NxsError("ERR_OUT_OF_BOUNDS", "columnar footer");
    this.tailPtr = Number(this.view.getBigUint64(fo, true));
    this.recordCount = Number(this.view.getBigUint64(fo + 8, true));
    const kc = this.keys.length;
    this._colBufOff = new Array(kc);
    this._colBufLen = new Array(kc);
    for (let i = 0; i < kc; i++) {
      const e = this.tailPtr + i * 20;
      const fid = this.view.getUint16(e, true);
      if (fid >= kc) {
        throw new NxsError("ERR_OUT_OF_BOUNDS", "invalid field ID in columnar tail");
      }
      this._colBufOff[fid] = Number(this.view.getBigUint64(e + 4, true));
      this._colBufLen[fid] = Number(this.view.getBigUint64(e + 12, true));
    }
    this._tailStart = this.tailPtr;
  }

  _readPaxFooter() {
    const fo = this.bytes.length - FOOTER_PAX_BYTES;
    if (fo < 32) throw new NxsError("ERR_OUT_OF_BOUNDS", "PAX footer");
    this.tailPtr = Number(this.view.getBigUint64(fo, true));
    this.recordCount = Number(this.view.getBigUint64(fo + 8, true));
    this.pageCount = this.view.getUint32(fo + 16, true);
    this.pageSizeHint = this.view.getUint32(fo + 20, true);
    this._tailStart = this.tailPtr;
    this._pageIndex = [];
    this._pageRecStart = [];
    this._pageRecCount = [];
    this._pageOffset = [];
    this._pageLength = [];
    for (let i = 0; i < this.pageCount; i++) {
      const e = this._tailStart + i * 28;
      this._pageIndex.push(this.view.getUint32(e, true));
      this._pageRecStart.push(Number(this.view.getBigUint64(e + 4, true)));
      this._pageRecCount.push(this.view.getUint32(e + 12, true));
      this._pageOffset.push(Number(this.view.getBigUint64(e + 16, true)));
      this._pageLength.push(this.view.getUint32(e + 24, true));
    }
    for (let i = 0; i < this.pageCount; i++) {
      const poff = this._pageOffset[i];
      if (poff > this.bytes.length - 4 || this.view.getUint32(poff, true) !== MAGIC_PAGE) {
        throw new NxsError("ERR_INVALID_PAGE_MAGIC", "PAX page magic mismatch");
      }
    }
  }

  _paxSumF64(slot) {
    if (slot < 0) throw new NxsError("ERR_KEY", "key not in schema");
    let sum = 0;
    for (let pi = 0; pi < this.pageCount; pi++) {
      const parts = this._pageFieldParts(pi, slot);
      if (!parts) continue;
      const { bitmap, values, pageRecCount: rc } = parts;
      for (let i = 0; i < rc; i++) {
        if (!this._colBit(bitmap, i)) continue;
        const off = i * 8;
        if (off + 8 <= values.length) sum += rdF64(values, off);
      }
    }
    return sum;
  }

  _paxFindPage(rec) {
    if (!this.pageCount) return null;
    let lo = 0;
    let hi = this.pageCount - 1;
    while (lo <= hi) {
      const mid = (lo + hi) >> 1;
      const start = this._pageRecStart[mid];
      const count = this._pageRecCount[mid];
      if (rec < start) hi = mid - 1;
      else if (rec >= start + count) lo = mid + 1;
      else return { page: mid, local: rec - start };
    }
    return null;
  }

  _isVarSigil(sig) {
    return isVarSigilByte(sig);
  }

  _fieldSectorLen(sectorOff, rc, sigil) {
    return fieldSectorLen(this.bytes, sectorOff, rc, sigil);
  }

  _pageFieldSector(pageIndex, slot) {
    const poff = this._pageOffset[pageIndex];
    if (poff + 24 > this.bytes.length) return null;
    if (this.view.getUint32(poff, true) !== MAGIC_PAGE) return null;
    const fieldCount = this.view.getUint16(poff + 20, true);
    if (slot < 0 || slot >= fieldCount || fieldCount > this.keySigils.length) return null;
    const rc = this._pageRecCount[pageIndex];
    let body = poff + 24;
    for (let fi = 0; fi < slot; fi++) {
      const sig = this.keySigils[fi] ?? SIGIL_INT;
      const flen = this._fieldSectorLen(body, rc, sig);
      if (flen < 0) return null;
      body += flen;
    }
    const sig = this.keySigils[slot] ?? SIGIL_INT;
    const flen = this._fieldSectorLen(body, rc, sig);
    if (flen < 0 || body + flen > this.bytes.length) return null;
    return this.bytes.subarray(body, body + flen);
  }

  _pageFieldParts(pageIndex, slot) {
    const sector = this._pageFieldSector(pageIndex, slot);
    if (!sector) return null;
    const rc = this._pageRecCount[pageIndex];
    const bmLen = this._nullBitmapBytes(rc);
    return {
      bitmap: sector.subarray(0, bmLen),
      values: sector.subarray(bmLen),
      pageRecCount: rc,
    };
  }

  _colVarParts(slot) {
    const { bitmap, values } = this._colFieldParts(slot);
    const offBytes = varOffBytes(this.recordCount);
    if (offBytes < 0 || values.length < offBytes) {
      throw new NxsError("ERR_OUT_OF_BOUNDS", "var offsets");
    }
    return {
      bitmap,
      offsets: values.subarray(0, offBytes),
      values: values.subarray(offBytes),
    };
  }

  _colVarPartsAt(rec, slot) {
    if (slot < 0 || !this._isVarSigil(this.keySigils[slot])) return null;
    if (this._layout === "columnar") return this._colVarParts(slot);
    if (this._layout === "pax") {
      const loc = this._paxFindPage(rec);
      if (!loc) return null;
      const parts = this._pageFieldParts(loc.page, slot);
      if (!parts) return null;
      const rc = parts.pageRecCount;
      const offBytes = varOffBytes(rc);
      if (offBytes < 0 || parts.values.length < offBytes) return null;
      return {
        bitmap: parts.bitmap,
        offsets: parts.values.subarray(0, offBytes),
        values: parts.values.subarray(offBytes),
        local: loc.local,
      };
    }
    return null;
  }

  _varStrAt(offsets, values, recordIndex) {
    const need = varNeedBytes(recordIndex);
    if (need < 0 || offsets.length < need) return null;
    const ri = recordIndex >>> 0;
    const start = rdU32(offsets, ri * 4);
    const end = rdU32(offsets, ri * 4 + 4);
    if (end < start || end > values.length) return null;
    return decodeUtf8Fast(values, start, end - start);
  }

  _varBinaryAt(offsets, values, recordIndex) {
    const need = varNeedBytes(recordIndex);
    if (need < 0 || offsets.length < need) return null;
    const ri = recordIndex >>> 0;
    const start = rdU32(offsets, ri * 4);
    const end = rdU32(offsets, ri * 4 + 4);
    if (end < start || end > values.length) return null;
    return values.subarray(start, end);
  }

  /** UTF-8 string at `recordIndex` in a columnar/PAX column (null → null). */
  colGetStr(key, recordIndex) {
    const slot = this._slotOf(key);
    if (slot < 0) throw new NxsError("ERR_KEY", `key ${key} not in schema`);
    if (this.keySigils[slot] !== SIGIL_STR) return null;
    const parts = this._colVarPartsAt(recordIndex, slot);
    if (!parts) return null;
    const bitIdx = this._layout === "pax" ? parts.local : recordIndex;
    if (!this._colBit(parts.bitmap, bitIdx)) return null;
    return this._varStrAt(parts.offsets, parts.values, bitIdx);
  }

  /** Binary blob at `recordIndex` in a columnar/PAX column (null → null). */
  colGetBinary(key, recordIndex) {
    const slot = this._slotOf(key);
    if (slot < 0) throw new NxsError("ERR_KEY", `key ${key} not in schema`);
    if (this.keySigils[slot] !== SIGIL_BINARY) return null;
    const parts = this._colVarPartsAt(recordIndex, slot);
    if (!parts) return null;
    const bitIdx = this._layout === "pax" ? parts.local : recordIndex;
    if (!this._colBit(parts.bitmap, bitIdx)) return null;
    return this._varBinaryAt(parts.offsets, parts.values, bitIdx);
  }

  _colNumericBytes(rec, slot) {
    if (slot >= 0 && this._isVarSigil(this.keySigils[slot])) return null;
    if (this._layout === "columnar") {
      const { bitmap, values } = this._colFieldParts(slot);
      if (!this._colBit(bitmap, rec)) return null;
      const off = rec * 8;
      if (off + 8 > values.length) return null;
      return values.subarray(off, off + 8);
    }
    if (this._layout === "pax") {
      const loc = this._paxFindPage(rec);
      if (!loc) return null;
      const parts = this._pageFieldParts(loc.page, slot);
      if (!parts || !this._colBit(parts.bitmap, loc.local)) return null;
      const off = loc.local * 8;
      if (off + 8 > parts.values.length) return null;
      return parts.values.subarray(off, off + 8);
    }
    return null;
  }

  _nullBitmapBytes(n) {
    const raw = Math.ceil(n / 8);
    return (raw + 7) & ~7;
  }

  _colFieldParts(slot) {
    const off = this._colBufOff[slot];
    const len = this._colBufLen[slot];
    if (off + len > this.bytes.length) {
      throw new NxsError("ERR_OUT_OF_BOUNDS", "column sector");
    }
    const bmLen = this._nullBitmapBytes(this.recordCount);
    if (len < bmLen) throw new NxsError("ERR_OUT_OF_BOUNDS", "column sector short");
    const sector = this._colOverlay.has(slot)
      ? this._colOverlay.get(slot)
      : this.bytes.subarray(off, off + len);
    return {
      bitmap: sector.subarray(0, bmLen),
      values: sector.subarray(bmLen),
    };
  }

  _colBit(bitmap, rec) {
    return ((bitmap[rec >> 3] >> (rec & 7)) & 1) === 1;
  }

  /**
   * Raw value bytes for a columnar/PAX field (dense numeric tail only).
   * @returns {{ values: Uint8Array, bitmap: Uint8Array, count: number }}
   */
  colBuffer(key) {
    if (this.layout !== "columnar" && this.layout !== "pax") {
      throw new NxsError("ERR_LAYOUT", "colBuffer requires columnar or PAX layout");
    }
    const slot = this._slotOf(key);
    if (slot < 0) throw new NxsError("ERR_KEY", `key ${key} not in schema`);
    if (this._isVarSigil(this.keySigils[slot])) {
      throw new NxsError("ERR_UNSUPPORTED_FIELD_TYPE", "use colVarBuffer for string/binary columns");
    }
    const { bitmap, values } = this._colFieldParts(slot);
    return { values, bitmap, count: this.recordCount };
  }

  /**
   * Zero-copy string/binary column: null bitmap + u32 offsets + values blob.
   * @returns {{ bitmap: Uint8Array, offsets: Uint8Array, values: Uint8Array, count: number }}
   */
  colVarBuffer(key) {
    if (this.layout !== "columnar") {
      throw new NxsError("ERR_LAYOUT", "colVarBuffer is columnar-only (use colGetStr per record on PAX)");
    }
    const slot = this._slotOf(key);
    if (slot < 0) throw new NxsError("ERR_KEY", `key ${key} not in schema`);
    if (!this._isVarSigil(this.keySigils[slot])) {
      throw new NxsError("ERR_UNSUPPORTED_FIELD_TYPE", "colVarBuffer requires string or binary field");
    }
    const { bitmap, offsets, values } = this._colVarParts(slot);
    return { bitmap, offsets, values, count: this.recordCount };
  }

  /** f64 at `recordIndex` in a columnar column (null → null). */
  colGetF64(key, recordIndex) {
    const slot = this._slotOf(key);
    if (slot < 0) throw new NxsError("ERR_KEY", `key ${key} not in schema`);
    const cell = this._colNumericBytes(recordIndex, slot);
    if (!cell) return null;
    return rdF64(cell, 0);
  }

  /**
   * Sum a columnar f64 column (dense fast path when bitmap is all-ones).
   */
  colSumF64(key) {
    if (this.layout === "pax") {
      return this._paxSumF64(this._slotOf(key));
    }
    if (this.layout !== "columnar") {
      return this.sumF64(key);
    }
    const slot = this._slotOf(key);
    if (slot < 0) throw new NxsError("ERR_KEY", `key ${key} not in schema`);
    const { bitmap, values } = this._colFieldParts(slot);
    const n = this.recordCount;
    if (this._nullBitmapDense(bitmap, n)) {
      return this._sumF64DenseColumn(values, n);
    }
    let sum = 0;
    for (let i = 0; i < n; i++) {
      if (!this._colBit(bitmap, i)) continue;
      const off = i * 8;
      if (off + 8 <= values.length) sum += rdF64(values, off);
    }
    return sum;
  }

  _nullBitmapDense(bitmap, n) {
    if (n === 0) return true;
    const full = (n / 8) | 0;
    for (let i = 0; i < full; i++) {
      if (bitmap[i] !== 0xff) return false;
    }
    const rem = n % 8;
    if (rem === 0) return true;
    const mask = (1 << rem) - 1;
    return (bitmap[full] & mask) === mask;
  }

  _sumF64DenseColumn(values, n) {
    let i = 0;
    let a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    const end4 = n - (n % 4);
    for (; i < end4; i += 4) {
      a0 += rdF64(values, i * 8);
      a1 += rdF64(values, (i + 1) * 8);
      a2 += rdF64(values, (i + 2) * 8);
      a3 += rdF64(values, (i + 3) * 8);
    }
    let sum = a0 + a1 + a2 + a3;
    for (; i < n; i++) sum += rdF64(values, i * 8);
    return sum;
  }

  /**
   * O(1) lookup: get the object at top-level index `i`.
   * Returns an NxsObject view — nothing is decoded until you call .get().
   */
  record(i) {
    if (i < 0 || i >= this.recordCount) {
      throw new NxsError("ERR_OUT_OF_BOUNDS", `record ${i} out of [0, ${this.recordCount})`);
    }
    if (this._layout === "columnar" || this._layout === "pax") {
      return new NxsObject(this, i, i);
    }
    this._onAccess(i);
    const absOffset = this._rowRecordOffset(i);
    if (absOffset == null) {
      throw new NxsError("ERR_OUT_OF_BOUNDS", `record ${i} offset out of bounds`);
    }
    return new NxsObject(this, absOffset);
  }

  /**
   * Returns a reusable cursor positioned initially at record 0.
   * Call `cursor.seek(i)` to move to record i without allocating.
   * Zero allocation per record → much faster full scans.
   */
  cursor() {
    return new NxsCursor(this);
  }

  /**
   * Efficient per-record scan with a reused cursor. Calls fn(cursor, i) for
   * each record. Identical in output to iterating records() but with no
   * per-record object allocation.
   */
  scan(fn) {
    const cur = new NxsCursor(this);
    const n = this.recordCount;
    for (let i = 0; i < n; i++) {
      cur._reset(this._rowRecordOffset(i));
      fn(cur, i);
    }
  }

  /**
   * Attach a loaded WASM module to accelerate reducers. The reader copies its
   * bytes into WASM memory once; subsequent sumF64/minF64/maxF64 calls run
   * in WASM. If not called, those methods use the pure-JS implementation.
   *
   * The WASM module is loaded via `loadWasm()` from `./wasm.js`.
   * See `./wasm/README.md` for build source, toolchain, and rebuild instructions.
   */
  useWasm(wasm) {
    this._wasm = wasm;
    wasm.loadPayload(this.bytes);
    // tail_start / record_count are the same across JS and WASM views because
    // we copied the whole file starting at `wasm.dataBase`.
    this._wasmTailStart = this._tailStart;
    this._wasmDataBase = wasm.dataBase;
    this._wasmSize = this.bytes.length;
  }

  /** Iterate all top-level records. */
  *records() {
    for (let i = 0; i < this.recordCount; i++) {
      yield this.record(i);
    }
  }

  /**
   * Resolve a key name to a slot index (integer). Use this once, then pass the
   * slot to `obj.getStrBySlot(slot)` etc. to skip the Map lookup on hot paths.
   */
  slot(key) {
    const s = this.keyIndex.get(key);
    if (s === undefined) throw new NxsError("ERR_KEY", `key ${key} not in schema`);
    return s;
  }

  /**
   * Inspect record 0 to find where `slot`'s value lives in every uniform record.
   * @returns {{ bitmaskLen: number, tableIdx: number, present: boolean }}
   */
  _computeFastLayout(slot) {
    if (this.recordCount === 0) {
      return { bitmaskLen: 0, tableIdx: 0, present: false };
    }
    const bytes = this.bytes;
    const abs = rdU64AsNumber(bytes, this._tailStart + 2);
    let p = abs + 8;
    const bitmaskStart = p;
    let curSlot = 0;
    let tableIdx = 0;
    let present = false;
    for (;;) {
      const b = bytes[p++];
      const bits = b & 0x7F;
      for (let i = 0; i < 7; i++) {
        if (curSlot === slot) present = ((bits >> i) & 1) === 1;
        else if (curSlot < slot && ((bits >> i) & 1)) tableIdx++;
        curSlot++;
      }
      if ((b & 0x80) === 0) break;
    }
    return { bitmaskLen: p - bitmaskStart, tableIdx, present };
  }

  /**
   * True when every record shares the same bitmask bytes as record 0.
   * O(n); only worth calling before many fast/indexed scans.
   */
  isUniform() {
    const n = this.recordCount;
    if (n === 0) return true;
    const bytes = this.bytes;
    const abs0 = rdU64AsNumber(bytes, this._tailStart + 2);
    let p = abs0 + 8;
    const start = p;
    for (;;) {
      const b = bytes[p++];
      if ((b & 0x80) === 0) break;
    }
    const mask = bytes.subarray(start, p);
    for (let i = 1; i < n; i++) {
      const abs = rdU64AsNumber(bytes, this._tailStart + i * 10 + 2);
      const other = bytes.subarray(abs + 8, abs + 8 + mask.length);
      if (other.length !== mask.length) return false;
      for (let j = 0; j < mask.length; j++) {
        if (other[j] !== mask[j]) return false;
      }
    }
    return true;
  }

  /**
   * One O(n) pass: absolute byte offset of `key`'s value per record.
   * Random access becomes `index.getStrAt(k)` with no bitmask walk.
   * @returns {NxsFieldIndex | null}
   */
  buildFieldIndex(key) {
    const slot = this._slotOf(key);
    if (slot < 0) throw new NxsError("ERR_KEY", `key ${key} not in schema`);
    if (this._wasm?.fns.build_field_index) {
      return this._buildFieldIndexWasm(slot);
    }
    return this._buildFieldIndexJs(slot);
  }

  _buildFieldIndexJs(slot) {
    const layout = this._computeFastLayout(slot);
    if (!layout.present) return null;
    const n = this.recordCount;
    const offsets = new Uint32Array(n);
    const bytes = this.bytes;
    const tailStart = this._tailStart;
    const offsetTablePos = 8 + layout.bitmaskLen + layout.tableIdx * 2;
    for (let i = 0; i < n; i++) {
      const abs = rdU64AsNumber(bytes, tailStart + i * 10 + 2);
      offsets[i] = abs + rdU16(bytes, abs + offsetTablePos);
    }
    return new NxsFieldIndex(this, slot, offsets);
  }

  _buildFieldIndexWasm(slot) {
    const n = this.recordCount;
    const wasm = this._wasm;
    const outPtr = wasm._allocScratch(n * 4);
    const written = wasm.fns.build_field_index(
      this._wasmDataBase, this._wasmSize,
      this._wasmTailStart,
      n, slot, outPtr);
    if (!written) return this._buildFieldIndexJs(slot);
    const offsets = new Uint32Array(wasm.memory.buffer, outPtr, n).slice();
    return new NxsFieldIndex(this, slot, offsets);
  }

  /**
   * Resolve value offsets for many record indices in one WASM call (strings
   * still decode in JS). Falls back to per-index JS resolve when WASM absent.
   * @param {number} slot — from reader.slot(key)
   * @param {Uint32Array | number[]} recordIndices
   * @returns {Uint32Array} absolute value offsets; `0xFFFFFFFF` = absent
   */
  batchResolveOffsets(slot, recordIndices) {
    const count = recordIndices.length;
    if (this._wasm?.fns.batch_resolve_offsets) {
      return this._batchResolveOffsetsWasm(slot, recordIndices);
    }
    const out = new Uint32Array(count);
    const cur = this.cursor();
    for (let j = 0; j < count; j++) {
      cur.seek(recordIndices[j]);
      const off = cur._resolveSlot(slot);
      out[j] = off < 0 ? 0xFFFFFFFF : off >>> 0;
    }
    return out;
  }

  _batchResolveOffsetsWasm(slot, recordIndices) {
    const wasm = this._wasm;
    const count = recordIndices.length;
    const idxPtr = wasm._allocScratch(count * 4);
    const outPtr = wasm._allocScratch(count * 4);
    const idxView = new Uint32Array(wasm.memory.buffer, idxPtr, count);
    for (let j = 0; j < count; j++) idxView[j] = recordIndices[j] >>> 0;
    wasm.fns.batch_resolve_offsets(
      this._wasmDataBase, this._wasmSize,
      this._wasmTailStart,
      this.recordCount, slot, idxPtr, count, outPtr);
    return new Uint32Array(wasm.memory.buffer, outPtr, count).slice();
  }

  // ── Bulk columnar scan ──────────────────────────────────────────────────
  //
  // These methods walk every record in a single JS-native loop that inlines
  // the LEB128 bitmask and offset-table reads, avoiding the per-record
  // allocation and indirection of `record(i).getF64(key)`.

  /** Returns the slot index for `key`, or -1 if not present. */
  _slotOf(key) {
    const idx = this.keyIndex.get(key);
    return idx === undefined ? -1 : idx;
  }

  /**
   * Inline scan primitive: for each record, locate the byte offset of `slot`'s
   * value and invoke `decode(dataView, offset)` to extract it.
   * Returns an array of length recordCount, with nulls where the field is absent.
   */
  _scanLoop(slot, decode) {
    const n = this.recordCount;
    const view = this.view;
    const bytes = this.bytes;
    const tailStart = this._tailStart;
    const size = bytes.length;
    const out = new Array(n);

    for (let i = 0; i < n; i++) {
      // Read u64 absOffset (two u32s avoids BigInt allocation)
      const entryOff = tailStart + i * 10 + 2;
      const lo = view.getUint32(entryOff, true);
      const hi = view.getUint32(entryOff + 4, true);
      const absOffset = hi * 0x100000000 + lo;

      let p = absOffset + 8; // skip NYXO magic + length

      // Walk LEB128 bitmask, counting present bits before slot
      let curSlot = 0;
      let tableIdx = 0;
      let found = false;
      let done = false;
      let byte = 0;
      while (!done) {
        if (p >= size) { out[i] = null; break; }
        byte = bytes[p++];
        const dataBits = byte & 0x7F;
        for (let b = 0; b < 7; b++) {
          if (curSlot === slot) {
            if ((dataBits >> b) & 1) { found = true; }
            else { out[i] = null; done = true; break; }
          } else if (curSlot < slot && ((dataBits >> b) & 1)) {
            tableIdx++;
          }
          curSlot++;
        }
        if (done) break;
        if (found && (byte & 0x80) === 0) break;
        if (curSlot > slot && found) break;
        if ((byte & 0x80) === 0) { out[i] = null; done = true; }
      }
      if (done) continue;
      if (!found) { out[i] = null; continue; }

      // If we stopped mid-mask, skip remaining continuation bytes
      while (byte & 0x80) {
        if (p >= size) break;
        byte = bytes[p++];
      }

      const ofPos = p + tableIdx * 2;
      const relOff = view.getUint16(ofPos, true);
      out[i] = decode(view, absOffset + relOff);
    }
    return out;
  }

  /** Scan all f64 values for `key`. Returns Array<number | null>. */
  scanF64(key) {
    const slot = this._slotOf(key);
    if (slot < 0) throw new NxsError("ERR_KEY", `key ${key} not in schema`);
    return this._scanLoop(slot, (v, off) => v.getFloat64(off, true));
  }

  /** Scan all i64 values. Returns Array<number | null> (safe when |v| < 2^53). */
  scanI64(key) {
    const slot = this._slotOf(key);
    if (slot < 0) throw new NxsError("ERR_KEY", `key ${key} not in schema`);
    return this._scanLoop(slot, (v, off) => Number(v.getBigInt64(off, true)));
  }

  /**
   * In-native reducer: sum of all f64 values for `key`.
   * Zero intermediate allocation — returns a single number.
   */
  sumF64(key) {
    if (this.layout === "columnar" || this.layout === "pax") {
      return this.colSumF64(key);
    }
    const slot = this._slotOf(key);
    if (slot < 0) throw new NxsError("ERR_KEY", `key ${key} not in schema`);
    if (this._wasm) {
      return this._wasm.fns.sum_f64(
        this._wasmDataBase, this._wasmSize,
        this._wasmTailStart,
        this.recordCount, slot);
    }
    return this._sumF64Js(slot);
  }

  /** Min over f64 field (returns null if no records). */
  minF64(key) {
    const slot = this._slotOf(key);
    if (slot < 0) throw new NxsError("ERR_KEY", `key ${key} not in schema`);
    if (this._wasm) {
      const v = this._wasm.fns.min_f64(
        this._wasmDataBase, this._wasmSize,
        this._wasmTailStart,
        this.recordCount, slot);
      return this._wasm.fns.min_max_has_result() ? v : null;
    }
    const arr = this.scanF64(key);
    let m = Infinity, have = false;
    for (const v of arr) { if (v !== null && v < m) { m = v; have = true; } }
    return have ? m : null;
  }

  /** Max over f64 field. */
  maxF64(key) {
    const slot = this._slotOf(key);
    if (slot < 0) throw new NxsError("ERR_KEY", `key ${key} not in schema`);
    if (this._wasm) {
      const v = this._wasm.fns.max_f64(
        this._wasmDataBase, this._wasmSize,
        this._wasmTailStart,
        this.recordCount, slot);
      return this._wasm.fns.min_max_has_result() ? v : null;
    }
    const arr = this.scanF64(key);
    let m = -Infinity, have = false;
    for (const v of arr) { if (v !== null && v > m) { m = v; have = true; } }
    return have ? m : null;
  }

  /** Sum over i64 field. */
  sumI64(key) {
    const slot = this._slotOf(key);
    if (slot < 0) throw new NxsError("ERR_KEY", `key ${key} not in schema`);
    if (this._wasm) {
      // WASM returns i64 which Node surfaces as BigInt; convert to Number
      // (safe for the fixture dataset).
      return Number(this._wasm.fns.sum_i64(
        this._wasmDataBase, this._wasmSize,
        this._wasmTailStart,
        this.recordCount, slot));
    }
    const arr = this.scanI64(key);
    let s = 0;
    for (const v of arr) if (v !== null) s += v;
    return s;
  }

  /** Pure-JS sumF64 fallback. */
  _sumF64Js(slot) {
    const n = this.recordCount;
    const bytes = this.bytes;
    const tailStart = this._tailStart;
    const size = bytes.length;
    let sum = 0;

    for (let i = 0; i < n; i++) {
      const absOffset = rdU64AsNumber(bytes, tailStart + i * 10 + 2);

      let p = absOffset + 8;
      let curSlot = 0;
      let tableIdx = 0;
      let found = false;
      let byte = 0;
      let brokeEarly = false;
      while (true) {
        if (p >= size) break;
        byte = bytes[p++];
        const dataBits = byte & 0x7F;
        for (let b = 0; b < 7; b++) {
          if (curSlot === slot) {
            if ((dataBits >> b) & 1) found = true;
            brokeEarly = true;
            break;
          } else if (curSlot < slot && ((dataBits >> b) & 1)) {
            tableIdx++;
          }
          curSlot++;
        }
        if (brokeEarly) break;
        if ((byte & 0x80) === 0) break;
      }
      if (!found) continue;
      while (byte & 0x80) {
        if (p >= size) break;
        byte = bytes[p++];
      }
      sum += rdF64(bytes, absOffset + rdU16(bytes, p + tableIdx * 2));
    }
    return sum;
  }
}

// ── PAX streaming reader (OLAP.md §4.5) ────────────────────────────────────

/**
 * Returns 8-byte-aligned page length when NXSP page at `off` is complete, else 0.
 * @param {Uint8Array} bytes
 * @param {number} off - byte offset of the page start
 * @param {number} fieldCount - number of columns
 * @param {ArrayLike<number>} sigils - per-field sigil bytes (length >= fieldCount)
 */
export function paxCompletePageAt(bytes, off, fieldCount, sigils) {
  if (off + 28 > bytes.length || fieldCount === 0) return 0;
  if (rdU32(bytes, off) !== MAGIC_PAGE) return 0;
  const rc = rdU32(bytes, off + 16);
  // Walk each field sector to compute the true page body length.
  let body = off + 24;
  for (let fi = 0; fi < fieldCount; fi++) {
    const sig = (sigils && fi < sigils.length) ? sigils[fi] : SIGIL_INT;
    const flen = fieldSectorLen(bytes, body, rc, sig);
    if (flen < 0) return 0;
    body += flen;
  }
  // body now points to the 4-byte page-length trailer.
  const pageLen = body - off + 4;
  const aligned = (pageLen + 7) & ~7;
  if (off + pageLen > bytes.length) return 0;
  if (rdU32(bytes, off + pageLen - 4) !== pageLen) return 0;
  if (off + aligned > bytes.length) return 0;
  return aligned;
}

export class NxsPaxStreamReader {
  constructor({ onPage, onSealed, onError } = {}) {
    this.onPage = onPage;
    this.onSealed = onSealed;
    this.onError = onError;
    this.bytes = new Uint8Array(0);
    this._buffer = new Uint8Array(0);
    this._length = 0;
    this.keys = [];
    this.keySigils = [];
    this.keyIndex = new Map();
    this._dataStart = 0;
    this._scanCursor = 0;
    this._headerParsed = false;
    this.sealed = false;
    this.pageCount = 0;
    this.recordsAvailable = 0;
    this._pageIndex = [];
    this._pageRecStart = [];
    this._pageRecCount = [];
    this._pageOffset = [];
    this._pageLength = [];
  }

  push(chunk) {
    try {
      const incoming = chunk instanceof Uint8Array ? chunk : new Uint8Array(chunk);
      this._ensureCapacity(this._length + incoming.length);
      this._buffer.set(incoming, this._length);
      this._length += incoming.length;
      this.bytes = this._buffer.subarray(0, this._length);
      this.view = new DataView(this.bytes.buffer, this.bytes.byteOffset, this.bytes.byteLength);
      this._parseAvailable();
    } catch (err) {
      this.onError?.(err);
      throw err;
    }
  }

  /** @returns {NxsReader} sealed random-access reader */
  finish() {
    this._parseAvailable();
    return new NxsReader(this.bytes);
  }

  completePageAt(off) {
    if (!this._headerParsed) return 0;
    return paxCompletePageAt(this.bytes, off, this.keys.length, this.keySigils);
  }

  colSumF64(key) {
    const slot = this.keyIndex.get(key);
    if (slot === undefined) throw new NxsError("ERR_KEY", `key ${key} not in schema`);
    let sum = 0;
    for (let pi = 0; pi < this.pageCount; pi++) {
      const parts = this._pageFieldParts(pi, slot);
      if (!parts) continue;
      const { bitmap, values, pageRecCount: rc } = parts;
      for (let i = 0; i < rc; i++) {
        if (!this._colBit(bitmap, i)) continue;
        const cellOff = i * 8;
        if (cellOff + 8 <= values.length) sum += rdF64(values, cellOff);
      }
    }
    return sum;
  }

  _pageFieldParts(pageIndex, slot) {
    const sector = this._paxPageFieldSector(pageIndex, slot);
    if (!sector) return null;
    const rc = this._pageRecCount[pageIndex];
    const bmLen = this._nullBitmapBytes(rc);
    return {
      bitmap: sector.subarray(0, bmLen),
      values: sector.subarray(bmLen),
      pageRecCount: rc,
    };
  }

  _paxPageFieldSector(pageIndex, slot) {
    const poff = this._pageOffset[pageIndex];
    if (poff + 24 > this.bytes.length) return null;
    if (rdU32(this.bytes, poff) !== MAGIC_PAGE) return null;
    const fieldCount = this.view.getUint16(poff + 20, true);
    if (slot < 0 || slot >= fieldCount || fieldCount > this.keySigils.length) return null;
    const rc = this._pageRecCount[pageIndex];
    let body = poff + 24;
    for (let fi = 0; fi < slot; fi++) {
      const sig = this.keySigils[fi] ?? SIGIL_INT;
      const flen = fieldSectorLen(this.bytes, body, rc, sig);
      if (flen < 0) return null;
      body += flen;
    }
    const sig = this.keySigils[slot] ?? SIGIL_INT;
    const flen = fieldSectorLen(this.bytes, body, rc, sig);
    if (flen < 0 || body + flen > this.bytes.length) return null;
    return this.bytes.subarray(body, body + flen);
  }

  _nullBitmapBytes(n) {
    return nullBitmapBytesLen(n);
  }

  _colBit(bitmap, rec) {
    return ((bitmap[rec >> 3] >> (rec & 7)) & 1) === 1;
  }

  _parseAvailable() {
    if (!this._headerParsed && !this._parseHeader()) return;
    if (!this.sealed && this._detectSealed()) return;
    if (this.sealed) return;
    const fc = this.keys.length;
    while (this._scanCursor + 28 <= this.bytes.length) {
      if (rdU32(this.bytes, this._scanCursor) !== MAGIC_PAGE) break;
      const plen = paxCompletePageAt(this.bytes, this._scanCursor, fc, this.keySigils);
      if (plen === 0) break;
      const pidx = rdU32(this.bytes, this._scanCursor + 4);
      const rstart = Number(this.view.getBigUint64(this._scanCursor + 8, true));
      const rc = rdU32(this.bytes, this._scanCursor + 16);
      this._pageIndex.push(pidx);
      this._pageRecStart.push(rstart);
      this._pageRecCount.push(rc);
      this._pageOffset.push(this._scanCursor);
      this._pageLength.push(plen);
      this.pageCount++;
      this.recordsAvailable += rc;
      this.onPage?.(this.pageCount - 1, rc);
      this._scanCursor += plen;
    }
  }

  _detectSealed() {
    if (this.bytes.length < FOOTER_PAX_BYTES) return false;
    if (rdU32(this.bytes, this.bytes.length - 4) !== MAGIC_FOOTER) return false;
    const tailOff = Number(this.view.getBigUint64(this.bytes.length - FOOTER_PAX_BYTES, true));
    if (tailOff === 0 || tailOff >= this.bytes.length ||
        this.bytes.length - tailOff < FOOTER_PAX_BYTES) {
      return false;
    }
    try {
      this._loadSealedTail(tailOff);
    } catch (err) {
      this.onError?.(err);
      return false;
    }
    this.sealed = true;
    this.onSealed?.();
    return true;
  }

  _loadSealedTail(tailOff) {
    const fo = this.bytes.length - FOOTER_PAX_BYTES;
    this.pageCount = this.view.getUint32(fo + 16, true);
    this.recordsAvailable = Number(this.view.getBigUint64(fo + 8, true));
    this._pageIndex = [];
    this._pageRecStart = [];
    this._pageRecCount = [];
    this._pageOffset = [];
    this._pageLength = [];
    for (let i = 0; i < this.pageCount; i++) {
      const e = tailOff + i * 28;
      if (e + 28 > this.bytes.length) {
        throw new NxsError("ERR_OUT_OF_BOUNDS", "PAX tail entry incomplete");
      }
      this._pageIndex.push(rdU32(this.bytes, e));
      this._pageRecStart.push(Number(this.view.getBigUint64(e + 4, true)));
      this._pageRecCount.push(rdU32(this.bytes, e + 12));
      this._pageOffset.push(Number(this.view.getBigUint64(e + 16, true)));
      this._pageLength.push(rdU32(this.bytes, e + 24));
    }
    this._scanCursor = this.bytes.length;
  }

  _parseHeader() {
    if (this.bytes.length < 34) return false;
    if (rdU32(this.bytes, 0) !== MAGIC_FILE) {
      throw new NxsError("ERR_BAD_MAGIC", "preamble");
    }
    this.version = this.view.getUint16(4, true);
    this.flags = this.view.getUint16(6, true);
    if ((this.flags & FLAG_PAX) === 0) {
      throw new NxsError("ERR_INVALID_FLAGS", "PAX stream requires FLAG_PAX");
    }
    if (Number(this.view.getBigUint64(16, true)) !== 0) {
      throw new NxsError("ERR_INCOMPATIBLE_FLAGS", "PAX stream requires TailPtr=0");
    }
    this.dictHash = this.view.getBigUint64(8, true);
    if ((this.flags & FLAG_SCHEMA_EMBEDDED) === 0) {
      throw new NxsError("ERR_DICT_MISMATCH", "PAX stream requires embedded schema");
    }
    const keyCount = this.view.getUint16(32, true);
    let offset = 34 + keyCount;
    if (this.bytes.length < offset) return false;
    this.keySigils = Array.from(this.bytes.subarray(34, 34 + keyCount));
    const keys = [];
    for (let i = 0; i < keyCount; i++) {
      let end = offset;
      while (end < this.bytes.length && byteAt(this.bytes, end) !== 0) end++;
      if (end >= this.bytes.length) return false;
      keys.push(_decodeMaybeShared(this.bytes, offset, end));
      offset = end + 1;
    }
    this.keys = keys;
    this.keyIndex = new Map(keys.map((key, index) => [key, index]));
    this._dataStart = (offset + 7) & ~7;
    if (this.bytes.length < this._dataStart) return false;
    const computedHash = murmur3_64(this.bytes, 32, this._dataStart - 32);
    if (computedHash !== this.dictHash) {
      throw new NxsError("ERR_DICT_MISMATCH", "schema hash mismatch");
    }
    this._scanCursor = this._dataStart;
    this._headerParsed = true;
    return true;
  }

  _ensureCapacity(required) {
    if (required <= this._buffer.length) return;
    let nextCap = this._buffer.length || 4096;
    while (nextCap < required) nextCap *= 2;
    const next = new Uint8Array(nextCap);
    next.set(this.bytes);
    this._buffer = next;
  }
}

// ── Row streaming reader ───────────────────────────────────────────────────

const STREAM_BUF_MAX = 64 * 1024 * 1024;
const STREAM_BUF_TARGET = 48 * 1024 * 1024;
/** Parsed records in the last N bytes are never compacted away. */
const STREAM_BUF_KEEP_BEFORE_CURSOR = 16 * 1024 * 1024;

export class NxsStreamReader {
  constructor({ onSchema, onRecord, onEnd, onError, onCompact, compactionEnabled = true } = {}) {
    this.onSchema = onSchema;
    this.onRecord = onRecord;
    this.onEnd = onEnd;
    this.onError = onError;
    this.onCompact = onCompact;
    /** When false, the full byte stream is retained (required before `finish()` → `NxsReader`). */
    this.compactionEnabled = compactionEnabled;
    this._streamComplete = false;
    this.bytes = new Uint8Array(0);
    this._buffer = new Uint8Array(0);
    this._length = 0;
    /** File byte offset of `bytes[0]` (sliding window during streaming). */
    this._baseOffset = 0;
    this.keys = [];
    this.keySigils = [];
    this.keyIndex = new Map();
    this._schemaEnd = 0;
    this._nextOffset = 0;
    this._headerParsed = false;
    this._recordCount = 0;
    /** Bumped on each buffer slide so UIs can invalidate cached row views. */
    this.compactGeneration = 0;
  }

  /** Earliest absolute file offset still present in the in-memory window. */
  get earliestRetainedOffset() {
    return this._baseOffset;
  }

  /** Absolute file offset for a record's buffer-relative `offset`. */
  fileOffsetOf(relativeOffset) {
    return this._baseOffset + relativeOffset;
  }

  /** Buffer-relative offset, or `null` if that record was compacted out. */
  relativeOffsetOrNull(fileOffset) {
    const rel = fileOffset - this._baseOffset;
    if (rel < 0 || rel + 8 > this._length) return null;
    const len = rdU32(this.bytes, rel + 4);
    if (len < 8 || rel + len > this._length) return null;
    return rel;
  }

  push(chunk) {
    try {
      const incoming = chunk instanceof Uint8Array ? chunk : new Uint8Array(chunk);
      this._ensureCapacity(this._length + incoming.length);
      this._buffer.set(incoming, this._length);
      this._length += incoming.length;
      this.bytes = this._buffer.subarray(0, this._length);
      this.view = new DataView(this.bytes.buffer, this.bytes.byteOffset, this.bytes.byteLength);
      this._parseAvailable();
      if (!this._streamComplete) this._compactBuffer();
    } catch (err) {
      this.onError?.(err);
      throw err;
    }
  }

  /** Parse trailing bytes; does not compact (call after the download stream ends). */
  endOfStream() {
    this._streamComplete = true;
    this._parseAvailable();
  }

  finish() {
    this._parseAvailable();
    if (this._baseOffset !== 0) {
      throw new NxsError(
        "ERR_INCOMPLETE",
        "cannot seal to NxsReader: buffer was compacted (NYXB header not at offset 0); disable compaction for this download",
      );
    }
    const reader = new NxsReader(this.bytes);
    this.onEnd?.(reader);
    return reader;
  }

  _parseAvailable() {
    if (!this._headerParsed && !this._parseHeader()) return;
    while (this._nextOffset + 8 <= this.bytes.length) {
      if (rdU32(this.bytes, this._nextOffset) !== MAGIC_OBJ) return;
      const length = rdU32(this.bytes, this._nextOffset + 4);
      if (length < 8) throw new NxsError("ERR_OUT_OF_BOUNDS", "invalid object length");
      if (this._nextOffset + length > this.bytes.length) return;
      const obj = new NxsObject(this, this._nextOffset);
      this.onRecord?.(obj, this._recordCount++);
      this._nextOffset += length;
    }
  }

  _parseHeader() {
    if (this.bytes.length < 34) return false;
    if (rdU32(this.bytes, 0) !== MAGIC_FILE) {
      throw new NxsError("ERR_BAD_MAGIC", "preamble");
    }
    this.version = this.view.getUint16(4, true);
    this.flags = this.view.getUint16(6, true);
    this.dictHash = this.view.getBigUint64(8, true);
    this.tailPtr = Number(this.view.getBigUint64(16, true));
    if ((this.flags & 0x0002) === 0) {
      throw new NxsError("ERR_DICT_MISMATCH", "stream reader requires embedded schema");
    }

    const keyCount = this.view.getUint16(32, true);
    let offset = 34 + keyCount;
    if (this.bytes.length < offset) return false;
    const sigils = Array.from(this.bytes.subarray(34, 34 + keyCount));
    const keys = [];
    for (let i = 0; i < keyCount; i++) {
      let end = offset;
      while (end < this.bytes.length && byteAt(this.bytes, end) !== 0) end++;
      if (end >= this.bytes.length) return false;
      keys.push(_decodeMaybeShared(this.bytes, offset, end));
      offset = end + 1;
    }
    this.keySigils = sigils;
    this.keys = keys;
    this.keyIndex = new Map(keys.map((key, index) => [key, index]));
    this._schemaEnd = (offset + 7) & ~7;
    if (this.bytes.length < this._schemaEnd) return false;
    const computedHash = murmur3_64(this.bytes, 32, this._schemaEnd - 32);
    if (computedHash !== this.dictHash) {
      throw new NxsError("ERR_DICT_MISMATCH", "schema hash mismatch");
    }
    this._nextOffset = this._schemaEnd;
    this._headerParsed = true;
    this.onSchema?.(this.keys, this.keySigils);
    return true;
  }

  slot(key) {
    const s = this.keyIndex.get(key);
    if (s === undefined) throw new NxsError("ERR_KEY", `key ${key} not in schema`);
    return s;
  }

  _ensureCapacity(required) {
    if (required <= this._buffer.length) return;
    let nextCap = this._buffer.length || 4096;
    while (nextCap < required) {
      const doubled = nextCap * 2;
      nextCap = doubled <= STREAM_BUF_MAX * 2
        ? doubled
        : Math.max(required, required + 65536);
      if (nextCap >= required) break;
    }
    const next = new Uint8Array(nextCap);
    next.set(this.bytes);
    this._buffer = next;
  }

  _compactBuffer() {
    if (!this.compactionEnabled || this._length <= STREAM_BUF_MAX) return;
    let cut = this._length - STREAM_BUF_TARGET;
    const maxCut = this._nextOffset - STREAM_BUF_KEEP_BEFORE_CURSOR;
    if (this._headerParsed) {
      cut = Math.min(cut, maxCut);
    } else {
      cut = Math.min(cut, this._length - STREAM_BUF_MAX);
      if (cut < this._schemaEnd) return;
    }
    if (cut <= 0) return;
    const newLen = this._length - cut;
    const next = new Uint8Array(newLen);
    next.set(this._buffer.subarray(cut, this._length));
    this._buffer = next;
    this._baseOffset += cut;
    this.compactGeneration++;
    this.onCompact?.(cut, this._baseOffset);
    this._length = newLen;
    this._nextOffset -= cut;
    this.bytes = this._buffer.subarray(0, this._length);
    this.view = new DataView(this.bytes.buffer, this.bytes.byteOffset, this.bytes.byteLength);
  }
}

// ── Pre-built field index (random access without per-read bitmask walk) ─────

const ABSENT_OFFSET = 0xFFFFFFFF;

/**
 * Flat per-record value offsets for one schema field. Built via
 * `reader.buildFieldIndex(key)` (one O(n) scan).
 */
export class NxsFieldIndex {
  /**
   * @param {NxsReader} reader
   * @param {number} slot
   * @param {Uint32Array} offsets — absolute file offsets of each value
   */
  constructor(reader, slot, offsets) {
    this.reader = reader;
    this.slot = slot;
    this.offsets = offsets;
  }

  _off(i) {
    const o = this.offsets[i];
    return o === ABSENT_OFFSET ? -1 : o;
  }

  getStrAt(i) {
    const off = this._off(i);
    if (off < 0) return undefined;
    const bytes = this.reader.bytes;
    return decodeUtf8Fast(bytes, off + 4, rdU32(bytes, off));
  }

  getF64At(i) {
    const off = this._off(i);
    if (off < 0) return undefined;
    return rdF64(this.reader.bytes, off);
  }

  getI64At(i) {
    const off = this._off(i);
    if (off < 0) return undefined;
    return rdI64Safe(this.reader.bytes, off);
  }

  getBoolAt(i) {
    const off = this._off(i);
    if (off < 0) return undefined;
    return this.reader.bytes[off] !== 0;
  }
}

// ── Object view ─────────────────────────────────────────────────────────────

/**
 * A view over a single NXS object. Does not decode fields until requested.
 * Constructing an NxsObject costs ~nothing (just stores an offset).
 */
export class NxsObject {
  constructor(reader, offset, recordIndex = null) {
    this.reader = reader;
    this.offset = offset;
    this.recordIndex = recordIndex;
    this._stage = 0; // 0 = untouched, 1 = offset-table located, 2 = rank cached
  }

  /** Top-level columnar/PAX record vs nested NYXO blob inside a columnar file. */
  _usesColumnarFieldAccess() {
    const r = this.reader;
    if (r._layout !== "columnar" && r._layout !== "pax") return false;
    if (this.recordIndex == null) return false;
    if (this.offset + 4 > r.bytes.length) return false;
    return rdU32(r.bytes, this.offset) !== MAGIC_OBJ;
  }

  /**
   * Stage 1: walk LEB128 bitmask only far enough to locate the offset table
   * start. No allocations. This is enough for single-field access.
   */
  _locateOffsetTable() {
    if (this._stage >= 1) return;
    const bytes = this.reader.bytes;
    let p = this.offset;

    const magic = rdU32(bytes, p);
    if (magic !== MAGIC_OBJ) {
      throw new NxsError("ERR_BAD_MAGIC", `expected NYXO at offset ${p}`);
    }
    this.length = rdU32(bytes, p + 4);
    p += 8;

    let byte;
    do { byte = bytes[p++]; } while (byte & 0x80);

    this._bitmaskStart = this.offset + 8;
    this._offsetTableStart = p;
    this._stage = 1;
  }

  /**
   * Stage 2 (lazy): build the full present/rank arrays for O(1) repeated
   * access. Only called when a second field is accessed on the same object.
   */
  _buildRankCache() {
    if (this._stage >= 2) return;
    if (this._stage < 1) this._locateOffsetTable();

    const bytes = this.reader.bytes;
    const keyCount = this.reader.keys.length;
    const present = new Uint8Array(keyCount);
    const rank = new Uint16Array(keyCount + 1);

    let p = this._bitmaskStart;
    let slot = 0;
    let byte;
    do {
      byte = bytes[p++];
      const dataBits = byte & 0x7F;
      for (let b = 0; b < 7 && slot < keyCount; b++, slot++) {
        present[slot] = (dataBits >> b) & 1;
      }
    } while ((byte & 0x80) && slot < keyCount);

    let acc = 0;
    for (let s = 0; s < keyCount; s++) {
      rank[s] = acc;
      acc += present[s];
    }
    rank[keyCount] = acc;

    this._present = present;
    this._rank = rank;
    this._stage = 2;
  }

  /**
   * Inline single-slot rank: walk the bitmask from the start, counting present
   * bits before `slot`, returning [present(0/1), tableIdx].
   * Used for the first access; cheaper than building the full rank array.
   */
  _inlineRank(slot) {
    const bytes = this.reader.bytes;
    let p = this._bitmaskStart;
    let curSlot = 0;
    let tableIdx = 0;
    while (curSlot <= slot) {
      const byte = bytes[p++];
      const dataBits = byte & 0x7F;
      for (let b = 0; b < 7; b++, curSlot++) {
        if (curSlot === slot) {
          return [(dataBits >> b) & 1, tableIdx];
        }
        if ((dataBits >> b) & 1) tableIdx++;
      }
      if ((byte & 0x80) === 0) break;
    }
    return [0, 0];
  }

  /**
   * Adaptive slot lookup: uses inline walk on the first call, rank cache
   * after a second distinct access. Returns absolute byte offset of the value,
   * or -1 if absent.
   */
  _resolveSlot(slot) {
    const r = this.reader;
    if (r.extSchema && r.cellPlan) {
      const denseFrames = (r.flags & FLAG_DENSE_FRAMES) !== 0;
      const off = resolveFieldOffset(r.bytes, this.offset, slot, r.extSchema, r.cellPlan, denseFrames);
      return off == null ? -1 : off;
    }
    const bytes = r.bytes;
    if (this._stage === 2) {
      if (!this._present[slot]) return -1;
      return this.offset + rdU16(bytes, this._offsetTableStart + this._rank[slot] * 2);
    }
    if (this._stage === 0) this._locateOffsetTable();

    if (this._firstAccessedSlot === undefined) {
      this._firstAccessedSlot = slot;
      const [present, tableIdx] = this._inlineRank(slot);
      if (!present) return -1;
      return this.offset + rdU16(bytes, this._offsetTableStart + tableIdx * 2);
    }

    if (slot !== this._firstAccessedSlot) {
      this._buildRankCache();
      if (!this._present[slot]) return -1;
      return this.offset + rdU16(bytes, this._offsetTableStart + this._rank[slot] * 2);
    }

    const [present, tableIdx] = this._inlineRank(slot);
    if (!present) return -1;
    return this.offset + rdU16(bytes, this._offsetTableStart + tableIdx * 2);
  }

  /**
   * Get the value of a field by key name.
   * Returns undefined if the field is not present.
   * O(1) on the first call after parseHeader; subsequent calls re-decode
   * but do not re-parse the header.
   */
  get(key) {
    const slot = this.reader.keyIndex.get(key);
    if (slot === undefined) return undefined;
    const off = this._resolveSlot(slot);
    if (off < 0) return undefined;
    return this._decodeValue(off, this.reader.keySigils[slot]);
  }

  /** Decode all fields into a plain JS object (the eager path). */
  toObject() {
    this._buildRankCache();
    const obj = {};
    for (const [key, slot] of this.reader.keyIndex) {
      if (this._present[slot]) obj[key] = this.get(key);
    }
    return obj;
  }

  _decodeValue(offset, _sigilHint) {
    const bytes = this.reader.bytes;

    // If we have no hint, peek: if first 4 bytes are a known magic, it's
    // a nested object or list. Otherwise assume an 8-byte atomic value.
    // Use rdU32 (not DataView): sparse openRemote keeps view scoped to header only.
    if (offset + 4 <= bytes.length) {
      const maybeMagic = rdU32(bytes, offset);
      if (maybeMagic === MAGIC_OBJ) return new NxsObject(this.reader, offset);
      if (maybeMagic === MAGIC_LIST) return this._decodeList(offset);
    }

    // Schema-less fallback: treat string-sigil slots as length-prefixed strings,
    // everything else as i64. (The current writer encodes all keys with the
    // SIGIL_STR byte in the TypeManifest; the *value* encoding is determined
    // by how the writer wrote it — the reader has to know the type.)
    //
    // For the benchmark/test we expose distinct accessors below.
    return this._readI64(offset);
  }

  _readI64(offset) {
    return rdI64Safe(this.reader.bytes, offset);
  }
  _readF64(offset) {
    return rdF64(this.reader.bytes, offset);
  }
  _readBool(offset) {
    return byteAt(this.reader.bytes, offset) !== 0;
  }
  _readStr(offset) {
    const bytes = this.reader.bytes;
    const len = rdU32(bytes, offset);
    return decodeUtf8Fast(bytes, offset + 4, len);
  }

  // ── Typed accessors (fast path when caller knows the type) ──────────────
  // The key-name variants do a Map lookup then delegate to the slot variants.
  // For hot code, use reader.slot(key) once and then call the ...BySlot forms.

  getI64(key)  { return this.getI64BySlot(this.reader.keyIndex.get(key)); }
  getF64(key)  { return this.getF64BySlot(this.reader.keyIndex.get(key)); }
  getBool(key) { return this.getBoolBySlot(this.reader.keyIndex.get(key)); }
  getStr(key)  { return this.getStrBySlot(this.reader.keyIndex.get(key)); }

  /** Fast path: resolve slot with `reader.slot("username")` once, reuse. */
  getI64BySlot(slot) {
    if (slot === undefined) return undefined;
    const r = this.reader;
    if (this._usesColumnarFieldAccess()) {
      const ri = this.recordIndex;
      const cell = r._colNumericBytes(ri, slot);
      if (!cell) return undefined;
      return rdI64Safe(cell, 0);
    }
    const off = this._resolveSlot(slot);
    if (off < 0) return undefined;
    if (r.extSchema) {
      return decodeIntCell(r.bytes, off, fieldCellWidth(r.extSchema, slot));
    }
    return rdI64Safe(r.bytes, off);
  }

  getF64BySlot(slot) {
    if (slot === undefined) return undefined;
    const r = this.reader;
    if (this._usesColumnarFieldAccess()) {
      const ri = this.recordIndex;
      const cell = r._colNumericBytes(ri, slot);
      if (!cell) return undefined;
      return rdF64(cell, 0);
    }
    const off = this._resolveSlot(slot);
    if (off < 0) return undefined;
    if (r.extSchema) {
      return decodeF64Cell(r.bytes, off, fieldCellWidth(r.extSchema, slot));
    }
    return rdF64(r.bytes, off);
  }

  getBoolBySlot(slot) {
    if (slot === undefined) return undefined;
    const r = this.reader;
    if (this._usesColumnarFieldAccess()) {
      const ri = this.recordIndex;
      const cell = r._colNumericBytes(ri, slot);
      if (!cell) return undefined;
      return cell[0] !== 0;
    }
    if (r.extSchema && r.cellPlan?.packedBools && r.cellPlan.boolSlots.includes(slot)) {
      return readPackedBool(r.bytes, this.offset, slot, r.extSchema, r.cellPlan);
    }
    const off = this._resolveSlot(slot);
    if (off < 0) return undefined;
    return byteAt(r.bytes, off) !== 0;
  }

  getStrBySlot(slot) {
    if (slot === undefined) return undefined;
    const r = this.reader;
    if (this._usesColumnarFieldAccess()) {
      return r.colGetStr(r.keys[slot], this.recordIndex);
    }
    const off = this._resolveSlot(slot);
    if (off < 0) return undefined;
    if (r.extSchema) {
      return materialiseStrAt(r.bytes, off, slot, r.extSchema);
    }
    const bytes = r.bytes;
    return decodeUtf8Fast(bytes, off + 4, rdU32(bytes, off));
  }

  getBinaryBySlot(slot) {
    if (slot === undefined) return undefined;
    const r = this.reader;
    if (this._usesColumnarFieldAccess()) {
      return r.colGetBinary(r.keys[slot], this.recordIndex);
    }
    const off = this._resolveSlot(slot);
    if (off < 0) return undefined;
    const bytes = r.bytes;
    const len = rdU32(bytes, off);
    return bytes.subarray(off + 4, off + 4 + len);
  }

  _decodeList(offset) {
    const bytes = this.reader.bytes;
    const magic = rdU32(bytes, offset);
    if (magic !== MAGIC_LIST) throw new NxsError("ERR_BAD_MAGIC", "list magic");
    // Length at offset+4, ElemSigil at offset+8, ElemCount at offset+9
    const elemSigil = byteAt(bytes, offset + 8);
    const elemCount = rdU32(bytes, offset + 9);
    // Data starts at offset + 16 (after 3-byte padding)
    const dataStart = offset + 16;
    const out = new Array(elemCount);
    for (let i = 0; i < elemCount; i++) {
      const elemOff = dataStart + i * 8;
      if (elemSigil === SIGIL_INT) out[i] = rdI64Safe(bytes, elemOff);
      else if (elemSigil === SIGIL_FLOAT) out[i] = rdF64(bytes, elemOff);
      else out[i] = null; // unsupported in reader for POC
    }
    return out;
  }
}

// ── Reusable cursor for zero-allocation scans ───────────────────────────────
//
// `NxsCursor` has the same field-access API as `NxsObject` but can be
// repositioned with `seek(i)` or `_reset(offset)` without any allocation.
// Use via `reader.scan((cur, i) => ...)` or `reader.cursor()`.

export class NxsCursor extends NxsObject {
  constructor(reader) {
    super(reader, 0);
  }

  /** Move to record `i`. */
  seek(i) {
    if (i < 0 || i >= this.reader.recordCount) {
      throw new NxsError("ERR_OUT_OF_BOUNDS", `record ${i} out of range`);
    }
    const off = this.reader._rowRecordOffset(i);
    if (off == null) {
      throw new NxsError("ERR_OUT_OF_BOUNDS", `record ${i} offset out of bounds`);
    }
    this._reset(off);
    return this;
  }

  /**
   * Seek and pre-build the rank cache for multi-field access on this record.
   */
  seekWarm(i) {
    this.seek(i);
    this._locateOffsetTable();
    this._buildRankCache();
    return this;
  }

  /** Internal: reset to a raw object offset. */
  _reset(absOffset) {
    this.offset = absOffset;
    this._stage = 0;
    this._firstAccessedSlot = undefined;
    // _present / _rank are reused if already allocated; their values are
    // overwritten on the next _buildRankCache(). No need to zero them here.
  }
}

// ── Query engine ──────────────────────────────────────────────────────────────

/**
 * Query is a lazy, filterable view over an NxsReader.
 * Created via `reader.where(pred)` or the `reader.all` getter.
 *
 * Usage:
 *   const q = reader.where(and(eq("active", true), gt("score", 80.0)));
 *   for (const rec of q) console.log(rec.getStr("username"));
 *   console.log(q.count());
 *   console.log(q.first()?.getStr("username"));
 */
export class Query {
  #reader;
  #pred; // null = all records

  constructor(reader, pred = null) {
    this.#reader = reader;
    this.#pred = pred;
  }

  /** Lazy generator — yields NxsObject instances for matching records. */
  *[Symbol.iterator]() {
    const n = this.#reader.recordCount;
    for (let i = 0; i < n; i++) {
      const rec = this.#reader.record(i);
      if (!this.#pred || this.#pred(rec)) yield rec;
    }
  }

  /** Returns the count of matching records. */
  count() {
    let n = 0;
    for (const _ of this) n++;
    return n;
  }

  /** Returns the first matching record, or null if none match. */
  first() {
    for (const r of this) return r;
    return null;
  }
}

// ── Predicate factories ───────────────────────────────────────────────────────
//
// Each factory returns a function (NxsObject) => boolean.
// Value-type detection determines which typed accessor is used:
//   string  → getStr
//   boolean → getBool
//   number  → getF64  (covers both int and float fields; use getI64 for BigInt)
//   bigint  → getI64 (returned as JS number from rdI64Safe)

function _makeGetter(val) {
  if (typeof val === "boolean") return (rec, key) => rec.getBool(key);
  if (typeof val === "bigint")  return (rec, key) => rec.getI64(key); // getI64 returns Number
  if (typeof val === "number")  return (rec, key) => rec.getF64(key);
  return (rec, key) => rec.getStr(key);
}

export const eq  = (key, val) => { const g = _makeGetter(val); return rec => g(rec, key) === val; };
export const gt  = (key, val) => { const g = _makeGetter(val); return rec => g(rec, key) > val; };
export const lt  = (key, val) => { const g = _makeGetter(val); return rec => g(rec, key) < val; };
export const gte = (key, val) => { const g = _makeGetter(val); return rec => g(rec, key) >= val; };
export const lte = (key, val) => { const g = _makeGetter(val); return rec => g(rec, key) <= val; };
export const and = (...preds) => rec => preds.every(p => p(rec));
export const or  = (...preds) => rec => preds.some(p => p(rec));
export const not = (pred)     => rec => !pred(rec);

// ── NxsReader query entry-points ─────────────────────────────────────────────
//
// Patch these onto NxsReader prototype after the class definition so the
// class body itself remains unchanged.

NxsReader.prototype.where = function where(pred) { return new Query(this, pred); };
Object.defineProperty(NxsReader.prototype, "all", {
  get() { return new Query(this); },
});
