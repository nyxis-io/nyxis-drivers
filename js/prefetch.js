// Adaptive prefetch — page cache, range coalescing, in-flight dedup (spec §6–§8.4).

export const HINT_UNKNOWN = 0;
export const HINT_SEQUENTIAL = 1;
export const HINT_RANDOM = 2;
export const HINT_FULL = 3;
export const HINT_PARTIAL = 4;

export const DEFAULT_PAGE_SIZE = 65536;
export const DEFAULT_MAX_PAGES = 64;
export const DEFAULT_COALESCE_GAP_PAGES = 4;

/**
 * @param {number[]} indices page indices (any order)
 * @param {number} gapPages max gap between pages to merge (inclusive)
 * @param {number} pageSize
 * @returns {{ pageStart: number, pageEnd: number, byteStart: number, byteLength: number }[]}
 */
export function coalescePageIndices(indices, gapPages, pageSize = DEFAULT_PAGE_SIZE) {
  if (!indices.length) return [];
  const uniq = [...new Set(indices)].sort((a, b) => a - b);
  const spans = [];
  let start = uniq[0];
  let end = uniq[0];
  for (let i = 1; i < uniq.length; i++) {
    if (uniq[i] - end <= gapPages) {
      end = uniq[i];
    } else {
      spans.push([start, end]);
      start = end = uniq[i];
    }
  }
  spans.push([start, end]);
  return spans.map(([a, b]) => ({
    pageStart: a,
    pageEnd: b,
    byteStart: a * pageSize,
    byteLength: (b - a + 1) * pageSize,
  }));
}

/** Clamp byte ranges to file size. */
export function clampRanges(ranges, fileSize) {
  return ranges
    .map((r) => {
      let len = r.byteLength;
      if (r.byteStart + len > fileSize) len = fileSize - r.byteStart;
      if (len <= 0) return null;
      return { ...r, byteLength: len };
    })
    .filter(Boolean);
}

export class PageCache {
  /**
   * @param {number} maxPages
   * @param {number} pageSize
   */
  constructor(maxPages = DEFAULT_MAX_PAGES, pageSize = DEFAULT_PAGE_SIZE) {
    this.maxPages = maxPages;
    this.pageSize = pageSize;
    /** @type {Map<number, { data: Uint8Array, lastUsed: number, pinned: boolean }>} */
    this.pages = new Map();
    this._clock = 0;
    this.hits = 0;
    this.misses = 0;
  }

  has(pageIndex) {
    return this.pages.has(pageIndex);
  }

  get(pageIndex) {
    const e = this.pages.get(pageIndex);
    if (!e) {
      this.misses++;
      return null;
    }
    e.lastUsed = ++this._clock;
    this.hits++;
    return e.data;
  }

  set(pageIndex, data, { pinned = false } = {}) {
    if (this.maxPages <= 0) return;
    while (this.pages.size >= this.maxPages) {
      if (!this._evictOne()) break;
    }
    this.pages.set(pageIndex, {
      data,
      lastUsed: ++this._clock,
      pinned,
    });
  }

  _evictOne() {
    let oldest = Infinity;
    let victim = -1;
    for (const [idx, e] of this.pages) {
      if (e.pinned) continue;
      if (e.lastUsed < oldest) {
        oldest = e.lastUsed;
        victim = idx;
      }
    }
    if (victim < 0) return false;
    this.pages.delete(victim);
    return true;
  }

  pinPages(pageIndices) {
    for (const p of pageIndices) {
      const e = this.pages.get(p);
      if (e) e.pinned = true;
    }
  }

  unpinAll() {
    for (const e of this.pages.values()) e.pinned = false;
  }

  stats() {
    let bytes = 0;
    for (const e of this.pages.values()) bytes += e.data.byteLength;
    return {
      pages_cached: this.pages.size,
      pages_max: this.maxPages,
      memory_used_bytes: bytes,
      cache_hits: this.hits,
      cache_misses: this.misses,
    };
  }
}

export class InFlightMap {
  constructor() {
    /** @type {Map<number, Promise<Uint8Array>>} */
    this._map = new Map();
  }

  has(pageIndex) {
    return this._map.has(pageIndex);
  }

  get(pageIndex) {
    return this._map.get(pageIndex);
  }

  set(pageIndex, promise) {
    this._map.set(pageIndex, promise);
    promise.finally(() => {
      if (this._map.get(pageIndex) === promise) this._map.delete(pageIndex);
    });
  }
}

/**
 * Collect page indices for record range [startIndex, endIndex] using tail-index offsets.
 * @param {(i: number) => number} recordOffset
 */
export function pageIndicesForViewport(startIndex, endIndex, pageSize, recordOffset) {
  const out = [];
  for (let i = startIndex; i <= endIndex; i++) {
    const off = recordOffset(i);
    out.push(Math.floor(off / pageSize));
  }
  return out;
}
