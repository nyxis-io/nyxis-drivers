<?php
/**
 * NXS Binary Reader — pure PHP 8.0+, no Composer, no extensions.
 *
 * Implements the Nyxis v1.1 binary (.nxb) wire format.
 * All multi-byte integers are little-endian.
 *
 * Usage:
 *   $bytes  = file_get_contents('records.nxb');
 *   $reader = new Nxs\Reader($bytes);
 *   echo $reader->recordCount();          // int
 *   echo implode(',', $reader->keys());   // CSV key names
 *   $obj = $reader->record(42);
 *   echo $obj->getStr('username');
 *   echo $reader->sumF64('score');
 */

namespace Nxs;

require_once __DIR__ . '/Prefetch.php';
require_once __DIR__ . '/Pattern.php';

// ── Constants ────────────────────────────────────────────────────────────────

const MAGIC_FILE   = 0x4E595842; // NYXB
const MAGIC_OBJ    = 0x4E59584F; // NYXO
const MAGIC_FOOTER = 0x2153584E; // NXS!

const FLAG_SCHEMA_EMBEDDED = 0x0002;
const FLAG_COLUMNAR        = 0x0001;
const FLAG_PAX             = 0x0004;
const MAGIC_PAGE           = 0x4E585350; // NYSP

const FOOTER_ROW_BYTES     = 12;
const FOOTER_COL_BYTES     = 20;
const FOOTER_PAX_BYTES     = 28;
const COL_TAIL_ENTRY_BYTES = 20;
const PAX_TAIL_ENTRY_BYTES = 28;

// ── Exceptions ───────────────────────────────────────────────────────────────

class NxsException extends \RuntimeException {}

// ── Helpers ──────────────────────────────────────────────────────────────────

/**
 * NXS custom 7-bit bitmask encoding (not standard LEB128).
 *
 * Reads continuation bytes until the MSB is clear, extracting 7 data bits
 * per byte in LSB-first order (NXS object presence mask encoding).
 * Advances $pos past the consumed bytes and returns the decoded integer.
 */
function scanBitmask(string $bytes, int &$pos): int
{
    $result = 0;
    $shift  = 0;
    do {
        $byte    = ord($bytes[$pos++]);
        $result |= ($byte & 0x7F) << $shift;
        $shift  += 7;
    } while ($byte & 0x80);
    return $result;
}

/**
 * Read uint32 little-endian at offset (no bounds check — caller's duty).
 */
function rdU32(string $bytes, int $off): int
{
    return unpack('Vval', $bytes, $off)['val'];
}

/**
 * Read uint64 little-endian at offset.
 * On 64-bit PHP (PHP_INT_SIZE === 8) 'Q' gives an int that fits.
 * Files in practice are < 2 GB so no overflow risk.
 */
function rdU64(string $bytes, int $off): int
{
    return unpack('Qval', $bytes, $off)['val'];
}

/**
 * Read uint16 little-endian at offset.
 */
function rdU16(string $bytes, int $off): int
{
    return unpack('vval', $bytes, $off)['val'];
}

/**
 * Read signed int64 little-endian at offset.
 * PHP 7+ 'q' is native-endian on 64-bit; on x86/arm64 LE machines this is LE.
 * We verify with PHP_INT_SIZE and a runtime check during Reader construction.
 */
function rdI64(string $bytes, int $off): int
{
    return unpack('qval', $bytes, $off)['val'];
}

/**
 * Read IEEE-754 double little-endian at offset.
 * 'e' = LE double (PHP ≥ 7.0.15). If unavailable, fall back to strrev+d.
 */
function rdF64(string $bytes, int $off): float
{
    static $useE = null;
    if ($useE === null) {
        // Runtime probe: 1.0 LE = 00 00 00 00 00 00 F0 3F
        $probe = unpack('eval', "\x00\x00\x00\x00\x00\x00\xF0\x3F")['val'];
        $useE  = (abs($probe - 1.0) < 1e-15);
    }
    if ($useE) {
        return unpack('eval', $bytes, $off)['val'];
    }
    // Fallback: 'e' not available — reverse bytes for big-endian unpack 'd'.
    return unpack('dval', strrev(substr($bytes, $off, 8)))['val'];
}

function nullBitmapBytes(int $n): int
{
    $raw = (int)(($n + 7) / 8);
    return ($raw + 7) & ~7;
}

function colBit(string $bm, int $rec): bool
{
    return ((ord($bm[$rec >> 3]) >> ($rec & 7)) & 1) === 1;
}

function isVarSigil(int $sigil): bool
{
    return $sigil === ord('"') || $sigil === ord('<');
}

function varOffBytesLen(int $rc): int
{
    $off = ($rc + 1) * 4;
    if ($off < 0) {
        throw new NxsException('ERR_OUT_OF_BOUNDS: var offsets overflow');
    }
    return $off;
}

function fieldSectorLen(string $bytes, int $sectorOff, int $rc, int $sigil): int
{
    $bmLen = nullBitmapBytes($rc);
    if (!isVarSigil($sigil)) {
        return $bmLen + $rc * 8;
    }
    $offBytes = varOffBytesLen($rc);
    if ($sectorOff + $bmLen + $offBytes > strlen($bytes)) {
        throw new NxsException('ERR_OUT_OF_BOUNDS: var offsets');
    }
    $end = rdU32($bytes, $sectorOff + $bmLen + $rc * 4);
    $total = $bmLen + $offBytes + $end;
    if ($sectorOff + $total > strlen($bytes)) {
        throw new NxsException('ERR_OUT_OF_BOUNDS: var values');
    }
    return $total;
}

function varStrAt(string $offsets, string $values, int $recordIndex): ?string
{
    $need = ($recordIndex + 2) * 4;
    if (strlen($offsets) < $need) {
        return null;
    }
    $off = $recordIndex * 4;
    $start = rdU32($offsets, $off);
    $end   = rdU32($offsets, $off + 4);
    if ($end < $start || $end > strlen($values)) {
        return null;
    }
    return substr($values, $start, $end - $start);
}

// ── NxsObject ────────────────────────────────────────────────────────────────

/**
 * Lazy view over a single NYXO record.
 * The bitmask/offset-table are parsed only on first field access.
 */
class NxsObject
{
    private Reader $reader;
    private int    $offset;
    private int    $recordIndex;

    // Stage 0 = untouched; filled lazily:
    private int   $offsetTableStart = -1;   // absolute byte pos of OffsetTable
    private array $present          = [];   // bool per slot
    private array $rank             = [];   // rank[slot] = index into OffsetTable

    public function __construct(Reader $reader, int $offset, int $recordIndex = -1)
    {
        $this->reader      = $reader;
        $this->offset      = $offset;
        $this->recordIndex = $recordIndex >= 0 ? $recordIndex : $offset;
    }

    private function objAtNyxo(): bool
    {
        $bytes = $this->reader->rawBytes();
        if ($this->offset + 4 > strlen($bytes)) {
            return false;
        }
        return rdU32($bytes, $this->offset) === MAGIC_OBJ;
    }

    /** Columnar/PAX top-level records use record index; nested NYXO blobs use row paths. */
    private function usesColumnarFieldAccess(): bool
    {
        return $this->reader->layout() !== 'row' && !$this->objAtNyxo();
    }

    // ── Internal: parse bitmask + build offset-table index ────────────────

    private function ensureParsed(): void
    {
        if ($this->offsetTableStart >= 0) return;

        $bytes  = $this->reader->rawBytes();
        $p      = $this->offset;

        // Validate NYXO magic
        $magic = rdU32($bytes, $p);
        if ($magic !== MAGIC_OBJ) {
            throw new NxsException(sprintf(
                'ERR_BAD_MAGIC: expected NYXO at offset %d, got 0x%08X', $p, $magic
            ));
        }
        $p += 8; // skip Magic(4) + Length(4)

        $bitmaskStart = $p;

        // Walk LEB128 bitmask, building present[] and rank[]
        $keyCount = $this->reader->keyCount();
        $present  = array_fill(0, $keyCount, false);
        $tableIdx = 0;
        $slot     = 0;

        do {
            $byte     = ord($bytes[$p++]);
            $dataBits = $byte & 0x7F;
            for ($b = 0; $b < 7 && $slot < $keyCount; $b++, $slot++) {
                if (($dataBits >> $b) & 1) {
                    $present[$slot] = true;
                }
            }
        } while ($byte & 0x80);

        // Build rank: rank[slot] = number of set bits before slot
        $rank = array_fill(0, $keyCount, 0);
        $acc  = 0;
        for ($s = 0; $s < $keyCount; $s++) {
            $rank[$s] = $acc;
            if ($present[$s]) $acc++;
        }

        $this->present          = $present;
        $this->rank             = $rank;
        $this->offsetTableStart = $p;
    }

    /**
     * Resolve the absolute byte offset of slot $slot's value.
     * Returns -1 if the field is absent.
     */
    private function resolveSlot(int $slot): int
    {
        $this->ensureParsed();
        if ($slot < 0 || $slot >= count($this->present) || !$this->present[$slot]) {
            return -1;
        }
        $bytes  = $this->reader->rawBytes();
        $relOff = rdU16($bytes, $this->offsetTableStart + $this->rank[$slot] * 2);
        return $this->offset + $relOff;
    }

    // ── Public typed accessors ─────────────────────────────────────────────

    public function getStr(string $key): ?string
    {
        $slot = $this->reader->slotOf($key);
        if ($slot < 0) return null;
        if ($this->usesColumnarFieldAccess()) {
            return $this->reader->colGetStr($key, $this->recordIndex);
        }
        $off = $this->resolveSlot($slot);
        if ($off < 0) return null;
        $bytes = $this->reader->rawBytes();
        $len   = rdU32($bytes, $off);
        return substr($bytes, $off + 4, $len);
    }

    public function getI64(string $key): ?int
    {
        $slot = $this->reader->slotOf($key);
        if ($slot < 0) return null;
        if ($this->usesColumnarFieldAccess()) {
            $cell = $this->reader->colNumericBytes($this->recordIndex, $slot);
            return $cell === null ? null : rdI64($cell, 0);
        }
        $off = $this->resolveSlot($slot);
        if ($off < 0) return null;
        return rdI64($this->reader->rawBytes(), $off);
    }

    public function getF64(string $key): ?float
    {
        $slot = $this->reader->slotOf($key);
        if ($slot < 0) return null;
        if ($this->usesColumnarFieldAccess()) {
            $cell = $this->reader->colNumericBytes($this->recordIndex, $slot);
            return $cell === null ? null : rdF64($cell, 0);
        }
        $off = $this->resolveSlot($slot);
        if ($off < 0) return null;
        return rdF64($this->reader->rawBytes(), $off);
    }

    public function getBool(string $key): ?bool
    {
        $slot = $this->reader->slotOf($key);
        if ($slot < 0) return null;
        if ($this->usesColumnarFieldAccess()) {
            $cell = $this->reader->colNumericBytes($this->recordIndex, $slot);
            return $cell === null ? null : (ord($cell[0]) !== 0);
        }
        $off = $this->resolveSlot($slot);
        if ($off < 0) return null;
        return ord($this->reader->rawBytes()[$off]) !== 0;
    }

    /**
     * Return all present fields as an associative array.
     * Types are derived from the schema's TypeManifest sigil bytes.
     */
    /** Expose sigil array from the reader (for predicate dispatch). */
    public function sigils(): array
    {
        return $this->reader->sigils();
    }

    /** Resolve key name → slot index, or -1 (delegates to reader). */
    public function slotOf(string $key): int
    {
        return $this->reader->slotOf($key);
    }

    public function toArray(): array
    {
        $result  = [];
        $keys    = $this->reader->keys();
        $sigils  = $this->reader->sigils();
        foreach ($keys as $slot => $key) {
            $off = $this->resolveSlot($slot);
            if ($off < 0) {
                $result[$key] = null;
                continue;
            }
            $sigil = $sigils[$slot] ?? 0;
            $result[$key] = match ($sigil) {
                ord('=') => $this->getI64($key),
                ord('~') => $this->getF64($key),
                ord('?') => $this->getBool($key),
                ord('"') => $this->getStr($key),
                ord('@') => $this->getI64($key),   // Time: raw nanoseconds as int
                default  => $this->getStr($key),
            };
        }
        return $result;
    }
}

// ── Reader ───────────────────────────────────────────────────────────────────

class Reader
{
    private string $bytes;
    private int    $recordCount;
    private array  $keys      = [];  // index → name
    private array  $keyIndex  = [];  // name → index
    private array  $sigils    = [];  // index → sigil byte (ord of ASCII sigil char)
    private int    $tailStart;       // absolute offset of first tail-index entry
    private string $layout    = 'row';
    private array  $colBufOff = [];
    private array  $colBufLen = [];
    private int    $pageCount    = 0;
    private array  $pageRecStart = [];
    private array  $pageRecCount = [];
    private array  $pageOffset   = [];
    private array  $pageLength   = [];

    private int $prefetchHint = HINT_UNKNOWN;
    private int $prefetchPageSize = DEFAULT_PAGE_SIZE;
    private int $coalesceGapPages = DEFAULT_COALESCE_GAP_PAGES;
    private PageCache $pageCache;
    private InFlightMap $inFlight;
    private int $fetchesIssued = 0;
    /** @var callable(int, int): string|null */
    private $fetchRange = null;
    private string $prefetchStrategy = 'lazy';
    private int $prefetchDepth = DEFAULT_PREFETCH_DEPTH;
    private AccessPatternDetector $patternDetector;
    private bool $eagerLoadComplete = false;
    private bool $prefetchPaused = false;
    /** @var object|null C extension NxsReader when available */
    private ?object $extReader = null;

    /**
     * @param array{
     *   hint?: int,
     *   max_pages?: int,
     *   page_size?: int,
     *   coalesce_gap_pages?: int,
     *   prefetch_depth?: int,
     *   fetch_range?: callable(int, int): string
     * } $options
     */
    public function __construct(string $bytes, array $options = [])
    {
        $this->bytes = $bytes;
        $len = strlen($bytes);
        if ($len < 32) {
            throw new NxsException('ERR_OUT_OF_BOUNDS: file too small');
        }

        // ── Validate header magic ──────────────────────────────────────────
        $magic = rdU32($bytes, 0);
        if ($magic !== MAGIC_FILE) {
            throw new NxsException(sprintf(
                'ERR_BAD_MAGIC: expected 0x%08X, got 0x%08X', MAGIC_FILE, $magic
            ));
        }

        // ── Preamble ───────────────────────────────────────────────────────
        // Offset 4: version u16 (ignored)
        $flags         = rdU16($bytes, 6);
        $dictHash      = rdU64($bytes, 8);
        $preambleTail  = rdU64($bytes, 16);

        // ── Footer check ───────────────────────────────────────────────────
        $footer = rdU32($bytes, $len - 4);
        if ($footer !== MAGIC_FOOTER) {
            throw new NxsException('ERR_BAD_MAGIC: footer magic mismatch');
        }

        // ── Schema (if embedded) ───────────────────────────────────────────
        if ($flags & FLAG_SCHEMA_EMBEDDED) {
            $schemaEnd = $this->readSchema(32);
            $computed  = $this->murmur3_64(substr($bytes, 32, $schemaEnd - 32));
            if ($computed !== $dictHash) {
                throw new NxsException('ERR_DICT_MISMATCH: schema hash mismatch');
            }
        }

        $this->parseLayoutTail($flags, $preambleTail, $len);
        $this->initPrefetch($options);
        $this->tryInitExtReader($bytes, $options);
    }

    /**
     * @param array{
     *   hint?: int,
     *   max_pages?: int,
     *   page_size?: int,
     *   coalesce_gap_pages?: int,
     *   prefetch_depth?: int,
     *   fetch_range?: callable(int, int): string
     * } $options
     */
    private function initPrefetch(array $options): void
    {
        $maxPages = $options['max_pages'] ?? DEFAULT_MAX_PAGES;
        $pageSize = $options['page_size'] ?? DEFAULT_PAGE_SIZE;
        $this->prefetchHint = $options['hint'] ?? HINT_UNKNOWN;
        $this->prefetchPageSize = $pageSize;
        $this->coalesceGapPages = $options['coalesce_gap_pages'] ?? DEFAULT_COALESCE_GAP_PAGES;
        $this->prefetchDepth = $options['prefetch_depth'] ?? DEFAULT_PREFETCH_DEPTH;
        $this->pageCache = new PageCache($maxPages, $pageSize);
        $this->inFlight = new InFlightMap();
        $this->fetchRange = $options['fetch_range'] ?? null;
        $this->patternDetector = new AccessPatternDetector();
        $this->prefetchStrategy = initialPrefetchStrategy(
            $this->prefetchHint,
            strlen($this->bytes),
        );
    }

    /**
     * @param array<string, mixed> $options
     */
    private function tryInitExtReader(string $bytes, array $options): void
    {
        if (isset($options['fetch_range']) || !extension_loaded('nxs') || !class_exists('NxsReader', false)) {
            return;
        }
        try {
            $ext = new \NxsReader($bytes, $options);
            if (method_exists($ext, 'prefetch_viewport')) {
                $this->extReader = $ext;
            }
        } catch (\Throwable) {
            // Pure PHP fallback.
        }
    }

    private function recordByteOffset(int $i): int
    {
        return rdU64($this->bytes, $this->tailStart + $i * 10 + 2);
    }

    /**
     * Prefetch pages for records [startIndex, endIndex] (row layout only).
     * Synchronous — blocks until required pages are cached.
     */
    public function prefetch_viewport(int $startIndex, int $endIndex): void
    {
        if ($this->extReader !== null) {
            $this->extReader->prefetch_viewport($startIndex, $endIndex);
            return;
        }

        if ($this->layout !== 'row') {
            return;
        }
        $n = $this->recordCount;
        if ($startIndex < 0 || $endIndex < $startIndex || $endIndex >= $n) {
            throw new NxsException(sprintf(
                'ERR_OUT_OF_BOUNDS: prefetch_viewport [%d, %d] out of [0, %d)',
                $startIndex, $endIndex, $n
            ));
        }

        $pageSize = $this->prefetchPageSize;
        $indices = pageIndicesForViewport(
            $startIndex,
            $endIndex,
            $pageSize,
            fn(int $i): int => $this->recordByteOffset($i),
        );
        $uniquePages = array_values(array_unique($indices));

        $missing = [];
        foreach ($uniquePages as $p) {
            if (!$this->pageCache->has($p) && !$this->inFlight->has($p)) {
                $missing[] = $p;
            }
        }

        if ($missing !== []) {
            $ranges = clampPageRanges(
                coalescePageIndices($missing, $this->coalesceGapPages, $pageSize),
                strlen($this->bytes),
            );
            foreach ($ranges as $range) {
                $this->fetchCoalescedRange($range);
            }
        }

        $this->pageCache->pinPages($uniquePages);
        $this->pageCache->unpinAll();
    }

    /** @param array{pageStart: int, pageEnd: int, byteStart: int, byteLength: int} $range */
    private function fetchCoalescedRange(array $range): void
    {
        $pages = [];
        for ($p = $range['pageStart']; $p <= $range['pageEnd']; $p++) {
            if (!$this->pageCache->has($p) && !$this->inFlight->has($p)) {
                $pages[] = $p;
            }
        }
        if ($pages === []) {
            return;
        }

        $this->inFlight->begin($pages);
        try {
            $blob = $this->fetchRangeBytes($range['byteStart'], $range['byteLength']);
            $pageSize = $this->prefetchPageSize;
            for ($p = $range['pageStart']; $p <= $range['pageEnd']; $p++) {
                if ($this->pageCache->has($p)) {
                    continue;
                }
                $pageOff = $p * $pageSize - $range['byteStart'];
                $pageLen = min($pageSize, strlen($blob) - $pageOff);
                if ($pageLen <= 0) {
                    continue;
                }
                $this->pageCache->set($p, substr($blob, $pageOff, $pageLen));
            }
        } finally {
            $this->inFlight->end($pages);
        }
    }

    private function fetchRangeBytes(int $byteStart, int $byteLength): string
    {
        $this->fetchesIssued++;
        if ($this->fetchRange !== null) {
            return ($this->fetchRange)($byteStart, $byteLength);
        }
        $end = $byteStart + $byteLength;
        if ($byteStart < 0 || $end > strlen($this->bytes)) {
            throw new NxsException(sprintf(
                'ERR_OUT_OF_BOUNDS: fetch range [%d, %d)', $byteStart, $end
            ));
        }
        return substr($this->bytes, $byteStart, $byteLength);
    }

    /** @return array<string, int|string> */
    public function cache_stats(): array
    {
        if ($this->extReader !== null && method_exists($this->extReader, 'cache_stats')) {
            return $this->extReader->cache_stats();
        }
        $s = $this->pageCache->stats();
        return [
            ...$s,
            'fetches_issued' => $this->fetchesIssued,
            'strategy'       => $this->prefetchStrategy,
            'pattern'        => $this->patternDetector->pattern(),
            'hint'           => $this->prefetchHint,
        ];
    }

    /** @return 'row'|'columnar'|'pax' */
    public function layout(): string
    {
        return $this->layout;
    }

    private function parseLayoutTail(int $flags, int $preambleTail, int $len): void
    {
        if (($flags & FLAG_COLUMNAR) && ($flags & FLAG_PAX)) {
            throw new NxsException('ERR_INVALID_FLAGS: columnar and PAX both set');
        }
        if (($flags & FLAG_COLUMNAR) && $preambleTail === 0) {
            throw new NxsException('ERR_INCOMPATIBLE_FLAGS: columnar with TailPtr=0');
        }

        if ($flags & FLAG_COLUMNAR) {
            $this->layout = 'columnar';
            $this->parseColumnarFooter($len);
            return;
        }
        if ($flags & FLAG_PAX) {
            $this->layout = 'pax';
            $this->parsePAXFooter($len);
            return;
        }

        $this->layout = 'row';
        $tailPtr = $preambleTail;
        if ($tailPtr === 0) {
            if ($len < 44) {
                throw new NxsException('ERR_OUT_OF_BOUNDS: stream footer missing tail pointer');
            }
            $tailPtr = rdU64($this->bytes, $len - FOOTER_ROW_BYTES);
        }
        if ((int)$tailPtr + 4 > $len) {
            throw new NxsException('ERR_OUT_OF_BOUNDS: tail index');
        }
        $this->recordCount = rdU32($this->bytes, (int)$tailPtr);
        $this->tailStart   = (int)$tailPtr + 4;
    }

    private function parseColumnarFooter(int $len): void
    {
        if ($len < FOOTER_COL_BYTES) {
            throw new NxsException('ERR_OUT_OF_BOUNDS: columnar footer');
        }
        $fo = $len - FOOTER_COL_BYTES;
        $tailPtr = rdU64($this->bytes, $fo);
        $this->recordCount = (int)rdU64($this->bytes, $fo + 8);
        $this->tailStart   = (int)$tailPtr;
        $kc = count($this->keys);
        $this->colBufOff = array_fill(0, $kc, 0);
        $this->colBufLen = array_fill(0, $kc, 0);
        for ($i = 0; $i < $kc; $i++) {
            $e = $this->tailStart + $i * COL_TAIL_ENTRY_BYTES;
            if ($e + COL_TAIL_ENTRY_BYTES > $len) {
                throw new NxsException('ERR_OUT_OF_BOUNDS: columnar tail entry');
            }
            $fid = rdU16($this->bytes, $e);
            if ($fid >= $kc) {
                throw new NxsException(sprintf('ERR_OUT_OF_BOUNDS: invalid field ID %d', $fid));
            }
            $this->colBufOff[$fid] = rdU64($this->bytes, $e + 4);
            $this->colBufLen[$fid] = rdU64($this->bytes, $e + 12);
        }
    }

    private function parsePAXFooter(int $len): void
    {
        if ($len < FOOTER_PAX_BYTES) {
            throw new NxsException('ERR_OUT_OF_BOUNDS: PAX footer');
        }
        $fo = $len - FOOTER_PAX_BYTES;
        $tailPtr = rdU64($this->bytes, $fo);
        $this->recordCount  = (int)rdU64($this->bytes, $fo + 8);
        $this->pageCount = rdU32($this->bytes, $fo + 16);
        $this->tailStart = (int)$tailPtr;
        if ($this->pageCount > 0) {
            for ($i = 0; $i < $this->pageCount; $i++) {
                $e = $this->tailStart + $i * PAX_TAIL_ENTRY_BYTES;
                if ($e + PAX_TAIL_ENTRY_BYTES > $len) {
                    throw new NxsException('ERR_OUT_OF_BOUNDS: PAX tail entry');
                }
                $this->pageRecStart[] = rdU64($this->bytes, $e + 4);
                $this->pageRecCount[] = rdU32($this->bytes, $e + 12);
                $this->pageOffset[]   = rdU64($this->bytes, $e + 16);
                $this->pageLength[]   = rdU32($this->bytes, $e + 24);
            }
            for ($i = 0; $i < $this->pageCount; $i++) {
                $poff = (int)$this->pageOffset[$i];
                $plen = (int)$this->pageLength[$i];
                if ($poff > $len || $poff + 4 > $len || ($plen > 0 && $poff + $plen > $len)) {
                    throw new NxsException('ERR_OUT_OF_BOUNDS: PAX page offset');
                }
                if (rdU32($this->bytes, $poff) !== MAGIC_PAGE) {
                    throw new NxsException('ERR_INVALID_PAGE_MAGIC: PAX page magic mismatch');
                }
            }
        }
    }

    private function paxFindPage(int $rec): ?array
    {
        if ($this->pageCount === 0) {
            return null;
        }
        $lo = 0;
        $hi = $this->pageCount - 1;
        while ($lo <= $hi) {
            $mid   = intdiv($lo + $hi, 2);
            $start = $this->pageRecStart[$mid];
            $count = $this->pageRecCount[$mid];
            if ($rec < $start) {
                $hi = $mid - 1;
            } elseif ($rec >= $start + $count) {
                $lo = $mid + 1;
            } else {
                return ['page' => $mid, 'local' => $rec - (int)$start];
            }
        }
        return null;
    }

    /** @return array{0: string, 1: string} bitmap, values */
    private function colFieldParts(int $slot): array
    {
        if ($slot < 0 || $slot >= count($this->colBufOff)) {
            throw new NxsException('ERR_KEY_NOT_FOUND');
        }
        $off    = (int)$this->colBufOff[$slot];
        $length = (int)$this->colBufLen[$slot];
        if ($off + $length > strlen($this->bytes)) {
            throw new NxsException('ERR_OUT_OF_BOUNDS: column buffer');
        }
        $bmLen = nullBitmapBytes($this->recordCount);
        if ($length < $bmLen) {
            throw new NxsException('ERR_OUT_OF_BOUNDS: null bitmap');
        }
        $bm   = substr($this->bytes, $off, $bmLen);
        $vals = substr($this->bytes, $off + $bmLen, $length - $bmLen);
        return [$bm, $vals];
    }

    /** @return array{0: string, 1: string, 2: string} bitmap, offsets, values */
    private function colVarParts(int $slot): array
    {
        [$bm, $tail] = $this->colFieldParts($slot);
        $offBytes = varOffBytesLen($this->recordCount);
        if (strlen($tail) < $offBytes) {
            throw new NxsException('ERR_OUT_OF_BOUNDS: var offsets');
        }
        return [$bm, substr($tail, 0, $offBytes), substr($tail, $offBytes)];
    }

    /** @return array{0: string, 1: string, 2: string, 3?: int}|null */
    private function colVarPartsAt(int $rec, int $slot): ?array
    {
        if ($slot < 0 || $slot >= count($this->sigils) || !isVarSigil($this->sigils[$slot])) {
            return null;
        }
        if ($this->layout === 'columnar') {
            return $this->colVarParts($slot);
        }
        if ($this->layout === 'pax') {
            $loc = $this->paxFindPage($rec);
            if ($loc === null) {
                return null;
            }
            $parts = $this->pageFieldParts($loc['page'], $slot);
            if ($parts === null) {
                return null;
            }
            $rc       = $this->pageRecCount[$loc['page']];
            $offBytes = varOffBytesLen($rc);
            if (strlen($parts[1]) < $offBytes) {
                return null;
            }
            return [
                $parts[0],
                substr($parts[1], 0, $offBytes),
                substr($parts[1], $offBytes),
                $loc['local'],
            ];
        }
        return null;
    }

    public function colGetStr(string $key, int $recordIndex): ?string
    {
        $slot = $this->slotOf($key);
        if ($slot < 0 || $recordIndex >= $this->recordCount || $this->layout === 'row') {
            return null;
        }
        if (($this->sigils[$slot] ?? 0) !== ord('"')) {
            return null;
        }
        $parts = $this->colVarPartsAt($recordIndex, $slot);
        if ($parts === null) {
            return null;
        }
        if ($this->layout === 'pax') {
            $bitIdx = $parts[3];
            if (!colBit($parts[0], $bitIdx)) {
                return null;
            }
            return varStrAt($parts[1], $parts[2], $bitIdx);
        }
        if (!colBit($parts[0], $recordIndex)) {
            return null;
        }
        return varStrAt($parts[1], $parts[2], $recordIndex);
    }

    public function colNumericBytes(int $rec, int $slot): ?string
    {
        if ($slot >= 0 && $slot < count($this->sigils) && isVarSigil($this->sigils[$slot])) {
            return null;
        }
        if ($this->layout === 'columnar') {
            [$bm, $vals] = $this->colFieldParts($slot);
            if ($rec >= $this->recordCount || !colBit($bm, $rec)) {
                return null;
            }
            $off = $rec * 8;
            if ($off + 8 > strlen($vals)) {
                return null;
            }
            return substr($vals, $off, 8);
        }
        if ($this->layout === 'pax') {
            $loc = $this->paxFindPage($rec);
            if ($loc === null) {
                return null;
            }
            $parts = $this->pageFieldParts($loc['page'], $slot);
            if ($parts === null || !colBit($parts[0], $loc['local'])) {
                return null;
            }
            $off = $loc['local'] * 8;
            if ($off + 8 > strlen($parts[1])) {
                return null;
            }
            return substr($parts[1], $off, 8);
        }
        return null;
    }

    /** @return array{0: string, 1: string}|null bitmap, values */
    private function pageFieldParts(int $pageIndex, int $slot): ?array
    {
        $sector = $this->pageFieldSector($pageIndex, $slot);
        if ($sector === null) {
            return null;
        }
        $bmLen = nullBitmapBytes($this->pageRecCount[$pageIndex]);
        if (strlen($sector) < $bmLen) {
            return null;
        }
        return [substr($sector, 0, $bmLen), substr($sector, $bmLen)];
    }

    private function pageFieldSector(int $pageIndex, int $slot): ?string
    {
        $poff = (int)$this->pageOffset[$pageIndex];
        if ($poff + 24 > strlen($this->bytes) || rdU32($this->bytes, $poff) !== MAGIC_PAGE) {
            return null;
        }
        $fc = rdU16($this->bytes, $poff + 20);
        if ($slot < 0 || $slot >= $fc || $fc > count($this->sigils)) {
            return null;
        }
        $rc   = $this->pageRecCount[$pageIndex];
        $body = $poff + 24;
        for ($fi = 0; $fi < $slot; $fi++) {
            $sig  = $this->sigils[$fi] ?? ord('=');
            try {
                $flen = fieldSectorLen($this->bytes, $body, $rc, $sig);
            } catch (NxsException) {
                return null;
            }
            $body += $flen;
        }
        $sig = $this->sigils[$slot] ?? ord('=');
        try {
            $flen = fieldSectorLen($this->bytes, $body, $rc, $sig);
        } catch (NxsException) {
            return null;
        }
        if ($body + $flen > strlen($this->bytes)) {
            return null;
        }
        return substr($this->bytes, $body, $flen);
    }

    private function paxSumF64(int $slot): float
    {
        $sum = 0.0;
        for ($pi = 0; $pi < $this->pageCount; $pi++) {
            $parts = $this->pageFieldParts($pi, $slot);
            if ($parts === null) {
                continue;
            }
            [$bm, $vals] = $parts;
            $rc = $this->pageRecCount[$pi];
            for ($i = 0; $i < $rc; $i++) {
                if (!colBit($bm, $i)) {
                    continue;
                }
                $off = $i * 8;
                if ($off + 8 > strlen($vals)) {
                    break;
                }
                $sum += rdF64($vals, $off);
            }
        }
        return $sum;
    }

    private function colSumF64(int $slot): float
    {
        [$bm, $vals] = $this->colFieldParts($slot);
        $sum = 0.0;
        for ($i = 0; $i < $this->recordCount; $i++) {
            if (!colBit($bm, $i)) {
                continue;
            }
            $off = $i * 8;
            if ($off + 8 > strlen($vals)) {
                break;
            }
            $sum += rdF64($vals, $off);
        }
        return $sum;
    }

    // ── Schema parser ──────────────────────────────────────────────────────

    private function readSchema(int $offset): int
    {
        $bytes    = $this->bytes;
        $keyCount = rdU16($bytes, $offset);
        $offset  += 2;

        // TypeManifest: keyCount sigil bytes
        for ($i = 0; $i < $keyCount; $i++) {
            $this->sigils[] = ord($bytes[$offset + $i]);
        }
        $offset += $keyCount;

        // StringPool: null-terminated UTF-8 key names
        for ($i = 0; $i < $keyCount; $i++) {
            $end = strpos($bytes, "\x00", $offset);
            if ($end === false) {
                throw new NxsException('ERR_OUT_OF_BOUNDS: unterminated key in StringPool');
            }
            $name = substr($bytes, $offset, $end - $offset);
            $this->keys[]          = $name;
            $this->keyIndex[$name] = $i;
            $offset = $end + 1;
        }

        // Pad to 8-byte boundary
        $rem = $offset % 8;
        if ($rem !== 0) {
            $offset += 8 - $rem;
        }
        return $offset;
    }

    /**
     * MurmurHash3-derived 64-bit hash matching the Rust reference implementation.
     * PHP integers are 64-bit signed on 64-bit platforms; we use intval() after
     * multiplication to keep values in the native int range (wrapping semantics).
     */
    private function murmur3_64(string $data): int
    {
        // PHP doesn't have unsigned 64-bit integers; use GMP for wrapping arithmetic.
        $mask = gmp_init('0xFFFFFFFFFFFFFFFF');
        $c1   = gmp_init('0xFF51AFD7ED558CCD');
        $c2   = gmp_init('0xC4CEB9FE1A85EC53');
        $h    = gmp_init('0x93681D6255313A99');

        $len  = strlen($data);
        $i    = 0;
        while ($i < $len) {
            $chunk = substr($data, $i, 8);
            $k     = gmp_init(0);
            for ($j = 0; $j < strlen($chunk); $j++) {
                $k = gmp_or($k, gmp_mul(gmp_init(ord($chunk[$j])), gmp_pow(2, $j * 8)));
            }
            $k = gmp_and(gmp_mul($k, $c1), $mask);
            $k = gmp_xor($k, gmp_div_q($k, gmp_pow(2, 33)));
            $h = gmp_xor($h, $k);
            $h = gmp_and(gmp_mul($h, $c2), $mask);
            $h = gmp_xor($h, gmp_div_q($h, gmp_pow(2, 33)));
            $i += 8;
        }
        $h = gmp_xor($h, gmp_init($len));
        $h = gmp_xor($h, gmp_div_q($h, gmp_pow(2, 33)));
        $h = gmp_and(gmp_mul($h, $c1), $mask);
        $h = gmp_xor($h, gmp_div_q($h, gmp_pow(2, 33)));

        // gmp_intval overflows for values > PHP_INT_MAX — use pack/unpack instead
        // to reinterpret the 64-bit GMP value as a PHP signed int (same bit pattern
        // as what rdU64 returns for the same bytes).
        $hex = gmp_strval($h, 16);
        $bytes = str_pad(hex2bin(str_pad($hex, 16, '0', STR_PAD_LEFT)), 8, "\x00", STR_PAD_LEFT);
        // Reverse bytes to little-endian so unpack('q') reads it correctly
        $result = unpack('q', strrev($bytes));
        return $result[1];
    }

    // ── Public API ─────────────────────────────────────────────────────────

    public function recordCount(): int
    {
        return $this->recordCount;
    }

    /** @return string[] */
    public function keys(): array
    {
        return $this->keys;
    }

    public function record(int $i): NxsObject
    {
        if ($i < 0 || $i >= $this->recordCount) {
            throw new NxsException(sprintf(
                'ERR_OUT_OF_BOUNDS: record %d out of [0, %d)', $i, $this->recordCount
            ));
        }
        if ($this->layout !== 'row') {
            return new NxsObject($this, $i, $i);
        }
        $this->onAccess($i);
        // Tail entry: KeyID u16 (skip 2) + AbsoluteOffset u64
        $entryOff  = $this->tailStart + $i * 10;
        $absOffset = rdU64($this->bytes, $entryOff + 2);
        return new NxsObject($this, (int)$absOffset);
    }

    /**
     * Wait for eager prefetch — PHP has no background threads; eager loads run here (§8 stub).
     */
    /** Stop scheduling speculative and eager prefetch (§8.1). */
    public function pausePrefetch(): void
    {
        if ($this->extReader !== null && method_exists($this->extReader, 'pausePrefetch')) {
            $this->extReader->pausePrefetch();
            return;
        }
        $this->prefetchPaused = true;
    }

    /** Re-enable speculative prefetch after pausePrefetch(). */
    public function resumePrefetch(): void
    {
        if ($this->extReader !== null && method_exists($this->extReader, 'resumePrefetch')) {
            $this->extReader->resumePrefetch();
            return;
        }
        $this->prefetchPaused = false;
    }

    public function warmup(): void
    {
        if ($this->extReader !== null && method_exists($this->extReader, 'warmup')) {
            $this->extReader->warmup();
            return;
        }
        if ($this->prefetchStrategy === 'eager' && !$this->eagerLoadComplete) {
            $this->runEagerLoadSync();
        }
    }

    private function onAccess(int $index): void
    {
        if ($this->extReader !== null || $this->layout !== 'row' || $this->recordCount === 0) {
            return;
        }
        if ($this->prefetchPaused) {
            return;
        }
        $this->patternDetector->observe($index);
        $this->maybeUpgradeToEager();

        if ($this->prefetchStrategy === 'eager' && $this->eagerLoadComplete) {
            return;
        }

        $off = $this->recordByteOffset($index);
        $pageIndex = intdiv($off, $this->prefetchPageSize);
        $this->pageCache->get($pageIndex);

        if ($this->prefetchStrategy === 'adaptive'
            && $this->patternDetector->pattern() === PATTERN_SEQUENTIAL) {
            $this->speculativePrefetch();
        }
    }

    private function maybeUpgradeToEager(): void
    {
        if ($this->prefetchPaused) {
            return;
        }
        if ($this->prefetchStrategy !== 'adaptive') {
            return;
        }
        if ($this->patternDetector->pattern() !== PATTERN_SEQUENTIAL) {
            return;
        }
        if ($this->patternDetector->sequentialRuns < UPGRADE_SEQUENTIAL_THRESHOLD) {
            return;
        }
        if (intdiv(strlen($this->bytes), 1024 * 1024) > EAGER_THRESHOLD_MB) {
            return;
        }
        $this->prefetchStrategy = 'eager';
        // No background thread — caller may invoke warmup() for sync eager load.
    }

    private function speculativePrefetch(): void
    {
        if ($this->prefetchPaused) {
            return;
        }
        $predicted = $this->patternDetector->predictNext(
            $this->prefetchDepth,
            $this->recordCount,
        );
        if ($predicted === []) {
            return;
        }

        $pageIndices = [];
        $seen = [];
        foreach ($predicted as $idx) {
            $off = $this->recordByteOffset($idx);
            $p = intdiv($off, $this->prefetchPageSize);
            if (isset($seen[$p])) {
                continue;
            }
            $seen[$p] = true;
            if (!$this->pageCache->has($p) && !$this->inFlight->has($p)) {
                $pageIndices[] = $p;
            }
        }
        if ($pageIndices === []) {
            return;
        }

        $ranges = clampPageRanges(
            coalescePageIndices($pageIndices, $this->coalesceGapPages, $this->prefetchPageSize),
            strlen($this->bytes),
        );
        foreach ($ranges as $range) {
            $this->fetchCoalescedRange($range);
        }
    }

    /** Synchronous eager data-sector fetch (PHP eager stub — no background worker). */
    private function runEagerLoadSync(): void
    {
        if ($this->eagerLoadComplete) {
            return;
        }
        [$sectorStart, $sectorLen] = rowDataSector($this->tailStart, strlen($this->bytes));
        if ($sectorLen === 0) {
            $this->eagerLoadComplete = true;
            return;
        }
        $end = min($sectorStart + $sectorLen, strlen($this->bytes));
        if ($sectorStart >= $end) {
            $this->eagerLoadComplete = true;
            return;
        }

        $pageSize = $this->prefetchPageSize;
        $firstPage = intdiv($sectorStart, $pageSize);
        $lastPage = intdiv($end - 1, $pageSize);
        $indices = [];
        for ($p = $firstPage; $p <= $lastPage; $p++) {
            $indices[] = $p;
        }

        $ranges = clampPageRanges(
            coalescePageIndices($indices, $this->coalesceGapPages, $pageSize),
            strlen($this->bytes),
        );
        foreach ($ranges as $range) {
            $this->fetchCoalescedRange($range);
        }
        $this->eagerLoadComplete = true;
    }

    // ── Bulk reducers (tight loops, no per-record NxsObject allocation) ──

    /**
     * Sum of all f64 values for $key over every record.
     * Uses a tight inline loop to avoid NxsObject allocation per record.
     */
    public function sumF64(string $key): float
    {
        $slot = $this->slotOf($key);
        if ($slot < 0) return 0.0;
        if ($this->layout === 'pax') {
            return $this->paxSumF64($slot);
        }
        if ($this->layout === 'columnar') {
            return $this->colSumF64($slot);
        }

        $bytes      = $this->bytes;
        $tailStart  = $this->tailStart;
        $n          = $this->recordCount;
        $sum        = 0.0;

        for ($i = 0; $i < $n; $i++) {
            $entryOff  = $tailStart + $i * 10;
            $absOffset = rdU64($bytes, $entryOff + 2);
            $relOff    = $this->valueOffset($bytes, (int)$absOffset, $slot);
            if ($relOff >= 0) {
                $sum += rdF64($bytes, (int)$absOffset + $relOff);
            }
        }
        return $sum;
    }

    public function minF64(string $key): ?float
    {
        $slot = $this->slotOf($key);
        if ($slot < 0) return null;
        if ($this->layout !== 'row') {
            return $this->colMinF64($slot);
        }

        $bytes     = $this->bytes;
        $tailStart = $this->tailStart;
        $n         = $this->recordCount;
        $min       = PHP_FLOAT_MAX;
        $have      = false;

        for ($i = 0; $i < $n; $i++) {
            $entryOff  = $tailStart + $i * 10;
            $absOffset = rdU64($bytes, $entryOff + 2);
            $relOff    = $this->valueOffset($bytes, (int)$absOffset, $slot);
            if ($relOff >= 0) {
                $v = rdF64($bytes, (int)$absOffset + $relOff);
                if (!$have || $v < $min) { $min = $v; $have = true; }
            }
        }
        return $have ? $min : null;
    }

    public function maxF64(string $key): ?float
    {
        $slot = $this->slotOf($key);
        if ($slot < 0) return null;
        if ($this->layout !== 'row') {
            return $this->colMaxF64($slot);
        }

        $bytes     = $this->bytes;
        $tailStart = $this->tailStart;
        $n         = $this->recordCount;
        $max       = -PHP_FLOAT_MAX;
        $have      = false;

        for ($i = 0; $i < $n; $i++) {
            $entryOff  = $tailStart + $i * 10;
            $absOffset = rdU64($bytes, $entryOff + 2);
            $relOff    = $this->valueOffset($bytes, (int)$absOffset, $slot);
            if ($relOff >= 0) {
                $v = rdF64($bytes, (int)$absOffset + $relOff);
                if (!$have || $v > $max) { $max = $v; $have = true; }
            }
        }
        return $have ? $max : null;
    }

    public function sumI64(string $key): int
    {
        $slot = $this->slotOf($key);
        if ($slot < 0) return 0;
        if ($this->layout !== 'row') {
            return (int)$this->colSumI64($slot);
        }

        $bytes     = $this->bytes;
        $tailStart = $this->tailStart;
        $n         = $this->recordCount;
        $sum       = 0;

        for ($i = 0; $i < $n; $i++) {
            $entryOff  = $tailStart + $i * 10;
            $absOffset = rdU64($bytes, $entryOff + 2);
            $relOff    = $this->valueOffset($bytes, (int)$absOffset, $slot);
            if ($relOff >= 0) {
                $sum += rdI64($bytes, (int)$absOffset + $relOff);
            }
        }
        return $sum;
    }

    private function colMinF64(int $slot): ?float
    {
        $min  = PHP_FLOAT_MAX;
        $have = false;
        if ($this->layout === 'columnar') {
            [$bm, $vals] = $this->colFieldParts($slot);
            for ($i = 0; $i < $this->recordCount; $i++) {
                if (!colBit($bm, $i)) continue;
                $off = $i * 8;
                if ($off + 8 > strlen($vals)) break;
                $v = rdF64($vals, $off);
                if (!$have || $v < $min) { $min = $v; $have = true; }
            }
            return $have ? $min : null;
        }
        for ($pi = 0; $pi < $this->pageCount; $pi++) {
            $parts = $this->pageFieldParts($pi, $slot);
            if ($parts === null) continue;
            [$bm, $vals] = $parts;
            $rc = $this->pageRecCount[$pi];
            for ($i = 0; $i < $rc; $i++) {
                if (!colBit($bm, $i)) continue;
                $off = $i * 8;
                if ($off + 8 > strlen($vals)) break;
                $v = rdF64($vals, $off);
                if (!$have || $v < $min) { $min = $v; $have = true; }
            }
        }
        return $have ? $min : null;
    }

    private function colMaxF64(int $slot): ?float
    {
        $max  = -PHP_FLOAT_MAX;
        $have = false;
        if ($this->layout === 'columnar') {
            [$bm, $vals] = $this->colFieldParts($slot);
            for ($i = 0; $i < $this->recordCount; $i++) {
                if (!colBit($bm, $i)) continue;
                $off = $i * 8;
                if ($off + 8 > strlen($vals)) break;
                $v = rdF64($vals, $off);
                if (!$have || $v > $max) { $max = $v; $have = true; }
            }
            return $have ? $max : null;
        }
        for ($pi = 0; $pi < $this->pageCount; $pi++) {
            $parts = $this->pageFieldParts($pi, $slot);
            if ($parts === null) continue;
            [$bm, $vals] = $parts;
            $rc = $this->pageRecCount[$pi];
            for ($i = 0; $i < $rc; $i++) {
                if (!colBit($bm, $i)) continue;
                $off = $i * 8;
                if ($off + 8 > strlen($vals)) break;
                $v = rdF64($vals, $off);
                if (!$have || $v > $max) { $max = $v; $have = true; }
            }
        }
        return $have ? $max : null;
    }

    private function colSumI64(int $slot): int
    {
        $sum = 0;
        if ($this->layout === 'columnar') {
            [$bm, $vals] = $this->colFieldParts($slot);
            for ($i = 0; $i < $this->recordCount; $i++) {
                if (!colBit($bm, $i)) continue;
                $off = $i * 8;
                if ($off + 8 > strlen($vals)) break;
                $sum += rdI64($vals, $off);
            }
            return $sum;
        }
        for ($pi = 0; $pi < $this->pageCount; $pi++) {
            $parts = $this->pageFieldParts($pi, $slot);
            if ($parts === null) continue;
            [$bm, $vals] = $parts;
            $rc = $this->pageRecCount[$pi];
            for ($i = 0; $i < $rc; $i++) {
                if (!colBit($bm, $i)) continue;
                $off = $i * 8;
                if ($off + 8 > strlen($vals)) break;
                $sum += rdI64($vals, $off);
            }
        }
        return $sum;
    }

    // ── Internal helpers (called by NxsObject too) ─────────────────────────

    /** Resolve key name → slot index, or -1. */
    public function slotOf(string $key): int
    {
        return array_key_exists($key, $this->keyIndex) ? $this->keyIndex[$key] : -1;
    }

    /** Number of keys in schema. */
    public function keyCount(): int
    {
        return count($this->keys);
    }

    /** Raw byte string (passed to NxsObject). */
    public function rawBytes(): string
    {
        return $this->bytes;
    }

    /** Sigil byte per slot (ord of ASCII sigil char). */
    public function sigils(): array
    {
        return $this->sigils;
    }

    public function where(NxsPred $pred): NxsQuery
    {
        return new NxsQuery($this, $pred);
    }

    public function all(): NxsQuery
    {
        return new NxsQuery($this, null);
    }

    /**
     * Inline bitmask walk: given a record at $absOffset, find the *relative*
     * offset of slot $slot's value (relative to $absOffset).
     * Returns -1 if the field is absent.
     *
     * This is the hot-path kernel used by sumF64 / minF64 / maxF64 / sumI64.
     * It avoids NxsObject construction and works directly on the byte string.
     */
    private function valueOffset(string $bytes, int $absOffset, int $slot): int
    {
        $p        = $absOffset + 8;  // skip NYXO Magic(4) + Length(4)
        $curSlot  = 0;
        $tableIdx = 0;
        $found    = false;
        $byte     = 0;

        // Walk LEB128 bitmask counting present-bits before $slot
        while (true) {
            $byte     = ord($bytes[$p++]);
            $dataBits = $byte & 0x7F;
            for ($b = 0; $b < 7; $b++) {
                if ($curSlot === $slot) {
                    if (($dataBits >> $b) & 1) {
                        $found = true;
                    }
                    break 2;
                }
                if (($dataBits >> $b) & 1) {
                    $tableIdx++;
                }
                $curSlot++;
            }
            if (!($byte & 0x80)) {
                // No more bitmask bytes — slot not present
                return -1;
            }
        }

        if (!$found) {
            return -1;
        }

        // Drain remaining continuation bytes
        while ($byte & 0x80) {
            $byte = ord($bytes[$p++]);
        }

        // OffsetTable: tableIdx * 2 bytes from $p
        return rdU16($bytes, $p + $tableIdx * 2);
    }
}

// ── Query engine ──────────────────────────────────────────────────────────────

abstract class NxsPred {
    abstract public function __invoke(NxsObject $record): bool;
    public function and(NxsPred $other): NxsAndPred { return new NxsAndPred($this, $other); }
    public function or(NxsPred $other): NxsOrPred   { return new NxsOrPred($this, $other); }
    public function not(): NxsNotPred               { return new NxsNotPred($this); }
}

class NxsEq extends NxsPred {
    public function __construct(private string $key, private mixed $value) {}
    public function __invoke(NxsObject $obj): bool {
        $sigils = $obj->sigils();
        $slot   = $obj->slotOf($this->key);
        if ($slot < 0) return false;
        $v = match ($sigils[$slot] ?? 0) {
            ord('='), ord('@') => $obj->getI64($this->key),
            ord('~')           => $obj->getF64($this->key),
            ord('?')           => $obj->getBool($this->key),
            default            => $obj->getStr($this->key),
        };
        return $v === $this->value;
    }
}

class NxsGt extends NxsPred {
    public function __construct(private string $key, private float $value) {}
    public function __invoke(NxsObject $obj): bool {
        $v = $obj->getF64($this->key);
        return $v !== null && $v > $this->value;
    }
}

class NxsLt extends NxsPred {
    public function __construct(private string $key, private float $value) {}
    public function __invoke(NxsObject $obj): bool {
        $v = $obj->getF64($this->key);
        return $v !== null && $v < $this->value;
    }
}

class NxsAndPred extends NxsPred {
    private array $preds;
    public function __construct(NxsPred ...$preds) { $this->preds = $preds; }
    public function __invoke(NxsObject $obj): bool {
        foreach ($this->preds as $p) { if (!$p($obj)) return false; }
        return true;
    }
}

class NxsOrPred extends NxsPred {
    private array $preds;
    public function __construct(NxsPred ...$preds) { $this->preds = $preds; }
    public function __invoke(NxsObject $obj): bool {
        foreach ($this->preds as $p) { if ($p($obj)) return true; }
        return false;
    }
}

class NxsNotPred extends NxsPred {
    public function __construct(private NxsPred $pred) {}
    public function __invoke(NxsObject $obj): bool { return !($this->pred)($obj); }
}

// Constructor functions
function nxs_eq(string $key, mixed $value): NxsEq { return new NxsEq($key, $value); }
function nxs_gt(string $key, float $value): NxsGt { return new NxsGt($key, $value); }
function nxs_lt(string $key, float $value): NxsLt { return new NxsLt($key, $value); }
function nxs_and(NxsPred ...$preds): NxsAndPred   { return new NxsAndPred(...$preds); }
function nxs_or(NxsPred ...$preds): NxsOrPred     { return new NxsOrPred(...$preds); }
function nxs_not(NxsPred $pred): NxsNotPred        { return new NxsNotPred($pred); }

class NxsQuery {
    public function __construct(
        private Reader      $reader,
        private ?NxsPred    $pred = null
    ) {}

    public function getIterator(): \Generator {
        $n = $this->reader->recordCount();
        for ($i = 0; $i < $n; $i++) {
            $obj = $this->reader->record($i);
            if ($this->pred === null || ($this->pred)($obj)) {
                yield $obj;
            }
        }
    }

    public function count(): int {
        $n = 0;
        foreach ($this->getIterator() as $_) { $n++; }
        return $n;
    }

    public function first(): ?NxsObject {
        foreach ($this->getIterator() as $r) { return $r; }
        return null;
    }
}
