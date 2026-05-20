# NXS — PHP

Zero-copy `.nxb` reader for PHP 8.0+. Pure-PHP implementation with an optional C extension for hot-path columnar scans. No Composer, no dependencies.

## Requirements

PHP 8.0+. The C extension requires a C compiler and PHP development headers (`php-dev` / `php-devel`).

## Read a file

```php
require_once __DIR__ . '/Nxs.php';

$bytes  = file_get_contents('data.nxb');
$reader = new Nxs\Reader($bytes);

echo $reader->recordCount() . "\n";    // instant — read from tail-index, no parse pass
$obj = $reader->record(42);            // O(1) seek
echo $obj->getStr("username") . "\n";
echo $obj->getF64("score") . "\n";
echo ($obj->getBool("active") ? "true" : "false") . "\n";
```

## Columnar reducers

```php
$total = $reader->sumF64("score");
$low   = $reader->minF64("score");
$high  = $reader->maxF64("score");
$ages  = $reader->sumI64("age");
```

## C extension (hot path)

Build once:

```bash
bash nxs_ext/build.sh
```

```php
dl(__DIR__ . '/nxs_ext/modules/nxs.so');   // or add extension= to php.ini

$reader = new NxsReader($bytes);
echo $reader->record(42)->getStr("username") . "\n";
echo $reader->sumF64("score") . "\n";      // 2.00 ms at 1M records
```

At 1M records the C extension is **143× faster** than pure PHP for `sumF64`, and **15× faster** than `json_decode`.

## Write a file

```php
require_once __DIR__ . '/NxsWriter.php';

$schema = new Nxs\Schema(['id', 'username', 'score', 'active']);
$w = new Nxs\Writer($schema);

$w->beginObject();
$w->writeI64(0, 42);
$w->writeStr(1, 'alice');
$w->writeF64(2, 9.5);
$w->writeBool(3, true);
$w->endObject();

$data = $w->finish();   // binary string

// Convenience: write from an array of associative arrays
$data2 = Nxs\Writer::fromRecords(
    ['id', 'username', 'score'],
    [['id' => 1, 'username' => 'bob', 'score' => 8.2]]
);
```

## Tests

```bash
php test.php ../js/fixtures    # 11 tests
```

## Benchmarks

```bash
php bench.php ../js/fixtures                                         # pure PHP vs json_decode
php -d extension=nxs_ext/modules/nxs.so bench_c.php ../js/fixtures  # C extension vs json_decode
```

## Files

| File | Purpose |
| :--- | :--- |
| `Nxs.php` | Pure-PHP reader (`Nxs\Reader`, `Nxs\Object`) |
| `NxsWriter.php` | Pure-PHP writer (`Nxs\Schema`, `Nxs\Writer`) |
| `nxs_ext/nxs_ext.c` | C extension source (`NxsReader`, `NxsObject`) |
| `nxs_ext/config.m4` | Extension build configuration |
| `nxs_ext/build.sh` | Compiles the C extension |

## Query engine

```php
require_once 'Nxs.php';
use Nxs\{Reader, NxsEq, NxsGt, NxsLt};

$reader = new Reader(file_get_contents('data.nxb'));

// Count matching records
$n = $reader->where(new NxsEq('active', true))
           ->where(new NxsGt('score', 80.0))  // chained = AND
           ->count();

// Or use nxs_and()
$q = $reader->where(nxs_and(new NxsEq('active', true), new NxsGt('score', 80.0)));
foreach ($q->getIterator() as $obj) {
    echo $obj->getStr('username') . "\n";
}

// First match or null
$first = $reader->where(new NxsGt('score', 99.0))->first();

// All records
foreach ($reader->all()->getIterator() as $obj) { ... }
```

### Predicates

| Class | Matches |
|-------|---------|
| `NxsEq($key, $value)` | equality — bool, string, int, float |
| `NxsGt($key, $v)` / `NxsLt($key, $v)` | numeric > / < |
| `nxs_and(...)` / `nxs_or(...)` / `nxs_not($p)` | combinators |

Predicates operate on `NxsObject` directly — field access is lazy.

---

For the format specification see [`SPEC.md`](../SPEC.md). For cross-language examples see [`GETTING_STARTED.md`](../GETTING_STARTED.md).
