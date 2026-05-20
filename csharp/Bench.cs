using System;
using System.IO;
using System.Diagnostics;
using System.Text.Json;
using System.Text;

namespace Nxs;

public static class Bench
{
    public static void Run(string dir)
    {
        string nxbPath = Path.Combine(dir, "records_1000000.nxb");
        string jsonPath = Path.Combine(dir, "records_1000000.json");
        string csvPath = Path.Combine(dir, "records_1000000.csv");

        if (!File.Exists(nxbPath)) { Console.WriteLine($"fixture not found: {nxbPath}"); return; }

        byte[] nxbData = File.ReadAllBytes(nxbPath);
        byte[] jsonData = File.Exists(jsonPath) ? File.ReadAllBytes(jsonPath) : Array.Empty<byte>();
        byte[] csvData = File.Exists(csvPath) ? File.ReadAllBytes(csvPath) : Array.Empty<byte>();

        var r = new NxsReader(nxbData);
        Console.WriteLine($"\nNXS C# Benchmark — {r.RecordCount} records");
        Console.WriteLine($"  .nxb {nxbData.Length / 1e6:F2} MB   .json {jsonData.Length / 1e6:F2} MB   .csv {csvData.Length / 1e6:F2} MB\n");

        const int RUNS = 5;

        double BenchMs(string label, double baseline, Action body)
        {
            double best = double.PositiveInfinity;
            for (int i = 0; i < RUNS; i++)
            {
                var sw = Stopwatch.StartNew(); body(); sw.Stop();
                double ms = sw.Elapsed.TotalMilliseconds;
                if (ms < best) best = ms;
            }
            string rel = baseline > 0 ? $"  {baseline / best:F1}x faster" : "";
            Console.WriteLine($"  │  {label,-28}  {best,7:F2} ms{rel}");
            return best;
        }

        // JSON sum — System.Text.Json
        double JsonSumScore()
        {
            double sum = 0;
            var doc = JsonDocument.Parse(jsonData);
            foreach (var el in doc.RootElement.EnumerateArray())
                sum += el.GetProperty("score").GetDouble();
            return sum;
        }

        // CSV sum — raw byte scan, score is column 6
        double CsvSumScore()
        {
            double sum = 0;
            int p = 0, size = csvData.Length, line = 0;
            while (p < size)
            {
                int rowEnd = p;
                while (rowEnd < size && csvData[rowEnd] != '\n') rowEnd++;
                if (line > 0)
                {
                    int col = p, c = 0;
                    while (c < 6 && col < rowEnd)
                    {
                        while (col < rowEnd && csvData[col] != ',') col++;
                        col++; c++;
                    }
                    if (c == 6 && col < rowEnd)
                    {
                        int end = col;
                        while (end < rowEnd && csvData[end] != ',' && csvData[end] != '\r') end++;
                        if (double.TryParse(Encoding.ASCII.GetString(csvData, col, end - col),
                                System.Globalization.NumberStyles.Float,
                                System.Globalization.CultureInfo.InvariantCulture, out double v))
                            sum += v;
                    }
                }
                line++; p = rowEnd + 1;
            }
            return sum;
        }

        Console.WriteLine("  ┌─ sum(score) ─────────────────────────────────────────────────────────┐");
        double jsonMs = jsonData.Length > 0 ? BenchMs("JSON parse + loop", 0, () => { _ = JsonSumScore(); }) : 0;
        if (csvData.Length > 0) BenchMs("CSV raw scan", jsonMs, () => { _ = CsvSumScore(); });
        BenchMs("NXS SumF64", jsonMs, () => { _ = r.SumF64("score"); });
        Console.WriteLine("  └──────────────────────────────────────────────────────────────────────┘\n");

        Console.WriteLine("  ┌─ sum(id) ────────────────────────────────────────────────────────────┐");
        BenchMs("NXS SumI64", 0, () => { _ = r.SumI64("id"); });
        Console.WriteLine("  └──────────────────────────────────────────────────────────────────────┘\n");

        Console.WriteLine("  ┌─ random access ×1000 ────────────────────────────────────────────────┐");
        BenchMs("NXS Record(k).GetF64", 0, () =>
        {
            for (int i = 0; i < 1000; i++) _ = r.Record(i * 997 % r.RecordCount).GetF64("score");
        });
        Console.WriteLine("  └──────────────────────────────────────────────────────────────────────┘\n");
    }
}
