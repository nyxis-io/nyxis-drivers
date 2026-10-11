# NXS — Go

Zero-copy `.nxb` reader for Go 1.26+. No external dependencies.

## Install

```bash
go get github.com/nyxis-io/nyxis-drivers/go
```

## Requirements

Go 1.26+ (see `go.mod`; CI uses 1.26).

## Read a file

```go
import (
    "github.com/nyxis-io/nyxis-drivers/go"
    "os"
)

data, _ := os.ReadFile("data.nxb")
r, err := nxs.NewReader(data)
if err != nil { panic(err) }

fmt.Println(r.RecordCount())       // instant — read from tail-index, no parse pass

obj := r.Record(42)                // O(1) seek
username, _ := obj.GetStr("username")
score, _    := obj.GetF64("score")
```

## Slot handles (hot path)

Resolve a key name to a slot index once, reuse it across every record:

```go
slot := r.Slot("score")
for i := 0; i < r.RecordCount(); i++ {
    v, _ := r.Record(i).GetF64BySlot(slot)
    _ = v
}
```

## Reducers

```go
// Safe — handles any bitmask layout
sum := r.SumF64("score")

// Fast — assumes uniform schema; bitmask computed once from record 0
sum = r.SumF64Fast("score")

// Parallel — fans out across GOMAXPROCS goroutines
sum = r.SumF64FastPar("score", 0)   // 0 = use GOMAXPROCS

min, _ := r.MinF64Fast("score")
max, _ := r.MaxF64Fast("score")
ages   := r.SumI64Fast("age")
```

At 1M records, `SumF64Fast` runs in ~10.9 ms vs ~1.05 s for `json.Unmarshal` (~105× faster).

## Write a file

```go
import "github.com/nyxis-io/nyxis-drivers/go"

schema := nxs.NewSchema([]string{"id", "username", "score", "active"})
w := nxs.NewWriter(schema)

w.BeginObject()
w.WriteI64(0, 42)
w.WriteStr(1, "alice")
w.WriteF64(2, 9.5)
w.WriteBool(3, true)
w.EndObject()

bytes := w.Finish()   // []byte

// Convenience: write from a slice of maps
bytes2 := nxs.FromRecords(
    []string{"id", "username", "score"},
    []map[string]any{{"id": int64(1), "username": "bob", "score": 8.2}},
)
```

## Streaming writer

`StreamWriter` writes the preamble and schema on the first `EndObject`, using the same per-slot sigils as `Writer` (not all-string defaults). Call `Close()` once after all records.

```go
var buf bytes.Buffer
sw, _ := nxs.NewStreamWriter(&buf, schema)
sw.BeginObject()
sw.WriteI64(0, 1)
sw.WriteStr(1, "alice")
_ = sw.EndObject()
_ = sw.Close()
```

TypeManifest sigils match SPEC (`=`, `~`, `?`, `"`, `<`, `^`, `@`).

## Standalone objects

`ObjectEncoder` builds one NYXO object without a file preamble or tail-index; its bytes equal the object region `Writer` emits for the same record. A standalone object carries no schema header, so sigils are fixed per slot up front. A write whose sigil differs from the slot's, an out-of-range slot, or a slot written twice makes `Bytes` return an error.

```go
keys := []string{"id", "name", "seen_at"}
sigils := []byte{'=', '"', '@'}

enc := nxs.NewObjectEncoder(keys, sigils)
enc.WriteI64(0, 42)
enc.WriteStr(1, "alice")
enc.WriteTime(2, time.Now().UnixNano())
obj, err := enc.Bytes() // valid until enc.Reset()

o, err := nxs.DecodeObject(keys, sigils, obj) // bounds-checks every field
name, ok := o.GetStr("name")
seen, _ := o.GetTime("seen_at")
_, ok = o.GetStr("not_in_schema") // ok == false
```

Use `NewObjectDecoder(keys, sigils)` to decode many objects that share a schema. `Object` also gains `GetBytes` (`<`), `GetTime` (`@`), and `Has` (presence; the value of a `^` slot).

## Write-ahead log (`wal`)

Package `github.com/nyxis-io/nyxis-drivers/go/wal` is a durable append-only log of NYXO objects (`.nxsw` v2) for use as an ingestion journal. Records are CRC32C-framed, acknowledged only after a sync covers them, and recovered after a crash by truncating the torn tail of the live segment. See [`wal/FORMAT.md`](wal/FORMAT.md) for the layout, recovery rules, and failure semantics.

```go
import "github.com/nyxis-io/nyxis-drivers/go/wal"

schema := wal.Schema{Fields: []wal.Field{
    {Key: "id", Sigil: '='},
    {Key: "body", Sigil: '"'},
}}
w, report, err := wal.Open("data/journal", schema, wal.Options{}) // SyncGroup by default
defer w.Close()

off, err := w.Append(ctx, func(rec *nxs.ObjectEncoder) {
    rec.WriteI64(0, 1)
    rec.WriteStr(1, "hello")
}) // returns once the record is durable

s, err := wal.OpenScanner("data/journal", wal.Offset{}) // or w.NewLiveScanner(after)
for s.Next() {
    id, _ := s.Record().GetI64("id")
    _ = id
}
err = s.Err()
```

- **Sync policies.** `SyncGroup` (default): one flusher writes everything queued while the previous sync ran, then syncs once; `GroupWindow` adds an optional wait to grow batches. `SyncEachAppend`: one sync per append. `SyncManual`: appends become durable on `Sync`.
- **Sticky failure.** After any short write or sync error, `Append`, `Sync`, and `Rotate` return `ErrWALFailed`. `Close` truncates the live segment to the durable end (retry it if it errors), then `Open` again.
- **Schema evolution.** Opening with the previous schema plus trailing fields seals the live segment and starts a new one. Older records read new keys as absent. Upgrades are roll-forward only: an older binary gets `ErrSchemaMismatch`.
- **Exclusive access.** `Open` holds an `flock` on `<dir>/LOCK`; a second opener gets `ErrLocked`. `OpenScanner` does not lock.
- **Platforms.** Linux uses `fdatasync`; darwin uses `F_FULLFSYNC` (via `os.File.Sync`). Locking is implemented for Linux and darwin.

## Query engine

Filter records lazily without scanning the full dataset:

```go
// Count active users with score > 80
n := r.Where(nxs.And(nxs.Eq("active", true), nxs.Gt("score", 80.0))).Count()

// Iterate matching records — stops early on break
for obj := range r.Where(nxs.Eq("active", true)).Records() {
    username, _ := obj.GetStr("username")
    fmt.Println(username)
}

// First match or nil
first := r.Where(nxs.Gt("score", 99.0)).First()

// Nested path access
city, _ := obj.GetStrPath("address.city")
```

### Predicates

| Constructor | Matches |
|-------------|---------|
| `Eq(key, value)` | field == value (bool, string, int64, float64) |
| `Gt(key, v)` / `Lt(key, v)` | field > v / field < v |
| `Gte(key, v)` / `Lte(key, v)` | field >= v / field <= v |
| `And(p1, p2, ...)` | all predicates pass |
| `Or(p1, p2, ...)` | any predicate passes |
| `Not(p)` | predicate does not pass |

`Records()` is an `iter.Seq[*Object]` — use with `range` (Go 1.22+).
`Count()` is verified zero-allocation via `testing.AllocsPerRun`.

## Tests

```bash
go test ./...
go test -race ./wal/
go test -run TestCrash_NoAckedRecordLost -wal.crash.iters=10000 ./wal/   # nightly crash run
go test -run '^$' -bench . ./wal/                                          # WAL throughput and latency
```

## Benchmarks

```bash
go build -o bench ./cmd/bench
./bench ../js/fixtures
```

Generate fixtures first if needed:

```bash
cargo run --release --bin gen_fixtures -- ../js/fixtures 1000000
```

## Files

| File | Purpose |
| :--- | :--- |
| `nxs.go` | Reader, schema parsing, tail-index, field accessors |
| `fast.go` | Uniform-schema fast path and parallel reducers |
| `writer.go` | `NxsWriter` / `NxsSchema` — direct binary writer |
| `object.go` | `ObjectEncoder` / `DecodeObject` — standalone NYXO objects |
| `wal/` | `.nxsw` v2 write-ahead log (`FORMAT.md`, recovery, group commit) |
| `nxs_test.go` | Test suite |
| `cmd/bench/main.go` | Benchmark binary |

---

For the format specification see [`SPEC.md`](../SPEC.md). For cross-language examples see [`GETTING_STARTED.md`](../GETTING_STARTED.md).
