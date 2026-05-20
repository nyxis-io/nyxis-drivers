<?php
/**
 * NXS PHP parity tests — validates the PHP reader against the 1000-record JSON fixture.
 *
 * Usage (from project root):
 *   php php/test.php js/fixtures
 */

declare(strict_types=1);

require __DIR__ . '/Nxs.php';

// ── Helpers ──────────────────────────────────────────────────────────────────

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

// ── Fixtures ─────────────────────────────────────────────────────────────────

$dir = $argv[1] ?? (__DIR__ . '/../js/fixtures');
$dir = rtrim($dir, '/');

$nxbPath  = "$dir/records_1000.nxb";
$jsonPath = "$dir/records_1000.json";

if (!file_exists($nxbPath)) {
    fwrite(STDERR, "ERROR: cannot find $nxbPath\n");
    exit(1);
}
if (!file_exists($jsonPath)) {
    fwrite(STDERR, "ERROR: cannot find $jsonPath\n");
    exit(1);
}

$nxbBytes = file_get_contents($nxbPath);
$json     = json_decode(file_get_contents($jsonPath), true, 512, JSON_THROW_ON_ERROR);

echo "\nNXS PHP Reader — parity tests against records_1000\n";
echo str_repeat('─', 56) . "\n";

// ── Reader construction ───────────────────────────────────────────────────────

try {
    $reader = new Nxs\Reader($nxbBytes);
    check('Reader construction succeeds', true);
} catch (\Throwable $e) {
    check('Reader construction succeeds', false, $e->getMessage());
    exit(1);
}

// ── Test 1: recordCount ────────────────────────────────────────────────────────

check('recordCount() === 1000', $reader->recordCount() === 1000,
    'got ' . $reader->recordCount());

// ── Test 2: keys() contains "username" ────────────────────────────────────────

check('keys() contains "username"', in_array('username', $reader->keys(), true));

// ── Test 3: record(42)->getStr("username") matches JSON ───────────────────────

$got42 = $reader->record(42)->getStr('username');
$exp42 = (string)$json[42]['username'];
check(
    'record(42)->getStr("username") matches JSON',
    $got42 === $exp42,
    "got=$got42, expected=$exp42"
);

// ── Test 4: record(500)->getF64("score") matches JSON (6 dp) ──────────────────

$gotScore = $reader->record(500)->getF64('score');
$expScore = (float)$json[500]['score'];
check(
    'record(500)->getF64("score") ≈ json[500].score',
    round((float)$gotScore, 6) === round($expScore, 6),
    "got=$gotScore, expected=$expScore"
);

// ── Test 5: record(999)->getBool("active") matches JSON ───────────────────────

$gotBool = $reader->record(999)->getBool('active');
$expBool = (bool)$json[999]['active'];
check(
    'record(999)->getBool("active") matches JSON',
    $gotBool === $expBool,
    'got=' . var_export($gotBool, true) . ', expected=' . var_export($expBool, true)
);

// ── Test 6: sumF64("score") matches JSON sum (4 dp) ───────────────────────────

$gotSum = $reader->sumF64('score');
$expSum = (float)array_sum(array_column($json, 'score'));
check(
    'sumF64("score") ≈ array_sum(json[*].score) [4 dp]',
    round($gotSum, 4) === round($expSum, 4),
    "got=$gotSum, expected=$expSum"
);

// ── Test 7: Out-of-bounds throws NxsException ─────────────────────────────────

try {
    $reader->record(1000);
    check('record(1000) throws NxsException', false, 'no exception thrown');
} catch (Nxs\NxsException $e) {
    check('record(1000) throws NxsException', true);
} catch (\Throwable $e) {
    check('record(1000) throws NxsException', false, get_class($e) . ': ' . $e->getMessage());
}

// ── Test 8: record(0)->getI64("id") matches JSON ─────────────────────────────

$gotId = $reader->record(0)->getI64('id');
$expId = (int)$json[0]['id'];
check(
    'record(0)->getI64("id") matches JSON',
    $gotId === $expId,
    "got=$gotId, expected=$expId"
);

// ── Test 9: record(1)->getBool("active") === true ─────────────────────────────

$gotActive1 = $reader->record(1)->getBool('active');
$expActive1 = (bool)$json[1]['active'];
check(
    'record(1)->getBool("active") matches JSON (true)',
    $gotActive1 === $expActive1,
    'got=' . var_export($gotActive1, true) . ', expected=' . var_export($expActive1, true)
);

// ── Test 10: record(1)->getF64("balance") ≈ json[1].balance ──────────────────

$gotBal1 = $reader->record(1)->getF64('balance');
$expBal1 = (float)$json[1]['balance'];
check(
    'record(1)->getF64("balance") ≈ json[1].balance [6 dp]',
    round((float)$gotBal1, 6) === round($expBal1, 6),
    "got=$gotBal1, expected=$expBal1"
);

// ── Writer round-trip tests ───────────────────────────────────────────────────

require __DIR__ . '/NxsWriter.php';

echo "\nNXS PHP Writer — Round-trip Tests\n";
echo str_repeat('─', 56) . "\n";

// writer round-trip: 3 records
$schema = new Nxs\Schema(['id', 'username', 'score', 'active']);
$w = new Nxs\Writer($schema);
foreach ([[1, 'alice', 9.5, true], [2, 'bob', 7.2, false], [3, 'carol', 8.8, true]] as [$id, $name, $score, $active]) {
    $w->beginObject();
    $w->writeI64(0, $id); $w->writeStr(1, $name); $w->writeF64(2, $score); $w->writeBool(3, $active);
    $w->endObject();
}
$rt = new Nxs\Reader($w->finish());
check('writer round-trip: record count', $rt->recordCount() === 3);
check('writer round-trip: record(0) id', $rt->record(0)->getI64('id') === 1);
check('writer round-trip: record(1) username', $rt->record(1)->getStr('username') === 'bob');
check('writer round-trip: record(2) score', abs((float)$rt->record(2)->getF64('score') - 8.8) < 1e-9);
check('writer round-trip: record(0) active', $rt->record(0)->getBool('active') === true);
check('writer round-trip: record(1) active', $rt->record(1)->getBool('active') === false);

// fromRecords convenience
$data2 = Nxs\Writer::fromRecords(['id', 'name', 'value'],
    [['id' => 10, 'name' => 'foo', 'value' => 1.5], ['id' => 20, 'name' => 'bar', 'value' => 2.5]]);
$rt2 = new Nxs\Reader($data2);
check('writer fromRecords: record count', $rt2->recordCount() === 2);
check('writer fromRecords: record(1) name', $rt2->record(1)->getStr('name') === 'bar');

// null field
$wn = new Nxs\Writer(new Nxs\Schema(['a', 'b']));
$wn->beginObject(); $wn->writeI64(0, 99); $wn->writeNull(1); $wn->endObject();
$rtn = new Nxs\Reader($wn->finish());
check('writer null field: a == 99', $rtn->record(0)->getI64('a') === 99);

// bool fields
$wb = new Nxs\Writer(new Nxs\Schema(['flag']));
$wb->beginObject(); $wb->writeBool(0, true);  $wb->endObject();
$wb->beginObject(); $wb->writeBool(0, false); $wb->endObject();
$rtb = new Nxs\Reader($wb->finish());
check('writer bool: record(0) true',  $rtb->record(0)->getBool('flag') === true);
check('writer bool: record(1) false', $rtb->record(1)->getBool('flag') === false);

// unicode string
$wu = new Nxs\Writer(new Nxs\Schema(['msg']));
$wu->beginObject(); $wu->writeStr(0, 'héllo wörld'); $wu->endObject();
$rtu = new Nxs\Reader($wu->finish());
check('writer unicode string', $rtu->record(0)->getStr('msg') === 'héllo wörld');

// many fields (>7, multi-byte bitmask)
$keys = array_map(fn($i) => "f$i", range(0, 8));
$wm = new Nxs\Writer(new Nxs\Schema($keys));
$wm->beginObject();
foreach ($keys as $i => $_) { $wm->writeI64($i, $i * 100); }
$wm->endObject();
$rtm = new Nxs\Reader($wm->finish());
$manyOk = true;
foreach ($keys as $i => $k) { if ($rtm->record(0)->getI64($k) !== $i * 100) { $manyOk = false; break; } }
check('writer many fields (multi-byte bitmask)', $manyOk);

// ── Security tests ───────────────────────────────────────────────────────────

$badMagic = $nxbBytes;
$badMagic[0] = "\x00";
try {
    new Nxs\Reader($badMagic);
    check('bad magic throws ERR_BAD_MAGIC', false, 'no exception');
} catch (Nxs\NxsException $e) {
    check('bad magic throws ERR_BAD_MAGIC', str_contains($e->getMessage(), 'ERR_BAD_MAGIC'), $e->getMessage());
}

try {
    new Nxs\Reader(substr($nxbBytes, 0, 16));
    check('truncated file throws NxsException', false, 'no exception');
} catch (Nxs\NxsException $e) {
    check('truncated file throws NxsException', true);
}

$badHash = $nxbBytes;
$badHash[8] = chr(ord($badHash[8]) ^ 0xFF);
try {
    new Nxs\Reader($badHash);
    check('corrupt DictHash throws ERR_DICT_MISMATCH', false, 'no exception');
} catch (Nxs\NxsException $e) {
    check('corrupt DictHash throws ERR_DICT_MISMATCH', str_contains($e->getMessage(), 'ERR_DICT_MISMATCH'), $e->getMessage());
}

// ── Query engine tests ────────────────────────────────────────────────────────

echo "\nNXS PHP Query Engine — Tests\n";
echo str_repeat('─', 56) . "\n";

// Q1: filter active == true, count matches JSON truth count
$activeCount = $reader->where(Nxs\nxs_eq('active', true))->count();
$expActiveCount = count(array_filter($json, fn($r) => $r['active'] === true));
check(
    'where(active==true)->count() matches JSON',
    $activeCount === $expActiveCount,
    "got=$activeCount, expected=$expActiveCount"
);

// Q2: filter score > 5.0, count matches JSON
$gt5Count = $reader->where(Nxs\nxs_gt('score', 5.0))->count();
$expGt5 = count(array_filter($json, fn($r) => (float)$r['score'] > 5.0));
check(
    'where(score>5.0)->count() matches JSON',
    $gt5Count === $expGt5,
    "got=$gt5Count, expected=$expGt5"
);

// Q3: nxs_and(active==true, score>5.0) count matches JSON
$andCount = $reader->where(Nxs\nxs_and(Nxs\nxs_eq('active', true), Nxs\nxs_gt('score', 5.0)))->count();
$expAnd = count(array_filter($json, fn($r) => $r['active'] === true && (float)$r['score'] > 5.0));
check(
    'where(and(active==true, score>5.0))->count() matches JSON',
    $andCount === $expAnd,
    "got=$andCount, expected=$expAnd"
);

// Q4: first() where active==true returns correct username
$firstActive = $reader->where(Nxs\nxs_eq('active', true))->first();
$expFirstUsername = null;
foreach ($json as $r) { if ($r['active'] === true) { $expFirstUsername = $r['username']; break; } }
$gotFirstUsername = $firstActive !== null ? $firstActive->getStr('username') : null;
check(
    'where(active==true)->first()->getStr("username") matches JSON',
    $gotFirstUsername === $expFirstUsername,
    "got=" . ($gotFirstUsername ?? 'null') . ", expected=$expFirstUsername"
);

// Q5: all()->count() == total records
$allCount = $reader->all()->count();
check(
    'all()->count() === recordCount()',
    $allCount === $reader->recordCount(),
    "got=$allCount, expected=" . $reader->recordCount()
);

// ── Summary ───────────────────────────────────────────────────────────────────

echo str_repeat('─', 56) . "\n";
$total = $pass + $fail;
if ($fail === 0) {
    echo "  All $pass/$total tests passed.\n\n";
    exit(0);
} else {
    echo "  $pass/$total passed, $fail FAILED.\n\n";
    exit(1);
}
