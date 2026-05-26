// Sparse file view: resident header/tail spans + range-fetched data pages.
// Used by NxsReader.openRemote so multi-GB .nxb files open without one giant ArrayBuffer.

import { DEFAULT_PAGE_SIZE } from "./prefetch.js";

/**
 * @typedef {(byteStart: number, byteLength: number, signal?: AbortSignal) => Promise<Uint8Array>} FetchRangeFn
 */

export class SparseBytes {
  /**
   * @param {number} fileSize
   * @param {{ start: number, data: Uint8Array }[]} resident — disjoint spans already in memory
   * @param {FetchRangeFn} fetchRange
   * @param {number} [pageSize]
   */
  constructor(fileSize, resident, fetchRange, pageSize = DEFAULT_PAGE_SIZE) {
    this._isSparseBytes = true;
    this.length = fileSize;
    this.pageSize = pageSize;
    this._resident = resident.slice().sort((a, b) => a.start - b.start);
    this._fetchRange = fetchRange;
    /** @type {Map<number, Uint8Array>} */
    this._pages = new Map();
  }

  /** @param {number} byteStart @param {Uint8Array} data */
  fillRange(byteStart, data) {
    const pageSize = this.pageSize;
    let off = 0;
    while (off < data.byteLength) {
      const abs = byteStart + off;
      const pageIndex = Math.floor(abs / pageSize);
      const pageStart = pageIndex * pageSize;
      let page = this._pages.get(pageIndex);
      if (!page) {
        const len = Math.min(pageSize, this.length - pageStart);
        page = new Uint8Array(len);
        this._pages.set(pageIndex, page);
      }
      const inPage = abs - pageStart;
      const n = Math.min(data.byteLength - off, page.byteLength - inPage);
      page.set(data.subarray(off, off + n), inPage);
      off += n;
    }
  }

  _residentByte(i) {
    for (const { start, data } of this._resident) {
      if (i >= start && i < start + data.length) return data[i - start];
    }
    return undefined;
  }

  _cachedByte(i) {
    const pageIndex = Math.floor(i / this.pageSize);
    const page = this._pages.get(pageIndex);
    if (!page) return undefined;
    const off = i - pageIndex * this.pageSize;
    if (off < 0 || off >= page.byteLength) return undefined;
    return page[off];
  }

  /**
   * @param {number} i
   * @returns {number}
   */
  readByte(i) {
    if (i < 0 || i >= this.length) {
      throw new RangeError(`SparseBytes read out of bounds: ${i}`);
    }
    const r = this._residentByte(i);
    if (r !== undefined) return r;
    const c = this._cachedByte(i);
    if (c !== undefined) return c;
    throw new Error(
      `SparseBytes: byte ${i} not resident — call prefetch_viewport (or fillRange) before read`,
    );
  }

  /**
   * @param {number} start
   * @param {number} end
   * @returns {Uint8Array}
   */
  subarray(start, end = this.length) {
    const len = end - start;
    const out = new Uint8Array(len);
    for (let i = 0; i < len; i++) out[i] = this.readByte(start + i);
    return out;
  }

  get buffer() {
    throw new Error("SparseBytes has no single backing ArrayBuffer");
  }

  get byteOffset() {
    return 0;
  }

  /**
   * Proxy so `bytes[i]` / `bytes[p++]` in NxsObject work like a Uint8Array.
   * @returns {SparseBytes & Uint8Array}
   */
  asIndexed() {
    const target = this;
    return new Proxy(target, {
      get(t, prop) {
        if (prop === "length") return t.length;
        if (prop === "subarray") return (start, end) => t.subarray(start, end ?? t.length);
        if (prop === "_isSparseBytes") return true;
        if (prop === "buffer") return t.buffer;
        if (prop === "byteOffset") return 0;
        if (typeof prop === "symbol") return undefined;
        const idx =
          typeof prop === "number"
            ? prop
            : typeof prop === "string" && /^\d+$/.test(prop)
              ? Number(prop)
              : null;
        if (idx !== null && idx >= 0 && idx < t.length) return t.readByte(idx);
        return Reflect.get(t, prop);
      },
    });
  }
}

/** @param {Uint8Array | SparseBytes} bytes @param {number} off */
export function byteAt(bytes, off) {
  if (bytes && bytes._isSparseBytes) return bytes.readByte(off);
  return bytes[off];
}
