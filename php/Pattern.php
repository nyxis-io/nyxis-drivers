<?php
/**
 * Access pattern detector (Adaptive-prefetch-spec §4).
 */

namespace Nxs;

const SEQUENTIAL_THRESHOLD = 10;
const RANDOM_THRESHOLD = 100;
const HISTORY_SIZE = 32;
const MIN_OBSERVATIONS = 8;
const UPGRADE_SEQUENTIAL_THRESHOLD = 100;

const PATTERN_UNKNOWN = 'unknown';
const PATTERN_SEQUENTIAL = 'sequential';
const PATTERN_RANDOM = 'random';
const PATTERN_MIXED = 'mixed';

const DEFAULT_PREFETCH_DEPTH = 4;
const EAGER_THRESHOLD_MB = 10;
const LAZY_THRESHOLD_MB = 50;

/**
 * Observes record indices and classifies access patterns.
 */
class AccessPatternDetector
{
    /** @var int[] */
    private array $accesses;
    private int $writePos = 0;
    private int $filled = 0;
    public int $sequentialRuns = 0;
    private int $randomJumps = 0;
    public int $lastIndex = -1;

    public function __construct()
    {
        $this->accesses = array_fill(0, HISTORY_SIZE, -1);
    }

    public function observe(int $index): void
    {
        if ($this->lastIndex >= 0) {
            $delta = abs($index - $this->lastIndex);
            if ($delta <= SEQUENTIAL_THRESHOLD) {
                $this->sequentialRuns = min($this->sequentialRuns + 1, PHP_INT_MAX);
            } elseif ($delta > RANDOM_THRESHOLD) {
                $this->randomJumps = min($this->randomJumps + 1, PHP_INT_MAX);
            }
        }
        $this->accesses[$this->writePos] = $index;
        $this->writePos = ($this->writePos + 1) % HISTORY_SIZE;
        if ($this->filled < HISTORY_SIZE) {
            $this->filled++;
        }
        $this->lastIndex = $index;
    }

    public function pattern(): string
    {
        $total = $this->sequentialRuns + $this->randomJumps;
        if ($total < MIN_OBSERVATIONS) {
            return PATTERN_UNKNOWN;
        }
        if ($this->sequentialRuns > $this->randomJumps * 3) {
            return PATTERN_SEQUENTIAL;
        }
        if ($this->randomJumps > $this->sequentialRuns) {
            return PATTERN_RANDOM;
        }
        return PATTERN_MIXED;
    }

    /** @return int[] */
    public function predictNext(int $depth, int $recordCount): array
    {
        if ($this->pattern() !== PATTERN_SEQUENTIAL || $this->lastIndex < 0) {
            return [];
        }
        $start = $this->lastIndex + 1;
        $out = [];
        for ($i = 0; $i < $depth; $i++) {
            $idx = $start + $i;
            if ($idx < $recordCount) {
                $out[] = $idx;
            }
        }
        return $out;
    }
}

/**
 * Initial prefetch strategy from hint and file size (§5.1).
 */
function initialPrefetchStrategy(int $hint, int $fileSize): string
{
    $fileSizeMb = intdiv($fileSize, 1024 * 1024);
    if ($hint === HINT_FULL && $fileSizeMb <= EAGER_THRESHOLD_MB) {
        return 'eager';
    }
    if ($fileSizeMb > LAZY_THRESHOLD_MB) {
        return 'lazy';
    }
    return 'adaptive';
}

/** @return array{0: int, 1: int} row data sector [start, length] */
function rowDataSector(int $tailStart, int $fileSize): array
{
    $sectorStart = 32;
    if ($tailStart > $sectorStart && $tailStart <= $fileSize) {
        return [$sectorStart, $tailStart - $sectorStart];
    }
    return [$sectorStart, 0];
}
