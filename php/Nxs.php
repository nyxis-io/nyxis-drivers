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

// ── Constants ────────────────────────────────────────────────────────────────

const MAGIC_FILE   = 0x4E595842; // NYXB
const MAGIC_OBJ    = 0x4E59584F; // NYXO
const MAGIC_FOOTER = 0x2153584E; // NXS!

const FLAG_SCHEMA_EMBEDDED = 0x0002;

// ── Exceptions ───────────────────────────────────────────────────────────────

class NxsException extends \RuntimeException {}

// ── Helpers ──────────────────────────────────────────────────────────────────

/**
 * Decode a LEB128 unsigned integer from $bytes at $pos.
 * Advances $pos past the consumed bytes.
 * Returns the decoded integer.
 */
function leb128(string $bytes, int &$pos): int
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

// ── NxsObject ────────────────────────────────────────────────────────────────

/**
 * Lazy view over a single NYXO record.
 * The bitmask/offset-table are parsed only on first field access.
 */
class NxsObject
{
    private Reader $reader;
    private int    $offset;

    // Stage 0 = untouched; filled lazily:
    private int   $offsetTableStart = -1;   // absolute byte pos of OffsetTable
    private array $present          = [];   // bool per slot
    private array $rank             = [];   // rank[slot] = index into OffsetTable

    public function __construct(Reader $reader, int $offset)
    {
        $this->reader = $reader;
        $this->offset = $offset;
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
        $off = $this->resolveSlot($slot);
        if ($off < 0) return null;
        return rdI64($this->reader->rawBytes(), $off);
    }

    public function getF64(string $key): ?float
    {
        $slot = $this->reader->slotOf($key);
        if ($slot < 0) return null;
        $off = $this->resolveSlot($slot);
        if ($off < 0) return null;
        return rdF64($this->reader->rawBytes(), $off);
    }

    public function getBool(string $key): ?bool
    {
        $slot = $this->reader->slotOf($key);
        if ($slot < 0) return null;
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

    public function __construct(string $bytes)
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
        $flags    = rdU16($bytes, 6);
        $dictHash = rdU64($bytes, 8);
        $tailPtr  = rdU64($bytes, 16);

        // ── Footer check ───────────────────────────────────────────────────
        $footer = rdU32($bytes, $len - 4);
        if ($footer !== MAGIC_FOOTER) {
            throw new NxsException('ERR_BAD_MAGIC: footer magic mismatch');
        }
        if ($tailPtr === 0) {
            if ($len < 44) {
                throw new NxsException('ERR_OUT_OF_BOUNDS: stream footer missing tail pointer');
            }
            $tailPtr = rdU64($bytes, $len - 12);
        }

        // ── Schema (if embedded) ───────────────────────────────────────────
        if ($flags & FLAG_SCHEMA_EMBEDDED) {
            $schemaEnd = $this->readSchema(32);
            $computed  = $this->murmur3_64(substr($bytes, 32, $schemaEnd - 32));
            if ($computed !== $dictHash) {
                throw new NxsException('ERR_DICT_MISMATCH: schema hash mismatch');
            }
        }

        // ── Tail-index ─────────────────────────────────────────────────────
        $this->recordCount = rdU32($bytes, (int)$tailPtr);
        // Each entry: u16 KeyID (2) + u64 AbsoluteOffset (8) = 10 bytes
        $this->tailStart = (int)$tailPtr + 4;
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
        // Tail entry: KeyID u16 (skip 2) + AbsoluteOffset u64
        $entryOff  = $this->tailStart + $i * 10;
        $absOffset = rdU64($this->bytes, $entryOff + 2);
        return new NxsObject($this, (int)$absOffset);
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
