<?php
/**
 * NXS PHP benchmark — compares NXS binary reader vs JSON decode vs CSV parse.
 *
 * Matches the 6-scenario layout used by the Go/Python/Ruby benches.
 * Uses hrtime(true) for nanosecond resolution (PHP 7.3+).
 *
 * Usage (from project root):
 *   php php/bench.php [fixtures_dir]
 *
 * Memory: the 1 M-record NXB file is ~132 MB; JSON is ~147 MB.
 * The script bumps memory_limit to 512 M automatically.
 */

declare(strict_types=1);

ini_set('memory_limit', '2G');

require __DIR__ . '/Nxs.php';

// ── Timing harness ────────────────────────────────────────────────────────────

/**
 * Run $fn $iters times (after 3 warmup calls) and return average nanoseconds.
 */
function bench(int $iters, callable $fn): float
{
    for ($i = 0; $i < 3; $i++) {
        $fn();
    }
    $t0 = hrtime(true);
    for ($i = 0; $i < $iters; $i++) {
        $fn();
    }
    return (hrtime(true) - $t0) / $iters;
}

// ── Formatting ────────────────────────────────────────────────────────────────

function fmtNs(float $ns): string
{
    if ($ns < 1_000) {
        return sprintf('%d ns', (int)$ns);
    }
    if ($ns < 1_000_000) {
        return sprintf('%.1f µs', $ns / 1_000);
    }
    if ($ns < 1_000_000_000) {
        return sprintf('%.2f ms', $ns / 1_000_000);
    }
    return sprintf('%.3f s', $ns / 1_000_000_000);
}

function fmtRatio(float $ns, float $baseline): string
{
    if (abs($ns - $baseline) < 1e-6) {
        return 'baseline';
    }
    if ($ns < $baseline) {
        return sprintf('%.1fx faster', $baseline / $ns);
    }
    return sprintf('%.1fx slower', $ns / $baseline);
}

function printHeader(string $title): void
{
    $width = 80;
    $dashes = $width - strlen($title) - 6;  // account for "  ┌─ " + " ┐\n"
    if ($dashes < 0) $dashes = 0;
    echo "\n  ┌─ $title " . str_repeat('─', $dashes) . "┐\n";
}

function printRow(string $label, float $ns, float $baseline): void
{
    $ratio = fmtRatio($ns, $baseline);
    printf("  │  %-44s %10s   %s\n", $label, fmtNs($ns), $ratio);
}

function printFooter(): void
{
    echo '  └' . str_repeat('─', 79) . "┘\n";
}

// ── CSV parser helper ─────────────────────────────────────────────────────────

/**
 * Parse CSV text into an array of associative records.
 * Uses str_getcsv (built-in, no extension needed).
 */
function parseCsv(string $text): array
{
    $lines   = explode("\n", rtrim($text));
    // Pass explicit $escape='' to silence PHP 8.4+ deprecation of the default.
    $headers = str_getcsv(array_shift($lines), ',', '"', '');
    $records = [];
    foreach ($lines as $line) {
        if ($line === '') continue;
        $cols = str_getcsv($line, ',', '"', '');
        $records[] = array_combine($headers, $cols);
    }
    return $records;
}

// ── Main ──────────────────────────────────────────────────────────────────────

$dir = $argv[1] ?? (__DIR__ . '/../js/fixtures');
$dir = rtrim($dir, '/');

$sizes = [1_000, 10_000, 100_000, 1_000_000];

echo "\n";
echo "╔══════════════════════════════════════════════════════════════════════╗\n";
echo "║          NXS PHP Reader Benchmark  (PHP " . sprintf('%-4s', PHP_MAJOR_VERSION . '.' . PHP_MINOR_VERSION) . ")                          ║\n";
echo "╚══════════════════════════════════════════════════════════════════════╝\n";
echo "\nScenarios per size:\n";
echo "  1. Open file     — parse NXB / JSON / CSV\n";
echo "  2. Warm access   — read one field (reader already open)\n";
echo "  3. Cold start    — open file + read one field\n";
echo "  4. Full scan     — iterate all records, sum score via record(i)->getF64()\n";
echo "  5. Reducer       — sumF64('score') built-in\n";
echo "  6. Cold pipeline — read file bytes + sumF64('score')\n";

foreach ($sizes as $n) {
    $nxbPath  = "$dir/records_$n.nxb";
    $jsonPath = "$dir/records_$n.json";
    $csvPath  = "$dir/records_$n.csv";

    if (!file_exists($nxbPath)) {
        echo "\n  [skip] $nxbPath not found\n";
        continue;
    }

    // Number of iterations — scale down for large sizes
    $iters = match(true) {
        $n >= 1_000_000 => 3,
        $n >= 100_000   => 5,
        $n >= 10_000    => 20,
        default         => 100,
    };
    $warmIters = max(5, (int)($iters / 2));

    // Pick a stable random index
    $k = (int)($n * 0.42);

    $nxbSize  = filesize($nxbPath);
    $jsonSize = filesize($jsonPath);
    $csvSize  = filesize($csvPath);

    $nxbMB  = sprintf('%.1f MB', $nxbSize  / 1_048_576);
    $jsonMB = sprintf('%.1f MB', $jsonSize / 1_048_576);
    $csvMB  = sprintf('%.1f MB', $csvSize  / 1_048_576);

    echo "\n";
    echo str_repeat('═', 82) . "\n";
    printf("  N = %s  |  NXB %-8s  JSON %-8s  CSV %-8s  iters=%d\n",
        number_format($n), $nxbMB, $jsonMB, $csvMB, $iters);
    echo str_repeat('═', 82) . "\n";

    // For the 1M case we cannot hold all three parsed representations in memory
    // simultaneously (~900 MB each for JSON/CSV arrays + 132 MB NXB = OOM).
    // Scenarios that need "warm" parsed data measure each format independently,
    // freeing the previous one before loading the next.

    // ── Pre-load NXB ──────────────────────────────────────────────────────
    $nxbBytes  = file_get_contents($nxbPath);
    $nxsReader = new Nxs\Reader($nxbBytes);

    // ── Scenario 1a: Open NXS ─────────────────────────────────────────────
    $nxbOpen = bench($iters, function() use ($nxbBytes) {
        new Nxs\Reader($nxbBytes);
    });

    // ── Scenario 2a: Warm NXS access ─────────────────────────────────────
    $nxbWarm = bench($warmIters, function() use ($nxsReader, $k) {
        $nxsReader->record($k)->getStr('username');
    });

    // ── Scenario 3a: Cold NXS start ──────────────────────────────────────
    $nxbCold = bench($iters, function() use ($nxbBytes, $k) {
        $r = new Nxs\Reader($nxbBytes);
        $r->record($k)->getStr('username');
    });

    // ── Scenario 4a: NXS full scan ───────────────────────────────────────
    $nxbScan = bench($iters, function() use ($nxsReader, $n) {
        $sum = 0.0;
        for ($i = 0; $i < $n; $i++) {
            $sum += (float)$nxsReader->record($i)->getF64('score');
        }
        return $sum;
    });

    // ── Scenario 5a: NXS reducer ─────────────────────────────────────────
    $nxbReducer = bench($iters, function() use ($nxsReader) {
        $nxsReader->sumF64('score');
    });

    // ── Scenario 6a: NXS cold pipeline ───────────────────────────────────
    $nxbPipeline = bench($iters, function() use ($nxbPath) {
        $b = file_get_contents($nxbPath);
        $r = new Nxs\Reader($b);
        $r->sumF64('score');
    });

    // Free NXB before loading JSON (saves ~132 MB for 1M)
    unset($nxsReader, $nxbBytes);
    gc_collect_cycles();

    // ── Pre-load JSON ─────────────────────────────────────────────────────
    $jsonStr    = file_get_contents($jsonPath);
    $jsonParsed = json_decode($jsonStr, true);

    $jsonOpen = bench($iters, function() use ($jsonStr) {
        json_decode($jsonStr, true);
    });
    $jsonWarm = bench($warmIters, function() use ($jsonParsed, $k) {
        $jsonParsed[$k]['username'];
    });
    $jsonCold = bench($iters, function() use ($jsonStr, $k) {
        $j = json_decode($jsonStr, true);
        $j[$k]['username'];
    });
    $jsonScan = bench($iters, function() use ($jsonParsed, $n) {
        $sum = 0.0;
        for ($i = 0; $i < $n; $i++) {
            $sum += (float)$jsonParsed[$i]['score'];
        }
        return $sum;
    });
    $jsonReducer = bench($iters, function() use ($jsonParsed) {
        array_sum(array_column($jsonParsed, 'score'));
    });
    $jsonPipeline = bench($iters, function() use ($jsonPath) {
        $s = file_get_contents($jsonPath);
        $j = json_decode($s, true);
        array_sum(array_column($j, 'score'));
    });

    // Free JSON before loading CSV
    unset($jsonParsed, $jsonStr);
    gc_collect_cycles();

    // ── Pre-load CSV ──────────────────────────────────────────────────────
    $csvStr    = file_get_contents($csvPath);
    $csvParsed = parseCsv($csvStr);

    $csvOpen = bench($iters, function() use ($csvStr) {
        parseCsv($csvStr);
    });
    $csvWarm = bench($warmIters, function() use ($csvParsed, $k) {
        $csvParsed[$k]['username'];
    });
    $csvCold = bench($iters, function() use ($csvStr, $k) {
        $c = parseCsv($csvStr);
        $c[$k]['username'];
    });
    $csvScan = bench($iters, function() use ($csvParsed, $n) {
        $sum = 0.0;
        for ($i = 0; $i < $n; $i++) {
            $sum += (float)$csvParsed[$i]['score'];
        }
        return $sum;
    });
    $csvReducer = bench($iters, function() use ($csvParsed) {
        array_sum(array_column($csvParsed, 'score'));
    });
    $csvPipeline = bench($iters, function() use ($csvPath) {
        $s = file_get_contents($csvPath);
        $c = parseCsv($s);
        array_sum(array_column($c, 'score'));
    });

    unset($csvParsed, $csvStr);
    gc_collect_cycles();

    // ── Print all 6 scenarios ─────────────────────────────────────────────
    printHeader("1. Open file (parse)");
    printRow("NXS  open  ($nxbMB binary)", $nxbOpen, $nxbOpen);
    printRow("JSON parse ($jsonMB text)", $jsonOpen, $nxbOpen);
    printRow("CSV  parse ($csvMB text)", $csvOpen, $nxbOpen);
    printFooter();

    printHeader("2. Warm random access  [record($k)->getStr(\"username\")]");
    printRow('NXS  record(k)->getStr("username")', $nxbWarm, $nxbWarm);
    printRow('JSON $parsed[k]["username"]', $jsonWarm, $nxbWarm);
    printRow('CSV  $parsed[k]["username"]', $csvWarm, $nxbWarm);
    printFooter();

    printHeader("3. Cold start  [open + read 1 field]");
    printRow('NXS  open + record(k)->getStr', $nxbCold, $nxbCold);
    printRow('JSON decode + $j[k]["username"]', $jsonCold, $nxbCold);
    printRow('CSV  parse  + $c[k]["username"]', $csvCold, $nxbCold);
    printFooter();

    printHeader("4. Full scan  [sum score via record(i)->getF64()]");
    printRow("NXS  scan $n records, sum score", $nxbScan, $nxbScan);
    printRow('JSON iterate array, sum score', $jsonScan, $nxbScan);
    printRow('CSV  iterate array, sum score', $csvScan, $nxbScan);
    printFooter();

    printHeader("5. Reducer  [sumF64(\"score\")]");
    printRow('NXS  sumF64("score")', $nxbReducer, $nxbReducer);
    printRow('JSON array_sum(array_column(...))', $jsonReducer, $nxbReducer);
    printRow('CSV  array_sum(array_column(...))', $csvReducer, $nxbReducer);
    printFooter();

    printHeader("6. Cold pipeline  [read file bytes + sumF64]");
    printRow('NXS  file_get + new Reader + sumF64', $nxbPipeline, $nxbPipeline);
    printRow('JSON file_get + json_decode + array_sum', $jsonPipeline, $nxbPipeline);
    printRow('CSV  file_get + parseCsv + array_sum', $csvPipeline, $nxbPipeline);
    printFooter();
}

echo "\n";
