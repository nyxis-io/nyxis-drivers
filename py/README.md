# NXS — Python

Zero-copy `.nxb` reader for Python 3.8+. Pure-Python implementation with an optional C extension for hot-path columnar scans.

## Requirements

Python 3.8+. No pip install, no dependencies. The C extension requires a C compiler and Python headers.

## Read a file

```python
from nxs import NxsReader

buf = open("data.nxb", "rb").read()   # or mmap.mmap() for true zero-copy
reader = NxsReader(buf)

print(reader.record_count)             # instant — read from tail-index, no parse pass
obj = reader.record(42)                # O(1) seek
print(obj.get_str("username"))
print(obj.get_f64("score"))
print(obj.get_bool("active"))
```

## Columnar scan

```python
scores = reader.scan_f64("score")      # list of all values for one field

total = reader.sum_f64("score")
low   = reader.min_f64("score")
high  = reader.max_f64("score")
ages  = reader.sum_i64("age")
```

## C extension (hot path)

Build once:

```bash
bash build_ext.sh
```

Use the same API, significantly faster for columnar work:

```python
import _nxs

reader = _nxs.Reader(buf)
print(reader.record(42).get_str("username"))   # ~374 ns vs ~1.2 µs pure Python
total = reader.sum_f64("score")                # 3.15 ms at 1M records
```

## Write a file

```python
from nxs_writer import NxsSchema, NxsWriter

schema = NxsSchema(["id", "username", "score", "active"])
w = NxsWriter(schema)

w.begin_object()
w.write_i64(0, 42)
w.write_str(1, "alice")
w.write_f64(2, 9.5)
w.write_bool(3, True)
w.end_object()

data: bytes = w.finish()

# Convenience: write from a list of dicts
data2 = NxsWriter.from_records(
    ["id", "username", "score"],
    [{"id": 1, "username": "bob", "score": 8.2}]
)
```

## Tests

```bash
python test_nxs.py       # pure-Python
python test_c_ext.py     # C extension (requires build_ext.sh first)
```

## Benchmarks

```bash
python bench.py          # pure-Python vs json.loads
python bench_c.py        # C extension vs json.loads
```

## Files

| File | Purpose |
| :--- | :--- |
| `nxs.py` | Pure-Python reader |
| `nxs_writer.py` | Pure-Python writer |
| `_nxs.c` | C extension source |
| `build_ext.sh` | Compiles `_nxs.c` → `_nxs.cpython-*.so` |

## Query engine

```python
from nxs import NxsReader, Eq, Gt, Lt, And, Or, Not, Query

data = open("data.nxb", "rb").read()
r = NxsReader(data)

# Count matching records
n = r.where(And(Eq("active", True), Gt("score", 80.0))).count()

# Iterate — yields dicts
for rec in r.where(Eq("active", True)):
    print(rec["username"])

# First match or None
first = r.where(Gt("score", 99.0)).first()

# No predicate = all records
total = r.where().count()
```

### Predicates

| Class | Matches |
|-------|---------|
| `Eq(key, value)` | equality — bool, str, int, float |
| `Gt(key, v)` / `Lt(key, v)` | numeric > / < |
| `Gte(key, v)` / `Lte(key, v)` | numeric >= / <= |
| `And(p1, p2)` / `Or(p1, p2)` / `Not(p)` | combinators |

Operator overloads: `p1 & p2`, `p1 | p2`, `~p`.

---

For the format specification see [`SPEC.md`](../SPEC.md). For cross-language examples see [`GETTING_STARTED.md`](../GETTING_STARTED.md).
