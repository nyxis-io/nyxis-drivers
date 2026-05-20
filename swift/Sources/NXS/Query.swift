// NXS Query Engine — lazy predicate-filtered iteration over NXSReader
// Mirrors the Go query engine (go/query.go).
//
// Usage:
//   let active = r.where(eq("active", true)).count()
//   let highScore = r.where(and(eq("active", true), gt("score", 80.0))).count()
//   if let first = r.where(gt("score", 99.0)).first() { ... }

// ── Predicates ────────────────────────────────────────────────────────────────

/// A predicate that tests a single NYXObject. Returns false on any read error.
public typealias NxsPredicate = (NYXObject) -> Bool

/// Match records where `key` == `value` (Bool).
public func eq(_ key: String, _ value: Bool) -> NxsPredicate {
    { (try? $0.getBool(key)) == value }
}

/// Match records where `key` == `value` (Int64).
public func eq(_ key: String, _ value: Int64) -> NxsPredicate {
    { (try? $0.getI64(key)) == value }
}

/// Match records where `key` == `value` (Double).
public func eq(_ key: String, _ value: Double) -> NxsPredicate {
    { (try? $0.getF64(key)) == value }
}

/// Match records where `key` == `value` (String).
public func eq(_ key: String, _ value: String) -> NxsPredicate {
    { (try? $0.getStr(key)) == value }
}

/// Match records where numeric field `key` > `value`.
public func gt(_ key: String, _ value: Double) -> NxsPredicate {
    { ((try? $0.getF64(key)) ?? -.infinity) > value }
}

/// Match records where numeric field `key` < `value`.
public func lt(_ key: String, _ value: Double) -> NxsPredicate {
    { ((try? $0.getF64(key)) ?? .infinity) < value }
}

/// Match records that satisfy both predicates.
public func and(_ a: @escaping NxsPredicate, _ b: @escaping NxsPredicate) -> NxsPredicate {
    { a($0) && b($0) }
}

/// Match records that satisfy either predicate.
public func or(_ a: @escaping NxsPredicate, _ b: @escaping NxsPredicate) -> NxsPredicate {
    { a($0) || b($0) }
}

/// Invert a predicate.
public func not(_ p: @escaping NxsPredicate) -> NxsPredicate {
    { !p($0) }
}

// ── Query ─────────────────────────────────────────────────────────────────────

/// A lazy, filterable sequence of NYXObjects read from an NXSReader.
///
/// Create via `reader.where(_:)` or `reader.all`:
/// ```swift
/// let n = reader.where(eq("active", true)).count()
/// let first = reader.where(gt("score", 90.0)).first()
/// ```
public struct NxsQuery: Sequence {
    public typealias Element = NYXObject

    private let reader: NXSReader
    private let pred: NxsPredicate?

    /// Internal init — use `NXSReader.where(_:)` or `NXSReader.all` instead.
    public init(_ reader: NXSReader, pred: NxsPredicate? = nil) {
        self.reader = reader
        self.pred   = pred
    }

    public func makeIterator() -> AnyIterator<NYXObject> {
        var index = 0
        let n = reader.recordCount
        let p = pred
        return AnyIterator {
            while index < n {
                guard let rec = try? reader.record(index) else { index += 1; continue }
                index += 1
                if p == nil || p!(rec) { return rec }
            }
            return nil
        }
    }

    /// Return the number of matching records (eager).
    public func count() -> Int {
        reduce(0) { c, _ in c + 1 }
    }

    /// Return the first matching record, or nil if none match.
    public func first() -> NYXObject? {
        first(where: { _ in true })
    }
}

// ── NXSReader extensions ──────────────────────────────────────────────────────

extension NXSReader {
    /// Return a query that yields only records matching `pred`.
    public func `where`(_ pred: @escaping NxsPredicate) -> NxsQuery {
        NxsQuery(self, pred: pred)
    }

    /// Return a query that yields every record in the file.
    public var all: NxsQuery { NxsQuery(self) }
}
