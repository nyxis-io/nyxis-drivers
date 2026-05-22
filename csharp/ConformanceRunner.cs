// NXS conformance runner for C#.
// Add to csharp/ project as a file, then:
//   dotnet run --project csharp/ -- ../conformance/
// Or build standalone:
//   dotnet-script conformance/run_csharp.cs conformance/
//
// To compile as a standalone .cs alongside NxsReader.cs:
//   cd csharp
//   dotnet run --project . -- ../conformance/
//
// This file is designed to be added as an additional file in the csharp project.
// Set as the entry point by uncommenting the main below and commenting out Program.cs main.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using Nxs;

namespace Nxs.Conformance;

public static class ConformanceRunner
{
    static double ApproxEq(double a, double b)
    {
        if (a == b) return 0;
        double diff = Math.Abs(a - b);
        double mag = Math.Max(Math.Abs(a), Math.Abs(b));
        if (mag < 1e-300) return diff < 1e-300 ? 0 : 1;
        return diff / mag;
    }

    static bool ValuesMatch(object? actual, JsonElement expected)
    {
        switch (expected.ValueKind)
        {
            case JsonValueKind.Null:
                return actual == null;
            case JsonValueKind.True:
            case JsonValueKind.False:
                return actual is bool b && b == expected.GetBoolean();
            case JsonValueKind.Number:
                double ev = expected.GetDouble();
                return actual switch
                {
                    long l => ApproxEq((double)l, ev) < 1e-9,
                    double d => ApproxEq(d, ev) < 1e-9,
                    int i => ApproxEq((double)i, ev) < 1e-9,
                    _ => false,
                };
            case JsonValueKind.String:
                return actual is string s && s == expected.GetString();
            case JsonValueKind.Array:
                if (actual is not object?[] arr) return false;
                var elems = expected.EnumerateArray().ToArray();
                if (arr.Length != elems.Length) return false;
                for (int i = 0; i < elems.Length; i++)
                    if (!ValuesMatch(arr[i], elems[i])) return false;
                return true;
            default:
                return false;
        }
    }

    static void RunPositive(string dir, string name, JsonElement expected)
    {
        var nxbPath = Path.Combine(dir, $"{name}.nxb");
        var reader = new NxsReader(File.ReadAllBytes(nxbPath));

        if (expected.TryGetProperty("record_count", out var jrc))
        {
            int expCount = jrc.GetInt32();
            if (reader.RecordCount != expCount)
                throw new Exception($"record_count: expected {expCount}, got {reader.RecordCount}");
        }

        if (expected.TryGetProperty("keys", out var jkeys))
        {
            var expKeys = jkeys.EnumerateArray().Select(k => k.GetString()!).ToArray();
            for (int i = 0; i < expKeys.Length; i++)
            {
                if (i >= reader.Keys.Length)
                    throw new Exception($"key[{i}] missing (expected {expKeys[i]})");
                if (reader.Keys[i] != expKeys[i])
                    throw new Exception($"key[{i}]: expected \"{expKeys[i]}\", got \"{reader.Keys[i]}\"");
            }
        }

        if (expected.TryGetProperty("records", out var jrecs))
        {
            int ri = 0;
            foreach (var expRec in jrecs.EnumerateArray())
            {
                var obj = reader.Record(ri);
                foreach (var kv in expRec.EnumerateObject())
                {
                    string key = kv.Name;
                    if (kv.Value.ValueKind == JsonValueKind.Null) continue;

                    int slot = Array.IndexOf(reader.Keys, key);
                    if (slot < 0) throw new Exception($"rec[{ri}].{key}: key not in schema");

                    byte sigil = slot < reader.KeySigils.Length ? reader.KeySigils[slot] : (byte)0x3D;
                    object? actual = obj.TryGetField(slot, sigil);

                    if (!ValuesMatch(actual, kv.Value))
                        throw new Exception($"rec[{ri}].{key}: expected {kv.Value}, got {actual}");
                }
                ri++;
            }
        }
    }

    static void RunNegative(string dir, string name, string expectedCode)
    {
        var nxbPath = Path.Combine(dir, $"{name}.nxb");
        var data = File.ReadAllBytes(nxbPath);

        try
        {
            _ = new NxsReader(data);
            throw new Exception($"expected error {expectedCode} but reader succeeded");
        }
        catch (NxsException e)
        {
            if (e.Code != expectedCode)
                throw new Exception($"expected error {expectedCode}, got: {e.Code} ({e.Message})");
        }
    }

    public static int Run(string[] args)
    {
        string dir = args.Length > 0 ? args[0] : ".";

        var entries = Directory.GetFiles(dir, "*.expected.json")
            .Select(f => Path.GetFileNameWithoutExtension(f.Replace(".expected", "")))
            .OrderBy(n => n)
            .ToArray();

        int passed = 0, failed = 0;

        foreach (var name in entries)
        {
            var jsonPath = Path.Combine(dir, $"{name}.expected.json");
            using var doc = JsonDocument.Parse(File.ReadAllText(jsonPath));
            var root = doc.RootElement;

            bool isNegative = root.TryGetProperty("error", out _);
            try
            {
                if (isNegative)
                {
                    string code = root.GetProperty("error").GetString()!;
                    RunNegative(dir, name, code);
                }
                else
                {
                    RunPositive(dir, name, root);
                }
                Console.WriteLine($"  PASS  {name}");
                passed++;
            }
            catch (Exception e)
            {
                Console.Error.WriteLine($"  FAIL  {name} — {e.Message}");
                failed++;
            }
        }

        Console.WriteLine($"\n{passed} passed, {failed} failed");
        return failed > 0 ? 1 : 0;
    }
}
