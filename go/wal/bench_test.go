package wal

import (
	"context"
	"slices"
	"strings"
	"sync"
	"testing"
	"time"

	nxs "github.com/nyxis-io/nyxis-drivers/go"
)

// benchBody approximates a journal envelope (well under 4 KiB).
var benchBody = strings.Repeat("e", 512)

func benchAppend(w *WAL, id int64) error {
	_, err := w.Append(context.Background(), func(e *nxs.ObjectEncoder) {
		e.WriteI64(0, id)
		e.WriteStr(1, benchBody)
	})
	return err
}

// runAppendBench spreads b.N appends over writers goroutines and reports
// throughput and latency percentiles.
func runAppendBench(b *testing.B, opts Options, writers int) {
	w, _, err := Open(b.TempDir(), testSchema, opts)
	if err != nil {
		b.Fatal(err)
	}
	defer w.Close()

	lat := make([]time.Duration, b.N)
	var next sync.Mutex
	i := 0
	claim := func() int {
		next.Lock()
		defer next.Unlock()
		if i >= b.N {
			return -1
		}
		i++
		return i - 1
	}

	b.ResetTimer()
	start := time.Now()
	var wg sync.WaitGroup
	for range writers {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for n := claim(); n >= 0; n = claim() {
				t0 := time.Now()
				if err := benchAppend(w, int64(n)); err != nil {
					b.Error(err)
					return
				}
				lat[n] = time.Since(t0)
			}
		}()
	}
	wg.Wait()
	elapsed := time.Since(start)
	b.StopTimer()

	slices.Sort(lat)
	pct := func(p float64) float64 {
		return float64(lat[min(len(lat)-1, int(p*float64(len(lat))))]) / float64(time.Millisecond)
	}
	b.ReportMetric(float64(b.N)/elapsed.Seconds(), "appends/s")
	b.ReportMetric(pct(0.50), "p50-ms")
	b.ReportMetric(pct(0.99), "p99-ms")
}

func BenchmarkAppend_SyncGroup_32Writers(b *testing.B) {
	b.Run("window=0", func(b *testing.B) { runAppendBench(b, Options{Sync: SyncGroup}, 32) })
	b.Run("window=2ms", func(b *testing.B) {
		runAppendBench(b, Options{Sync: SyncGroup, GroupWindow: 2 * time.Millisecond}, 32)
	})
}

func BenchmarkAppend_SyncEachAppend(b *testing.B) {
	runAppendBench(b, Options{Sync: SyncEachAppend}, 1)
}

func BenchmarkScan_100k(b *testing.B) {
	const records = 100_000
	dir := b.TempDir()
	w, _, err := Open(dir, testSchema, Options{Sync: SyncManual})
	if err != nil {
		b.Fatal(err)
	}
	for i := range records {
		if err := benchAppend(w, int64(i)); err != nil {
			b.Fatal(err)
		}
	}
	if err := w.Close(); err != nil {
		b.Fatal(err)
	}

	b.ResetTimer()
	for b.Loop() {
		s, err := OpenScanner(dir, Offset{})
		if err != nil {
			b.Fatal(err)
		}
		n := 0
		for s.Next() {
			if _, ok := s.Record().GetI64("id"); !ok {
				b.Fatal("id missing")
			}
			n++
		}
		if err := s.Err(); err != nil {
			b.Fatal(err)
		}
		s.Close()
		if n != records {
			b.Fatalf("scanned %d records; want %d", n, records)
		}
	}
	b.ReportMetric(float64(records)*float64(b.N)/b.Elapsed().Seconds(), "records/s")
}
