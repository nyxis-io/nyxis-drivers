// Access pattern detector (Adaptive-prefetch-spec §4).

export const SEQUENTIAL_THRESHOLD = 10;
export const RANDOM_THRESHOLD = 100;
export const HISTORY_SIZE = 32;
export const MIN_OBSERVATIONS = 8;
export const UPGRADE_SEQUENTIAL_THRESHOLD = 100;

export const PATTERN_UNKNOWN = "unknown";
export const PATTERN_SEQUENTIAL = "sequential";
export const PATTERN_RANDOM = "random";
export const PATTERN_MIXED = "mixed";

/** Observes record(index) / seek calls and classifies access patterns. */
export class AccessPatternDetector {
  constructor() {
    /** @type {number[]} */
    this.accesses = Array(HISTORY_SIZE).fill(-1);
    this.writePos = 0;
    this.filled = 0;
    this._sequentialRuns = 0;
    this._randomJumps = 0;
    this.lastIndex = -1;
  }

  sequentialRuns() {
    return this._sequentialRuns;
  }

  getLastIndex() {
    return this.lastIndex;
  }

  /** @param {number} index */
  observe(index) {
    const idx = index;
    if (this.lastIndex >= 0) {
      const delta = Math.abs(idx - this.lastIndex);
      if (delta <= SEQUENTIAL_THRESHOLD) {
        this._sequentialRuns = Math.min(this._sequentialRuns + 1, 0xffffffff);
      } else if (delta > RANDOM_THRESHOLD) {
        this._randomJumps = Math.min(this._randomJumps + 1, 0xffffffff);
      }
    }
    this.accesses[this.writePos] = idx;
    this.writePos = (this.writePos + 1) % HISTORY_SIZE;
    if (this.filled < HISTORY_SIZE) this.filled++;
    this.lastIndex = idx;
  }

  /** @returns {string} */
  pattern() {
    const total = this._sequentialRuns + this._randomJumps;
    if (total < MIN_OBSERVATIONS) return PATTERN_UNKNOWN;
    if (this._sequentialRuns > this._randomJumps * 3) return PATTERN_SEQUENTIAL;
    if (this._randomJumps > this._sequentialRuns) return PATTERN_RANDOM;
    return PATTERN_MIXED;
  }

  /**
   * Predicted next record indices when pattern is sequential (§4.4).
   * @param {number} depth
   * @param {number} recordCount
   * @returns {number[]}
   */
  predictNext(depth, recordCount) {
    if (this.pattern() !== PATTERN_SEQUENTIAL || this.lastIndex < 0) return [];
    const start = this.lastIndex + 1;
    const out = [];
    for (let i = 0; i < depth; i++) {
      const idx = start + i;
      if (idx < recordCount) out.push(idx);
    }
    return out;
  }
}
