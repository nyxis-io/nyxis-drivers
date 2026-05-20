// NXS Query Engine — lazy predicate-filtered iteration over NxsReader records.
// Mirrors the Go query.go reference implementation.
//
// Usage:
//   val q = reader.where(eq("active", true) and gt("score", 80.0))
//   val count = q.count()
//   val first = q.first()
package nxs

// ── Predicates ────────────────────────────────────────────────────────────────

interface Predicate {
    fun test(record: NxsObject): Boolean

    infix fun and(other: Predicate) = Predicate { this.test(it) && other.test(it) }

    infix fun or(other: Predicate) = Predicate { this.test(it) || other.test(it) }

    fun not() = Predicate { !this.test(it) }
}

/** SAM-style factory: `Predicate { rec -> … }` */
fun Predicate(block: (NxsObject) -> Boolean): Predicate =
    object : Predicate {
        override fun test(record: NxsObject) = block(record)
    }

/**
 * Returns a predicate that passes when [key] == [value].
 * Supported value types: String, Long, Double, Boolean.
 * Int values are widened to Long automatically.
 */
fun eq(
    key: String,
    value: Any,
): Predicate =
    Predicate { rec ->
        try {
            when (value) {
                is Boolean -> rec.tryGetBool(key) == value
                is String -> rec.tryGetStr(key) == value
                is Long -> rec.tryGetI64(key) == value
                is Int -> rec.tryGetI64(key) == value.toLong()
                is Double -> rec.tryGetF64(key) == value
                else -> false
            }
        } catch (_: NxsError) {
            false
        }
    }

/** Passes when the F64 field [key] > [value]. */
fun gt(
    key: String,
    value: Double,
): Predicate =
    Predicate { rec ->
        rec.tryGetF64(key)?.let { v -> v > value } ?: false
    }

/** Passes when the F64 field [key] < [value]. */
fun lt(
    key: String,
    value: Double,
): Predicate =
    Predicate { rec ->
        rec.tryGetF64(key)?.let { v -> v < value } ?: false
    }

/** Passes when the F64 field [key] >= [value]. */
fun gte(
    key: String,
    value: Double,
): Predicate =
    Predicate { rec ->
        rec.tryGetF64(key)?.let { v -> v >= value } ?: false
    }

/** Passes when the F64 field [key] <= [value]. */
fun lte(
    key: String,
    value: Double,
): Predicate =
    Predicate { rec ->
        rec.tryGetF64(key)?.let { v -> v <= value } ?: false
    }

// ── Query ─────────────────────────────────────────────────────────────────────

/**
 * A lazy, filtered view over the records of an [NxsReader].
 *
 * Create via the extension functions:
 *   reader.where(pred)   — filtered
 *   reader.all           — all records
 */
class Query(private val reader: NxsReader, private val pred: Predicate?) {
    /** Returns a cold [Sequence] that yields every matching [NxsObject]. */
    fun asSequence(): Sequence<NxsObject> =
        sequence {
            for (i in 0 until reader.recordCount) {
                val rec = reader.record(i)
                if (pred == null || pred.test(rec)) yield(rec)
            }
        }

    /** Returns the count of matching records. */
    fun count(): Int = asSequence().count()

    /** Returns the first matching record, or null if none match. */
    fun first(): NxsObject? = asSequence().firstOrNull()
}

// ── Extensions on NxsReader ───────────────────────────────────────────────────

/** Returns a [Query] filtered by [pred]. */
fun NxsReader.where(pred: Predicate) = Query(this, pred)

/** Returns a [Query] that yields all records. */
val NxsReader.all: Query get() = Query(this, null)
