<?php
/**
 * Adaptive prefetch — page cache, range coalescing, in-flight dedup (spec §6–§8.4).
 */

namespace Nxs;

const HINT_UNKNOWN    = 0;
const HINT_SEQUENTIAL = 1;
const HINT_RANDOM     = 2;
const HINT_FULL       = 3;
const HINT_PARTIAL    = 4;

const DEFAULT_PAGE_SIZE         = 65536;
const DEFAULT_MAX_PAGES         = 32;
const DEFAULT_COALESCE_GAP_PAGES = 1;

/**
 * @param int[] $indices page indices (any order)
 * @return array<int, array{pageStart: int, pageEnd: int, byteStart: int, byteLength: int}>
 */
function coalescePageIndices(array $indices, int $gapPages, int $pageSize = DEFAULT_PAGE_SIZE): array
{
    if ($indices === []) {
        return [];
    }
    $uniq = array_values(array_unique($indices));
    sort($uniq, SORT_NUMERIC);

    $spans = [];
    $start = $uniq[0];
    $end   = $uniq[0];
    for ($i = 1, $n = count($uniq); $i < $n; $i++) {
        if ($uniq[$i] - $end <= $gapPages) {
            $end = $uniq[$i];
        } else {
            $spans[] = [$start, $end];
            $start = $end = $uniq[$i];
        }
    }
    $spans[] = [$start, $end];

    $out = [];
    foreach ($spans as [$a, $b]) {
        $out[] = [
            'pageStart'  => $a,
            'pageEnd'    => $b,
            'byteStart'  => $a * $pageSize,
            'byteLength' => ($b - $a + 1) * $pageSize,
        ];
    }
    return $out;
}

/**
 * @param array<int, array{pageStart: int, pageEnd: int, byteStart: int, byteLength: int}> $ranges
 * @return array<int, array{pageStart: int, pageEnd: int, byteStart: int, byteLength: int}>
 */
function clampPageRanges(array $ranges, int $fileSize): array
{
    $out = [];
    foreach ($ranges as $r) {
        $len = $r['byteLength'];
        if ($r['byteStart'] + $len > $fileSize) {
            $len = $fileSize - $r['byteStart'];
        }
        if ($len <= 0) {
            continue;
        }
        $out[] = [...$r, 'byteLength' => $len];
    }
    return $out;
}

/**
 * @param callable(int): int $recordOffset
 * @return int[]
 */
function pageIndicesForViewport(int $startIndex, int $endIndex, int $pageSize, callable $recordOffset): array
{
    $out = [];
    for ($i = $startIndex; $i <= $endIndex; $i++) {
        $out[] = intdiv($recordOffset($i), $pageSize);
    }
    return $out;
}

class PageCache
{
    /** @var array<int, array{data: string, lastUsed: int, pinned: bool}> */
    private array $pages = [];
    private int $clock = 0;
    public int $hits = 0;
    public int $misses = 0;

    public function __construct(
        private int $maxPages,
        private int $pageSize,
    ) {}

    public function has(int $pageIndex): bool
    {
        return isset($this->pages[$pageIndex]);
    }

    public function get(int $pageIndex): ?string
    {
        if (!isset($this->pages[$pageIndex])) {
            $this->misses++;
            return null;
        }
        $this->pages[$pageIndex]['lastUsed'] = ++$this->clock;
        $this->hits++;
        return $this->pages[$pageIndex]['data'];
    }

    public function set(int $pageIndex, string $data, bool $pinned = false): void
    {
        if ($this->maxPages <= 0) {
            return;
        }
        while (count($this->pages) >= $this->maxPages) {
            if (!$this->evictOne()) {
                break;
            }
        }
        $this->pages[$pageIndex] = [
            'data'     => $data,
            'lastUsed' => ++$this->clock,
            'pinned'   => $pinned,
        ];
    }

    private function evictOne(): bool
    {
        $oldest = PHP_INT_MAX;
        $victim = -1;
        foreach ($this->pages as $idx => $e) {
            if ($e['pinned']) {
                continue;
            }
            if ($e['lastUsed'] < $oldest) {
                $oldest = $e['lastUsed'];
                $victim = $idx;
            }
        }
        if ($victim < 0) {
            return false;
        }
        unset($this->pages[$victim]);
        return true;
    }

    /** @param int[] $pageIndices */
    public function pinPages(array $pageIndices): void
    {
        foreach ($pageIndices as $p) {
            if (isset($this->pages[$p])) {
                $this->pages[$p]['pinned'] = true;
            }
        }
    }

    public function unpinAll(): void
    {
        foreach ($this->pages as &$e) {
            $e['pinned'] = false;
        }
        unset($e);
    }

    /** @return array{pages_cached: int, pages_max: int, memory_used_bytes: int, cache_hits: int, cache_misses: int} */
    public function stats(): array
    {
        $bytes = 0;
        foreach ($this->pages as $e) {
            $bytes += strlen($e['data']);
        }
        return [
            'pages_cached'     => count($this->pages),
            'pages_max'        => $this->maxPages,
            'memory_used_bytes' => $bytes,
            'cache_hits'       => $this->hits,
            'cache_misses'     => $this->misses,
        ];
    }
}

/**
 * Tracks in-flight page loads for deduplication (sync: marks pages being fetched).
 */
class InFlightMap
{
    /** @var array<int, true> */
    private array $active = [];

    public function has(int $pageIndex): bool
    {
        return isset($this->active[$pageIndex]);
    }

    /** @param int[] $pages */
    public function begin(array $pages): void
    {
        foreach ($pages as $p) {
            $this->active[$p] = true;
        }
    }

    /** @param int[] $pages */
    public function end(array $pages): void
    {
        foreach ($pages as $p) {
            unset($this->active[$p]);
        }
    }
}
