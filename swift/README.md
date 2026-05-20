# NXS — Swift

Zero-copy `.nxb` reader and direct-to-buffer writer in Swift 5.9+. Uses `Foundation.Data` for memory mapping; no third-party dependencies.

## Build & Test

Requires Swift 5.9+ (Xcode 15+ or swift.org toolchain).

```bash
swift run nxs-test ../js/fixtures     # smoke tests
swift run -c release nxs-bench ../js/fixtures   # benchmark
```

## Read a file

```swift
import NXS

let data = try Data(contentsOf: URL(fileURLWithPath: "data.nxb"))
let reader = try NXSReader(data)

print(reader.recordCount)   // Int
print(reader.keys)          // [String]

let obj = try reader.record(42)
let id:     Int64  = try obj.getI64("id")
let score:  Double = try obj.getF64("score")
let active: Bool   = try obj.getBool("active")
let name:   String = try obj.getStr("username")

// Slot optimisation
let scoreSlot = try reader.slot("score")
let s: Double = try obj.getF64BySlot(scoreSlot)

// Bulk reducers
let sum:  Double  = try reader.sumF64("score")
let sumi: Int64   = try reader.sumI64("id")
let mn:   Double? = try reader.minF64("score")
let mx:   Double? = try reader.maxF64("score")
```

## Write a file

```swift
import NXS

let schema = NXSSchema(keys: ["id", "username", "score", "active"])
let w = NXSWriter(schema: schema)

w.beginObject()
w.writeI64(slot: 0, value: 42)
w.writeStr(slot: 1, value: "alice")
w.writeF64(slot: 2, value: 9.5)
w.writeBool(slot: 3, value: true)
w.endObject()

let bytes: [UInt8] = w.finish()

// Convenience: write from [[String: Any]]
let bytes2 = NXSWriter.fromRecords(
    keys: ["id", "username", "score"],
    records: [["id": 1, "username": "bob", "score": 8.2]]
)
```

## Files

| File | Purpose |
| :--- | :--- |
| `Sources/NXS/NXSReader.swift` | Reader (`NXSReader`, `NYXObject`) |
| `Sources/NXS/NXSWriter.swift` | Writer (`NXSSchema`, `NXSWriter`) |

## Query engine

```swift
import NXS

let data = try Data(contentsOf: url)
let reader = try NXSReader(data)

// Count matching records
let n = reader.where(and(eq("active", true), gt("score", 80.0))).count()

// Iterate — yields NYXObject
for obj in reader.where(eq("active", true)) {
    print(try? obj.getStr("username"))
}

// First match or nil
let first = reader.where(gt("score", 99.0)).first()

// All records
for obj in reader.all { ... }
```

### Predicate functions

| Function | Matches |
|----------|---------|
| `eq(_:_:)` | equality — Bool, Int64, Double, String |
| `gt(_:_:)` / `lt(_:_:)` | numeric > / < |
| `and(_:_:)` / `or(_:_:)` / `not(_:)` | combinators |

`NxsQuery` conforms to `Sequence` — use with `for…in`, `map`, `filter`, etc.

---

For the format specification see [`SPEC.md`](../SPEC.md). For cross-language examples see [`GETTING_STARTED.md`](../GETTING_STARTED.md).
