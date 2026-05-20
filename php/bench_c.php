<?php
/**
 * NXS PHP C-extension benchmark — three-way: C ext vs pure PHP vs json_decode.
 *
 * Mirrors the 6-scenario layout from py/bench_c.py and php/bench.php.
 * Requires the nxs.so extension to be loadable.
 *
 * Usage (from project root):
 *   php -d extension=php/nxs_ext/modules/nxs.so php/bench_c.php [fixtures_dir]
 *   # or if extension is installed system-wide:
 *   php php/bench_c.php [fixtures_dir]
 */

declare(strict_types=1);

ini_set('memory_limit', '2G');

/* ── Load C extension ────────────────────────────────────────────────────── */

if (!extension_loaded('nxs')) {
    $so = __DIR__ . '/nxs_ext/modules/nxs.so';
    if (!file_exists($so)) {
        fwrite(STDERR, "ERROR: nxs.so not found at $so\n");
        fwrite(STDERR, "Build it first: bash php/nxs_ext/build.sh\n");
        exit(1);
    }
    if (!function_exists('dl')) {
        fwrite(STDERR, "ERROR: dl() not available. Run: php -d extension=$so bench_c.php\n");
        exit(1);
    }
    dl($so);
}

/* ── Load pure PHP reader ─────────────────────────────────────────────────── */

require __DIR__ . '/Nxs.php';

/* ── Timing harness ──────────────────────────────────────────────────────── */

/**
 * Run $fn $iters times (after 2 warmup calls) and return average nanoseconds.
 */
function bench(int $iters, callable $fn): float
{
    // warmup
    $fn(); $fn();
    $t0 = hrtime(true);
    for ($i = 0; $i < $iters; $i++) $fn();
    return (hrtime(true) - $t0) / $iters;
}

/* ── Formatting ──────────────────────────────────────────────────────────── */

function fmtNs(float $ns): string
{
    if ($ns < 1_000)       return sprintf('%d ns',    (int)$ns);
    if ($ns < 1_000_000)   return sprintf('%.1f µs',  $ns / 1_000);
    if ($ns < 1_000_000_000) return sprintf('%.2f ms', $ns / 1_000_000);
    return sprintf('%.3f s', $ns / 1_000_000_000);
}

function fmtRatio(float $ns, float $baseline): string
{
    if (abs($ns - $baseline) < 1e-6) return 'baseline';
    if ($ns < $baseline) return sprintf('%.1fx faster', $baseline / $ns);
    return sprintf('%.1fx slower', $ns / $baseline);
}

function printHeader(string $title): void
{
    $width = 82;
    $dashes = $width - strlen($title) - 6;
    if ($dashes < 0) $dashes = 0;
    echo "\n  ┌─ $title " . str_repeat('─', $dashes) . "┐\n";
}

function printRow(string $label, float $ns, float $baseline): void
{
    $ratio = fmtRatio($ns, $baseline);
    printf("  │  %-46s %10s   %s\n", $label, fmtNs($ns), $ratio);
}

function printFooter(): void
{
    echo '  └' . str_repeat('─', 81) . "┘\n";
}

/* ── Main ────────────────────────────────────────────────────────────────── */

$dir   = $argv[1] ?? (__DIR__ . '/../js/fixtures');
$dir   = rtrim($dir, '/');
$sizes = [1_000, 10_000, 100_000, 1_000_000];

echo "\n";
echo "╔═══════════════════════════════════════════════════════════════════════╗\n";
echo "║       NXS PHP Benchmark: C extension vs pure PHP vs JSON             ║\n";
printf("║       PHP %-4s  |  NxsReader (C ext)  vs  Nxs\\Reader (PHP)         ║\n",
    PHP_MAJOR_VERSION . '.' . PHP_MINOR_VERSION);
echo "╚═══════════════════════════════════════════════════════════════════════╝\n";
echo "\nScenarios per size:\n";
echo "  1. Open file       — parse NXB (C ext / pure PHP) / JSON\n";
echo "  2. Warm access     — read one field (reader already open)\n";
echo "  3. Cold start      — open file + read one field\n";
echo "  4. Full scan       — iterate all records, sum score via record(i)->getF64()\n";
echo "  5. Reducer sumF64  — built-in sumF64('score') tight C loop\n";
echo "  6. Cold pipeline   — file_get_contents + sumF64('score')\n";

foreach ($sizes as $n) {
    $nxbPath  = "$dir/records_$n.nxb";
    $jsonPath = "$dir/records_$n.json";

    if (!file_exists($nxbPath)) {
        echo "\n  [skip] $nxbPath not found\n";
        continue;
    }

    $iters = match(true) {
        $n >= 1_000_000 => 3,
        $n >= 100_000   => 5,
        $n >= 10_000    => 15,
        default         => 50,
    };
    $warmIters = max(5, (int)($iters / 2));
    $k = (int)($n * 0.42);

    $nxbSize  = filesize($nxbPath);
    $jsonSize = file_exists($jsonPath) ? filesize($jsonPath) : 0;
    $nxbMB    = sprintf('%.1f MB', $nxbSize  / 1_048_576);
    $jsonMB   = sprintf('%.1f MB', $jsonSize / 1_048_576);

    echo "\n";
    echo str_repeat('═', 84) . "\n";
    printf("  N = %s  |  NXB %-8s  JSON %-8s  iters=%d\n",
        number_format($n), $nxbMB, $jsonMB, $iters);
    echo str_repeat('═', 84) . "\n";

    /* ── C extension ──────────────────────────────────────────────────── */
    $nxbBytes = file_get_contents($nxbPath);

    $cReader  = new NxsReader($nxbBytes);

    $cOpen = bench($iters, function() use ($nxbBytes) {
        new NxsReader($nxbBytes);
    });
    $cWarm = bench($warmIters, function() use ($cReader, $k) {
        $cReader->record($k)->getStr('username');
    });
    $cCold = bench($iters, function() use ($nxbBytes, $k) {
        $r = new NxsReader($nxbBytes);
        $r->record($k)->getStr('username');
    });
    $cScan = bench($iters, function() use ($cReader, $n) {
        $sum = 0.0;
        for ($i = 0; $i < $n; $i++) $sum += (float)$cReader->record($i)->getF64('score');
        return $sum;
    });
    $cReducer = bench($iters, function() use ($cReader) {
        $cReader->sumF64('score');
    });
    $cPipeline = bench($iters, function() use ($nxbPath) {
        $b = file_get_contents($nxbPath);
        $r = new NxsReader($b);
        $r->sumF64('score');
    });

    /* ── Pure PHP ─────────────────────────────────────────────────────── */
    $phpReader = new Nxs\Reader($nxbBytes);

    $phpOpen = bench($iters, function() use ($nxbBytes) {
        new Nxs\Reader($nxbBytes);
    });
    $phpWarm = bench($warmIters, function() use ($phpReader, $k) {
        $phpReader->record($k)->getStr('username');
    });
    $phpCold = bench($iters, function() use ($nxbBytes, $k) {
        $r = new Nxs\Reader($nxbBytes);
        $r->record($k)->getStr('username');
    });
    $phpScan = bench($iters, function() use ($phpReader, $n) {
        $sum = 0.0;
        for ($i = 0; $i < $n; $i++) $sum += (float)$phpReader->record($i)->getF64('score');
        return $sum;
    });
    $phpReducer = bench($iters, function() use ($phpReader) {
        $phpReader->sumF64('score');
    });
    $phpPipeline = bench($iters, function() use ($nxbPath) {
        $b = file_get_contents($nxbPath);
        $r = new Nxs\Reader($b);
        $r->sumF64('score');
    });

    unset($cReader, $phpReader, $nxbBytes);
    gc_collect_cycles();

    /* ── JSON ─────────────────────────────────────────────────────────── */
    $jsonOpen = $jsonWarm = $jsonCold = $jsonScan = $jsonReducer = $jsonPipeline = 0.0;
    if (file_exists($jsonPath)) {
        $jsonStr    = file_get_contents($jsonPath);
        $jsonParsed = json_decode($jsonStr, true);

        $jsonOpen = bench($iters, function() use ($jsonStr) {
            json_decode($jsonStr, true);
        });
        $jsonWarm = bench($warmIters, function() use ($jsonParsed, $k) {
            return $jsonParsed[$k]['username'];
        });
        $jsonCold = bench($iters, function() use ($jsonStr, $k) {
            $j = json_decode($jsonStr, true);
            return $j[$k]['username'];
        });
        $jsonScan = bench($iters, function() use ($jsonParsed, $n) {
            $sum = 0.0;
            for ($i = 0; $i < $n; $i++) $sum += (float)$jsonParsed[$i]['score'];
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
        unset($jsonParsed, $jsonStr);
        gc_collect_cycles();
    }

    /* ── Print results ────────────────────────────────────────────────── */
    printHeader("1. Open file (parse)");
    printRow("C   NxsReader::__construct ($nxbMB binary)", $cOpen,    $cOpen);
    printRow("PHP Nxs\\Reader::__construct ($nxbMB binary)", $phpOpen, $cOpen);
    if ($jsonOpen > 0) printRow("JSON json_decode ($jsonMB text)",   $jsonOpen, $cOpen);
    printFooter();

    printHeader("2. Warm random access  [record($k)->getStr('username')]");
    printRow("C   record(k)->getStr()",        $cWarm,    $cWarm);
    printRow("PHP record(k)->getStr()",        $phpWarm,  $cWarm);
    if ($jsonWarm > 0) printRow("JSON \$parsed[k]['username']", $jsonWarm, $cWarm);
    printFooter();

    printHeader("3. Cold start  [open + read 1 field]");
    printRow("C   open + record(k)->getStr()", $cCold,    $cCold);
    printRow("PHP open + record(k)->getStr()", $phpCold,  $cCold);
    if ($jsonCold > 0) printRow("JSON decode + \$j[k]['username']", $jsonCold, $cCold);
    printFooter();

    printHeader("4. Full scan  [sum score via record(i)->getF64()]");
    printRow("C   scan $n records",            $cScan,    $cScan);
    printRow("PHP scan $n records",            $phpScan,  $cScan);
    if ($jsonScan > 0) printRow("JSON iterate array",            $jsonScan, $cScan);
    printFooter();

    printHeader("5. Reducer  [sumF64('score')]");
    printRow("C   NxsReader::sumF64('score')",      $cReducer,    $cReducer);
    printRow("PHP Nxs\\Reader::sumF64('score')",     $phpReducer,  $cReducer);
    if ($jsonReducer > 0) printRow("JSON array_sum(array_column(...))", $jsonReducer, $cReducer);
    printFooter();

    printHeader("6. Cold pipeline  [file bytes + sumF64]");
    printRow("C   file_get + NxsReader + sumF64",    $cPipeline,    $cPipeline);
    printRow("PHP file_get + Nxs\\Reader + sumF64",   $phpPipeline,  $cPipeline);
    if ($jsonPipeline > 0) printRow("JSON file_get + json_decode + array_sum", $jsonPipeline, $cPipeline);
    printFooter();

    /* Summary line for 1M */
    if ($n >= 1_000_000 && $phpReducer > 0) {
        $speedup = $phpReducer / $cReducer;
        printf("\n  *** C sumF64 speedup over pure PHP: %.1fx ***\n", $speedup);
    }
}

echo "\n";
