<?php
/**
 * Adaptive prefetch tests — page cache, coalescing, viewport (phase 1).
 *
 * Usage:
 *   php test_prefetch.php
 */

declare(strict_types=1);

require __DIR__ . '/Nxs.php';
require __DIR__ . '/NxsWriter.php';

$pass = 0;
$fail = 0;

function check(string $label, bool $ok, string $detail = ''): void
{
    global $pass, $fail;
    if ($ok) {
        echo "  \u{2713} $label\n";
        $pass++;
    } else {
        echo "  \u{2717} $label" . ($detail ? " — $detail" : '') . "\n";
        $fail++;
    }
}

function buildCompactRecords(int $n): string
{
    $schema = new Nxs\Schema(['id', 'tag']);
    $w = new Nxs\Writer($schema);
    for ($i = 0; $i < $n; $i++) {
        $w->beginObject();
        $w->writeI64(0, $i);
        $w->writeStr(1, "r$i");
        $w->endObject();
    }
    return $w->finish();
}

function buildRecords(int $n): string
{
    $schema = new Nxs\Schema(['id', 'username', 'score', 'active']);
    $w = new Nxs\Writer($schema);
    for ($i = 0; $i < $n; $i++) {
        $w->beginObject();
        $w->writeI64(0, $i);
        $w->writeStr(1, "user_$i");
        $w->writeF64(2, $i * 0.25);
        $w->writeBool(3, $i % 2 === 0);
        $w->endObject();
    }
    return $w->finish();
}

echo "\nNXS PHP Prefetch — Tests\n";
echo str_repeat('─', 56) . "\n";

// coalescePageIndices
$r = Nxs\coalescePageIndices([3, 4, 6, 7, 12], 1, Nxs\DEFAULT_PAGE_SIZE);
check('coalescePageIndices [3,4,6,7,12] gap=1 → 3 ranges', count($r) === 3
    && $r[0]['pageStart'] === 3 && $r[0]['pageEnd'] === 4
    && $r[1]['pageStart'] === 6 && $r[1]['pageEnd'] === 7
    && $r[2]['pageStart'] === 12 && $r[2]['pageEnd'] === 12);

// PageCache LRU
$c = new Nxs\PageCache(2, 64);
$c->set(0, str_repeat("\x00", 64));
$c->set(1, str_repeat("\x00", 64));
$c->get(0);
$c->set(2, str_repeat("\x00", 64));
check('PageCache LRU evicts at max_pages', !$c->has(1) && $c->has(0) && $c->has(2));

// prefetch_viewport coalescing
$buf = buildRecords(60);
$ranges = [];
$reader = new Nxs\Reader($buf, [
    'max_pages'          => 64,
    'coalesce_gap_pages' => 1,
    'fetch_range'        => static function (int $start, int $len) use ($buf, &$ranges): string {
        $ranges[] = ['start' => $start, 'len' => $len];
        return substr($buf, $start, $len);
    },
]);
$reader->prefetch_viewport(0, 49);
check(
    'prefetch_viewport uses ≤3 coalesced fetch_range calls for 50 records',
    count($ranges) <= 3,
    'got ' . count($ranges) . ': ' . json_encode($ranges)
);
$stats = $reader->cache_stats();
check(
    'fetches_issued matches recorder count',
    ($stats['fetches_issued'] ?? -1) === count($ranges),
    'fetches_issued=' . ($stats['fetches_issued'] ?? 'null')
);

// prefetch_viewport_basic
$buf55 = buildRecords(55);
$reader2 = new Nxs\Reader($buf55, [
    'fetch_range' => static fn(int $s, int $l): string => substr($buf55, $s, $l),
]);
$reader2->prefetch_viewport(0, 49);
check(
    'prefetch_viewport_basic — record(49) readable after prefetch',
    $reader2->record(49)->getI64('id') === 49
);

// prefetch_memory_eviction
$buf20 = buildRecords(20);
$reader3 = new Nxs\Reader($buf20, [
    'max_pages'          => 2,
    'page_size'          => 256,
    'coalesce_gap_pages' => 0,
]);
$reader3->prefetch_viewport(0, 0);
$reader3->prefetch_viewport(19, 19);
$stats3 = $reader3->cache_stats();
check(
    'prefetch_memory_eviction — cache stays within max_pages',
    ($stats3['pages_cached'] ?? 99) <= 2,
    'pages_cached=' . ($stats3['pages_cached'] ?? 'null')
);

// prefetch_deduplication — sequential double-call should not re-fetch cached pages
$buf10 = buildRecords(10);
$calls = 0;
$reader4 = new Nxs\Reader($buf10, [
    'max_pages'   => 8,
    'fetch_range' => static function (int $s, int $l) use ($buf10, &$calls): string {
        $calls++;
        return substr($buf10, $s, $l);
    },
]);
$reader4->prefetch_viewport(0, 4);
$before = $calls;
$reader4->prefetch_viewport(0, 4);
check(
    'prefetch_deduplication — second viewport skips cached pages',
    $calls === $before,
    "calls before=$before after=$calls"
);

// default max_pages = 32
$reader5 = new Nxs\Reader(buildRecords(5));
$stats5 = $reader5->cache_stats();
check('default max_pages is 32', ($stats5['pages_max'] ?? 0) === 32);

// out-of-bounds
$threw = false;
try {
    $reader->prefetch_viewport(0, 9999);
} catch (\Throwable) {
    $threw = true;
}
check('prefetch_viewport out-of-bounds throws', $threw);

// cache_stats shape
$shapeOk = isset($stats['pages_cached'], $stats['pages_max'], $stats['memory_used_bytes'],
    $stats['cache_hits'], $stats['cache_misses'], $stats['fetches_issued'],
    $stats['strategy'], $stats['pattern']);
check('cache_stats returns expected keys', $shapeOk);

// pattern detector
$d = new Nxs\AccessPatternDetector();
for ($i = 0; $i < 8; $i++) {
    $d->observe($i);
}
check('pattern unknown until min observations', $d->pattern() === Nxs\PATTERN_UNKNOWN);
$d->observe(8);
check('pattern classified after 9th access', $d->pattern() !== Nxs\PATTERN_UNKNOWN);

$d2 = new Nxs\AccessPatternDetector();
for ($i = 0; $i < 20; $i++) {
    $d2->observe($i);
}
check('pattern sequential small deltas', $d2->pattern() === Nxs\PATTERN_SEQUENTIAL);

$d3 = new Nxs\AccessPatternDetector();
for ($i = 0; $i < 8; $i++) {
    $d3->observe($i);
}
for ($k = 0; $k < 12; $k++) {
    $d3->observe($k * 200);
}
check('pattern random large jumps', $d3->pattern() === Nxs\PATTERN_RANDOM);

$d4 = new Nxs\AccessPatternDetector();
for ($i = 0; $i < 10; $i++) {
    $d4->observe($i);
}
check(
    'predict_next sequential',
    $d4->predictNext(4, 100) === [10, 11, 12, 13]
);

// pause stops speculative prefetch
$bufPause = buildCompactRecords(200);
$readerPause = new Nxs\Reader($bufPause);
for ($i = 0; $i < 25; $i++) {
    $readerPause->record($i);
}
$statsPause = $readerPause->cache_stats();
check(
    'pause: pattern sequential before pause',
    ($statsPause['pattern'] ?? '') === 'sequential'
);
$beforePause = $statsPause['fetches_issued'] ?? 0;
$readerPause->pausePrefetch();
$readerPause->record(26);
$afterPause = $readerPause->cache_stats()['fetches_issued'] ?? 0;
$readerPause->resumePrefetch();
$readerPause->record(27);
$afterResume = $readerPause->cache_stats()['fetches_issued'] ?? 0;
check(
    'pause stops speculative prefetch',
    $afterPause === $beforePause && $afterResume >= $beforePause,
    "before=$beforePause after_pause=$afterPause after_resume=$afterResume"
);

// hint full → eager at open (sync load via warmup)
$buf200 = buildRecords(200);
$readerFull = new Nxs\Reader($buf200, ['hint' => Nxs\HINT_FULL]);
$readerFull->warmup();
check(
    'hint full small file eager at open',
    ($readerFull->cache_stats()['strategy'] ?? '') === 'eager'
);

// sequential upgrade after 150 record() calls
$readerUp = new Nxs\Reader($buf200);
for ($i = 0; $i < 150; $i++) {
    $readerUp->record($i);
}
$readerUp->warmup();
$statsUp = $readerUp->cache_stats();
check(
    'sequential upgrade to eager after 150 accesses',
    ($statsUp['strategy'] ?? '') === 'eager' && ($statsUp['pattern'] ?? '') === 'sequential',
    json_encode($statsUp)
);

echo str_repeat('─', 56) . "\n";
$total = $pass + $fail;
if ($fail === 0) {
    echo "  All $pass/$total tests passed.\n\n";
    exit(0);
}
echo "  $pass/$total passed, $fail FAILED.\n\n";
exit(1);
