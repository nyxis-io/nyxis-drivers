<?php

/**
 * NXS Writer — direct-to-buffer .nxb emitter for PHP 8.0+.
 *
 * Mirrors the Rust NxsWriter API:
 *   NxsSchema — precompile keys once; share across NxsWriter instances.
 *   NxsWriter — slot-based hot path; no per-key hash lookups during write.
 *
 * Usage:
 *   require_once __DIR__ . '/NxsWriter.php';
 *
 *   $schema = new Nxs\Schema(['id', 'username', 'score', 'active']);
 *   $w = new Nxs\Writer($schema);
 *   $w->beginObject();
 *   $w->writeI64(0, 42);
 *   $w->writeStr(1, 'alice');
 *   $w->writeF64(2, 9.5);
 *   $w->writeBool(3, true);
 *   $w->endObject();
 *   $bytes = $w->finish();   // binary string
 */

namespace Nxs;

const WRITER_MAGIC_FILE    = 0x4E595842;
const WRITER_MAGIC_OBJ     = 0x4E59584F;
const WRITER_MAGIC_LIST    = 0x4E59584C;
const WRITER_MAGIC_FOOTER  = 0x2153584E;
const WRITER_VERSION       = 0x0101;
const WRITER_FLAG_SCHEMA   = 0x0002;

// ── MurmurHash3-64 ────────────────────────────────────────────────────────────

function _murmur3_64(string $data): string
{
    $mask = gmp_init('0xFFFFFFFFFFFFFFFF');
    $c1   = gmp_init('0xFF51AFD7ED558CCD');
    $c2   = gmp_init('0xC4CEB9FE1A85EC53');
    $h    = gmp_init('0x93681D6255313A99');
    $len  = strlen($data);
    $i    = 0;
    while ($i < $len) {
        $k = gmp_init(0);
        for ($b = 0; $b < 8 && $i + $b < $len; $b++) {
            $k = gmp_or($k, gmp_mul(gmp_init(ord($data[$i + $b])), gmp_pow(2, $b * 8)));
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
    // Emit as 8 little-endian bytes matching the reader's pack layout
    $hex   = str_pad(gmp_strval($h, 16), 16, '0', STR_PAD_LEFT);
    $bytes = hex2bin($hex);           // 8 bytes, big-endian
    return strrev($bytes);            // → little-endian
}

// ── Schema ────────────────────────────────────────────────────────────────────

class Schema
{
    public readonly array $keys;
    public readonly int   $bitmaskBytes;

    public function __construct(array $keys)
    {
        $this->keys         = $keys;
        $this->bitmaskBytes = max(1, intdiv(count($keys) + 6, 7));
    }

    public function length(): int { return count($this->keys); }
}

// ── Frame (per open object) ───────────────────────────────────────────────────

class _Frame
{
    public int    $start;
    public string $bitmask;
    public array  $offsetTable = [];
    public array  $slotOffsets = [];  // [[slot, bufOff], ...]
    public int    $lastSlot    = -1;
    public bool   $needsSort   = false;

    public function __construct(int $start, string $bitmask)
    {
        $this->start   = $start;
        $this->bitmask = $bitmask;
    }
}

// ── Writer ────────────────────────────────────────────────────────────────────

const SIGIL_STR    = 0x22; // '"' — string / var-length
const SIGIL_I64    = 0x69; // 'i'
const SIGIL_F64    = 0x64; // 'd'
const SIGIL_BOOL   = 0x62; // 'b'
const SIGIL_NULL   = 0x6E; // 'n'
const SIGIL_BINARY = 0x42; // 'B'

class Writer
{
    private Schema $schema;
    private string $buf            = '';
    private array  $frames         = [];
    private array  $recordOffsets  = [];
    private array  $slotSigils     = [];

    public function __construct(Schema $schema)
    {
        $this->schema     = $schema;
        $this->slotSigils = array_fill(0, $schema->length(), SIGIL_STR);
    }

    public function beginObject(): void
    {
        if (empty($this->frames)) {
            $this->recordOffsets[] = strlen($this->buf);
        }

        $start = strlen($this->buf);

        $bm = str_repeat("\x00", $this->schema->bitmaskBytes);
        for ($i = 0; $i < $this->schema->bitmaskBytes - 1; $i++) {
            $bm[$i] = "\x80";
        }

        $this->frames[] = new _Frame($start, $bm);

        $this->buf .= pack('VV', WRITER_MAGIC_OBJ, 0); // magic + length placeholder
        $this->buf .= $bm;                              // bitmask placeholder
        $this->buf .= str_repeat("\x00", $this->schema->length() * 2); // offset table

        while ((strlen($this->buf) - $start) % 8 !== 0) {
            $this->buf .= "\x00";
        }
    }

    public function endObject(): void
    {
        if (empty($this->frames)) {
            throw new \RuntimeException('endObject without beginObject');
        }
        $frame = array_pop($this->frames);

        $totalLen = strlen($this->buf) - $frame->start;

        // Back-patch Length at start + 4
        $lenBytes = pack('V', $totalLen);
        for ($i = 0; $i < 4; $i++) {
            $this->buf[$frame->start + 4 + $i] = $lenBytes[$i];
        }

        // Back-patch bitmask at start + 8
        $bmOff = $frame->start + 8;
        for ($i = 0; $i < $this->schema->bitmaskBytes; $i++) {
            $this->buf[$bmOff + $i] = $frame->bitmask[$i];
        }

        // Back-patch offset table
        $otStart = $bmOff + $this->schema->bitmaskBytes;
        $present = count($frame->offsetTable);

        if (!$frame->needsSort) {
            foreach ($frame->offsetTable as $idx => $rel) {
                $packed = pack('v', $rel);
                $this->buf[$otStart + $idx * 2]     = $packed[0];
                $this->buf[$otStart + $idx * 2 + 1] = $packed[1];
            }
        } else {
            $pairs = $frame->slotOffsets;
            usort($pairs, fn($a, $b) => $a[0] <=> $b[0]);
            foreach ($pairs as $idx => [$slot, $bufOff]) {
                $rel    = $bufOff - $frame->start;
                $packed = pack('v', $rel);
                $this->buf[$otStart + $idx * 2]     = $packed[0];
                $this->buf[$otStart + $idx * 2 + 1] = $packed[1];
            }
        }

        // Zero unused slots
        for ($i = $present * 2; $i < $this->schema->length() * 2; $i++) {
            $this->buf[$otStart + $i] = "\x00";
        }
    }

    public function finish(): string
    {
        if (!empty($this->frames)) {
            throw new \RuntimeException('unclosed objects');
        }

        $schemaBytes  = $this->_buildSchema();
        $dictHash     = _murmur3_64($schemaBytes);
        $dataStartAbs = 32 + strlen($schemaBytes);

        $dataSector = $this->buf;
        $tailPtr    = $dataStartAbs + strlen($dataSector);
        $tail       = $this->_buildTailIndex($dataStartAbs, $tailPtr);

        $out  = pack('V', WRITER_MAGIC_FILE);
        $out .= pack('vv', WRITER_VERSION, WRITER_FLAG_SCHEMA);
        $out .= $dictHash;
        $out .= pack('VV', 0, 0);
        $out .= str_repeat("\x00", 8); // reserved

        $out .= $schemaBytes;
        $out .= $dataSector;
        $out .= $tail;

        return $out;
    }

    // ── Typed write methods ──────────────────────────────────────────────────────

    public function writeI64(int $slot, int $v): void
    {
        $this->slotSigils[$slot] = SIGIL_I64;
        $this->_markSlot($slot);
        $this->buf .= pack('q', $v);
    }

    public function writeF64(int $slot, float $v): void
    {
        $this->slotSigils[$slot] = SIGIL_F64;
        $this->_markSlot($slot);
        $this->buf .= pack('e', $v);
    }

    public function writeBool(int $slot, bool $v): void
    {
        $this->slotSigils[$slot] = SIGIL_BOOL;
        $this->_markSlot($slot);
        $this->buf .= $v ? "\x01" : "\x00";
        $this->buf .= str_repeat("\x00", 7);
    }

    public function writeTime(int $slot, int $unixNs): void
    {
        $this->slotSigils[$slot] = SIGIL_I64;
        $this->writeI64($slot, $unixNs);
    }

    public function writeNull(int $slot): void
    {
        $this->slotSigils[$slot] = SIGIL_NULL;
        $this->_markSlot($slot);
        $this->buf .= str_repeat("\x00", 8);
    }

    public function writeStr(int $slot, string $v): void
    {
        $this->slotSigils[$slot] = SIGIL_STR;
        $this->_markSlot($slot);
        $b    = $v; // PHP strings are binary-safe
        $blen = strlen($b);
        $this->buf .= pack('V', $blen);
        $this->buf .= $b;
        $used = (4 + $blen) % 8;
        if ($used !== 0) {
            $this->buf .= str_repeat("\x00", 8 - $used);
        }
    }

    public function writeBytes(int $slot, string $data): void
    {
        $this->slotSigils[$slot] = SIGIL_BINARY;
        $this->_markSlot($slot);
        $dlen = strlen($data);
        $this->buf .= pack('V', $dlen);
        $this->buf .= $data;
        $used = (4 + $dlen) % 8;
        if ($used !== 0) {
            $this->buf .= str_repeat("\x00", 8 - $used);
        }
    }

    public function writeListI64(int $slot, array $values): void
    {
        $this->_markSlot($slot); // list is var-length — keep SIGIL_STR default
        $n     = count($values);
        $total = 16 + $n * 8;
        $this->buf .= pack('VVCVx3', WRITER_MAGIC_LIST, $total, 0x3D, $n);
        foreach ($values as $v) {
            $this->buf .= pack('q', $v);
        }
    }

    public function writeListF64(int $slot, array $values): void
    {
        $this->_markSlot($slot);
        $n     = count($values);
        $total = 16 + $n * 8;
        $this->buf .= pack('VVCVx3', WRITER_MAGIC_LIST, $total, 0x7E, $n);
        foreach ($values as $v) {
            $this->buf .= pack('e', $v);
        }
    }

    /** Convenience: write records from an array of associative arrays. */
    public static function fromRecords(array $keys, array $records): string
    {
        $schema = new Schema($keys);
        $w      = new self($schema);
        foreach ($records as $rec) {
            $w->beginObject();
            foreach ($keys as $i => $key) {
                if (!array_key_exists($key, $rec)) continue;
                $val = $rec[$key];
                if ($val === null)          { $w->writeNull($i); }
                elseif (is_bool($val))      { $w->writeBool($i, $val); }
                elseif (is_int($val))       { $w->writeI64($i, $val); }
                elseif (is_float($val))     { $w->writeF64($i, $val); }
                elseif (is_string($val))    { $w->writeStr($i, $val); }
            }
            $w->endObject();
        }
        return $w->finish();
    }

    // ── Private ──────────────────────────────────────────────────────────────────

    private function _markSlot(int $slot): void
    {
        if (empty($this->frames)) {
            throw new \RuntimeException('no active object');
        }
        $frame = &$this->frames[count($this->frames) - 1];

        $byteIdx = intdiv($slot, 7);
        $bitIdx  = $slot % 7;
        $frame->bitmask[$byteIdx] = chr(ord($frame->bitmask[$byteIdx]) | (1 << $bitIdx));

        $rel = strlen($this->buf) - $frame->start;
        if ($slot < $frame->lastSlot) {
            $frame->needsSort = true;
        }
        $frame->lastSlot = $slot;

        $frame->offsetTable[] = $rel;
        $frame->slotOffsets[] = [$slot, strlen($this->buf)];
    }

    private function _buildSchema(): string
    {
        $keys    = $this->schema->keys;
        $n       = count($keys);
        $encoded = array_map('strval', $keys);

        $size = 2 + $n;
        foreach ($encoded as $e) { $size += strlen($e) + 1; }
        $padded = $size + ((-$size) % 8 + 8) % 8;

        $buf = str_repeat("\x00", $padded);
        $p   = 0;
        $buf[$p] = chr($n & 0xFF);       $p++;
        $buf[$p] = chr(($n >> 8) & 0xFF); $p++;
        for ($i = 0; $i < $n; $i++) {
            $buf[$p++] = chr($this->slotSigils[$i]);
        }
        foreach ($encoded as $e) {
            $elen = strlen($e);
            for ($i = 0; $i < $elen; $i++) { $buf[$p++] = $e[$i]; }
            $buf[$p++] = "\x00";
        }
        return $buf;
    }

    private function _buildTailIndex(int $dataStart, int $tailPtr): string
    {
        $n   = count($this->recordOffsets);
        $buf = pack('V', $n);
        foreach ($this->recordOffsets as $i => $rel) {
            $abs  = $dataStart + $rel;
            $buf .= pack('v', $i);
            $buf .= pack('VV', $abs & 0xFFFFFFFF, ($abs >> 32) & 0xFFFFFFFF);
        }
        $buf .= pack('VV', $tailPtr & 0xFFFFFFFF, ($tailPtr >> 32) & 0xFFFFFFFF);
        $buf .= pack('V', WRITER_MAGIC_FOOTER);
        return $buf;
    }
}
