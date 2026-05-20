# NXS — Kotlin

Zero-copy `.nxb` reader and direct-to-buffer writer for Kotlin/JVM. Uses only `java.nio.ByteBuffer` plus `org.json` for test JSON parsing.

## Requirements

- JDK 17+
- Gradle 8+ (`gradle wrapper` or system Gradle)

## Build & Test

```bash
cd kotlin
gradle run --args="../js/fixtures"    # smoke tests
```

## Read a file

```kotlin
import nxs.NxsReader

val data = File("data.nxb").readBytes()
val reader = NxsReader(data)

println(reader.recordCount)   // Int
println(reader.keys)          // List<String>

val obj = reader.record(42)
val id:     Long    = obj.getI64("id")
val score:  Double  = obj.getF64("score")
val active: Boolean = obj.getBool("active")
val name:   String  = obj.getStr("username")

// Slot optimisation
val scoreSlot = reader.slot("score")
val s: Double = obj.getF64BySlot(scoreSlot)

// Bulk reducers
val sum:  Double  = reader.sumF64("score")
val sumi: Long    = reader.sumI64("id")
val mn:   Double? = reader.minF64("score")
val mx:   Double? = reader.maxF64("score")
```

## Write a file

```kotlin
import nxs.NxsSchema
import nxs.NxsWriter

val schema = NxsSchema(listOf("id", "username", "score", "active"))
val w = NxsWriter(schema)

w.beginObject()
w.writeI64(0, 42L)
w.writeStr(1, "alice")
w.writeF64(2, 9.5)
w.writeBool(3, true)
w.endObject()

val bytes: ByteArray = w.finish()

// Convenience: write from a list of maps
val bytes2 = NxsWriter.fromRecords(
    listOf("id", "username", "score"),
    listOf(mapOf("id" to 1L, "username" to "bob", "score" to 8.2))
)
```

## Files

| File | Purpose |
| :--- | :--- |
| `src/main/kotlin/nxs/NxsReader.kt` | Reader (`NxsReader`, `NxsObject`) |
| `src/main/kotlin/nxs/NxsWriter.kt` | Writer (`NxsSchema`, `NxsWriter`) |

## Query engine

```kotlin
import nxs.Reader
import nxs.eq
import nxs.gt
import nxs.and

val data = File("data.nxb").readBytes()
val reader = Reader(data)

// Count matching records
val n = reader.where(eq("active", true) and gt("score", 80.0)).count()

// Iterate as a Sequence
reader.where(eq("active", true)).asSequence().forEach { obj ->
    println(obj.tryGetStr("username"))
}

// First match or null
val first = reader.where(gt("score", 99.0)).first()

// All records
reader.all.asSequence().forEach { ... }
```

### Predicates

| Function | Matches |
|----------|---------|
| `eq(key, value)` | equality — Boolean, String, Long, Double |
| `gt(key, v)` / `lt(key, v)` | numeric > / < |
| `p1 and p2` / `p1 or p2` / `p.not()` | infix combinators |

Backed by Kotlin `sequence { }` for lazy evaluation.

---

For the format specification see [`SPEC.md`](../SPEC.md). For cross-language examples see [`GETTING_STARTED.md`](../GETTING_STARTED.md).
