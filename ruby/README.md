# NXS — Ruby

Zero-copy `.nxb` reader for Ruby 3.x. Pure-Ruby implementation with an optional C extension for hot-path columnar scans. No gems required.

## Requirements

Ruby 3.0+. The C extension requires a C compiler and Ruby headers (`ruby-dev` / `ruby-devel`).

## Read a file

```ruby
require_relative "nxs"

bytes  = File.binread("data.nxb")
reader = Nxs::Reader.new(bytes)

puts reader.record_count              # instant — read from tail-index, no parse pass
obj = reader.record(42)               # O(1) seek
puts obj.get_str("username")
puts obj.get_f64("score")
puts obj.get_bool("active")
puts obj.get_i64("id")
```

## Columnar reducers

```ruby
total = reader.sum_f64("score")
low   = reader.min_f64("score")
high  = reader.max_f64("score")
ages  = reader.sum_i64("age")
```

## C extension (hot path)

Build once:

```bash
bash ext/build.sh
```

```ruby
require_relative "ext/nxs/nxs_ext"   # loads Nxs::CReader and Nxs::CObject

reader = Nxs::CReader.new(bytes)
puts reader.record(42).get_str("username")
puts reader.sum_f64("score")          # 6.78 ms at 1M records vs 942 ms pure Ruby
```

At 1M records the C extension is **139× faster** than pure Ruby for `sum_f64`, and **5.6× faster** than `JSON.parse`.

## Write a file

```ruby
require_relative "nxs_writer"

schema = Nxs::Schema.new(["id", "username", "score", "active"])
w = Nxs::Writer.new(schema)

w.begin_object
w.write_i64(0, 42)
w.write_str(1, "alice")
w.write_f64(2, 9.5)
w.write_bool(3, true)
w.end_object

data = w.finish   # binary String (encoding ASCII-8BIT)

# Convenience: write from an array of hashes
data2 = Nxs::Writer.from_records(
  ["id", "username", "score"],
  [{ "id" => 1, "username" => "bob", "score" => 8.2 }]
)
```

## Tests

```bash
ruby test.rb ../js/fixtures    # 22 tests
```

## Benchmarks

```bash
ruby bench.rb ../js/fixtures       # pure Ruby vs JSON
ruby bench_c.rb ../js/fixtures     # C extension vs JSON
```

## Files

| File | Purpose |
| :--- | :--- |
| `nxs.rb` | Pure-Ruby reader (`Nxs::Reader`, `Nxs::Object`) |
| `nxs_writer.rb` | Pure-Ruby writer (`Nxs::Schema`, `Nxs::Writer`) |
| `ext/nxs/nxs_ext.c` | C extension source (`Nxs::CReader`, `Nxs::CObject`) |
| `ext/nxs/extconf.rb` | Extension build configuration |
| `ext/build.sh` | Compiles the C extension |

## Query engine

```ruby
require_relative 'nxs'

data   = File.binread("data.nxb")
reader = Nxs::Reader.new(data)

# Count matching records
n = reader.where(Nxs::Eq.new("active", true) & Nxs::Gt.new("score", 80.0)).count

# Iterate — yields Nxs::Object
reader.where(Nxs::Eq.new("active", true)).each do |obj|
  puts obj.get_str("username")
end

# First match or nil
first = reader.where(Nxs::Gt.new("score", 99.0)).first

# All records
reader.all.each { |obj| ... }
```

### Predicates

| Class | Matches |
|-------|---------|
| `Eq.new(key, value)` | equality — String, Integer, Float, boolean |
| `Gt.new(key, v)` / `Lt.new(key, v)` | numeric comparison |
| `p1 & p2` / `p1 \| p2` / `~p` | And / Or / Not via operator overloads |

`Query` includes `Enumerable` — all `map`, `select`, `reject` etc. are available.

---

For the format specification see [`SPEC.md`](../SPEC.md). For cross-language examples see [`GETTING_STARTED.md`](../GETTING_STARTED.md).
