// NXS Query Engine — predicate-based lazy filtering over NxsReader
// Usage:
//   var q = reader.Where(Pred.Eq("active", (object)true));
//   int n = q.Count();
//   NxsObject? first = q.First();
using System;
using System.Collections;
using System.Collections.Generic;
using System.Linq;

namespace Nxs;

// ── Predicates ────────────────────────────────────────────────────────────────

/// <summary>A function that tests a single record and returns true if it matches.</summary>
public delegate bool NxsPredicate(NxsObject record);

/// <summary>Factory methods for composing NxsPredicate instances.</summary>
public static class Pred
{
    /// <summary>
    /// Returns a predicate that matches records where <paramref name="key"/> equals
    /// <paramref name="value"/>. Supported value types: bool, long, double, string.
    /// </summary>
    public static NxsPredicate Eq(string key, object value) => rec => value switch
    {
        bool b => TryGet(rec, rec.GetBool, key, out bool gb) && gb == b,
        long l => TryGet(rec, rec.GetI64, key, out long gl) && gl == l,
        double d => TryGet(rec, rec.GetF64, key, out double gd) && gd == d,
        string s => TryGet(rec, rec.GetStr, key, out string? gs) && gs == s,
        _ => false,
    };

    /// <summary>Returns a predicate that matches records where <paramref name="key"/> &gt; <paramref name="value"/>.</summary>
    public static NxsPredicate Gt(string key, double value) =>
        rec => TryGet(rec, rec.GetF64, key, out double v) && v > value;

    /// <summary>Returns a predicate that matches records where <paramref name="key"/> &lt; <paramref name="value"/>.</summary>
    public static NxsPredicate Lt(string key, double value) =>
        rec => TryGet(rec, rec.GetF64, key, out double v) && v < value;

    /// <summary>Returns a predicate that passes only when both <paramref name="a"/> and <paramref name="b"/> pass.</summary>
    public static NxsPredicate And(NxsPredicate a, NxsPredicate b) => rec => a(rec) && b(rec);

    /// <summary>Returns a predicate that passes when either <paramref name="a"/> or <paramref name="b"/> passes.</summary>
    public static NxsPredicate Or(NxsPredicate a, NxsPredicate b) => rec => a(rec) || b(rec);

    /// <summary>Returns a predicate that inverts <paramref name="p"/>.</summary>
    public static NxsPredicate Not(NxsPredicate p) => rec => !p(rec);

    // ── Helpers ───────────────────────────────────────────────────────────────

    private static bool TryGet(Func<string, bool> getter, string key, out bool result)
    {
        // Use HasField to avoid exception-as-control-flow overhead.
        // The rec reference is captured via the getter delegate's target, but we
        // need the NxsObject for HasField. Instead we delegate to the overload
        // that takes the record directly.
        result = default;
        try { result = getter(key); return true; }
        catch (NxsException) { return false; }
    }

    private static bool TryGet(NxsObject rec, Func<string, bool> getter, string key, out bool result)
    {
        if (!rec.HasField(key)) { result = default; return false; }
        result = getter(key);
        return true;
    }

    private static bool TryGet(NxsObject rec, Func<string, long> getter, string key, out long result)
    {
        if (!rec.HasField(key)) { result = default; return false; }
        result = getter(key);
        return true;
    }

    private static bool TryGet(NxsObject rec, Func<string, double> getter, string key, out double result)
    {
        if (!rec.HasField(key)) { result = default; return false; }
        result = getter(key);
        return true;
    }

    private static bool TryGet(NxsObject rec, Func<string, string?> getter, string key, out string? result)
    {
        if (!rec.HasField(key)) { result = default; return false; }
        result = getter(key);
        return true;
    }
}

// ── Query ─────────────────────────────────────────────────────────────────────

/// <summary>
/// A lazy, filterable view over an <see cref="NxsReader"/>.
/// Created via <see cref="NxsReaderExtensions.Where"/> or <see cref="NxsReaderExtensions.All"/>.
/// </summary>
public sealed class NxsQuery : IEnumerable<NxsObject>
{
    private readonly NxsReader _reader;
    private readonly NxsPredicate? _pred;

    internal NxsQuery(NxsReader reader, NxsPredicate? pred = null)
    {
        _reader = reader;
        _pred = pred;
    }

    /// <inheritdoc/>
    public IEnumerator<NxsObject> GetEnumerator()
    {
        int n = _reader.RecordCount;
        for (int i = 0; i < n; i++)
        {
            var rec = _reader.Record(i);
            if (_pred is null || _pred(rec))
                yield return rec;
        }
    }

    IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();

    /// <summary>Returns the number of records that match the predicate.</summary>
    public int Count()
    {
        int n = 0;
        foreach (var _ in this) n++;
        return n;
    }

    /// <summary>Returns the first matching record, or <c>null</c> if none match.</summary>
    public NxsObject? First()
    {
        foreach (var rec in this)
            return rec;
        return null;
    }
}

// ── Extensions on NxsReader ───────────────────────────────────────────────────

/// <summary>Extension methods that add query capabilities to <see cref="NxsReader"/>.</summary>
public static class NxsReaderExtensions
{
    /// <summary>Returns a <see cref="NxsQuery"/> that yields only records matching <paramref name="pred"/>.</summary>
    public static NxsQuery Where(this NxsReader reader, NxsPredicate pred) => new(reader, pred);

    /// <summary>Returns a <see cref="NxsQuery"/> that yields every record in the file.</summary>
    public static NxsQuery All(this NxsReader reader) => new(reader);
}
