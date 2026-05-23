package nxs

import (
	"context"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func buildPrefetchRecords(t *testing.T, n int) []byte {
	t.Helper()
	schema := NewSchema([]string{"id", "username", "score", "active"})
	w := NewWriter(schema)
	for i := 0; i < n; i++ {
		w.BeginObject()
		w.WriteI64(0, int64(i))
		w.WriteStr(1, "user_"+itoa(i))
		w.WriteF64(2, float64(i)*0.25)
		w.WriteBool(3, i%2 == 0)
		w.EndObject()
	}
	return w.Finish()
}

func TestCoalesce(t *testing.T) {
	ranges := CoalescePageIndices([]int{3, 4, 6, 7, 12}, 1, DefaultPageSize)
	if len(ranges) != 3 {
		t.Fatalf("got %d ranges, want 3: %#v", len(ranges), ranges)
	}
	if ranges[0].PageStart != 3 || ranges[0].PageEnd != 4 {
		t.Fatalf("range 0: %#v", ranges[0])
	}
	if ranges[1].PageStart != 6 || ranges[1].PageEnd != 7 {
		t.Fatalf("range 1: %#v", ranges[1])
	}
	if ranges[2].PageStart != 12 || ranges[2].PageEnd != 12 {
		t.Fatalf("range 2: %#v", ranges[2])
	}
	if ranges[0].ByteLength != 2*DefaultPageSize {
		t.Fatalf("range 0 byte length = %d, want %d", ranges[0].ByteLength, 2*DefaultPageSize)
	}

	deduped := CoalescePageIndices([]int{3, 3, 4}, 1, DefaultPageSize)
	if len(deduped) != 1 || deduped[0].PageStart != 3 || deduped[0].PageEnd != 4 {
		t.Fatalf("dedupe: %#v", deduped)
	}
}

func TestPrefetchViewportCoalesce(t *testing.T) {
	buf := buildPrefetchRecords(t, 60)
	var ranges [][2]int64
	r, err := NewReader(buf,
		WithMaxPages(64),
		WithCoalesceGapPages(1),
		WithFetchRange(func(off, length int64) ([]byte, error) {
			ranges = append(ranges, [2]int64{off, length})
			return buf[off : off+length], nil
		}),
	)
	if err != nil {
		t.Fatal(err)
	}
	if err := r.PrefetchViewport(context.Background(), 0, 49); err != nil {
		t.Fatal(err)
	}
	if len(ranges) > 3 {
		t.Fatalf("expected ≤3 fetches, got %d: %#v", len(ranges), ranges)
	}
	stats := r.CacheStats()
	if stats.FetchesIssued != len(ranges) {
		t.Fatalf("fetches_issued = %d, want %d", stats.FetchesIssued, len(ranges))
	}
}

func TestLRUEviction(t *testing.T) {
	c := newPageCache(2, 64)
	c.set(0, make([]byte, 64), false)
	c.set(1, make([]byte, 64), false)
	c.get(0)
	c.set(2, make([]byte, 64), false)
	if c.has(1) {
		t.Fatal("page 1 should be evicted")
	}
	if !c.has(0) || !c.has(2) {
		t.Fatal("pages 0 and 2 should remain")
	}

	buf := buildPrefetchRecords(t, 20)
	r, err := NewReader(buf, WithMaxPages(2), WithPageSize(256), WithCoalesceGapPages(0))
	if err != nil {
		t.Fatal(err)
	}
	if err := r.PrefetchViewport(context.Background(), 0, 0); err != nil {
		t.Fatal(err)
	}
	if err := r.PrefetchViewport(context.Background(), 19, 19); err != nil {
		t.Fatal(err)
	}
	stats := r.CacheStats()
	if stats.PagesCached > 2 {
		t.Fatalf("cache grew past max: pages_cached=%d", stats.PagesCached)
	}
}

func TestDedup(t *testing.T) {
	buf := buildPrefetchRecords(t, 10)
	var calls atomic.Int32
	r, err := NewReader(buf,
		WithMaxPages(8),
		WithFetchRange(func(off, length int64) ([]byte, error) {
			calls.Add(1)
			time.Sleep(5 * time.Millisecond)
			return buf[off : off+length], nil
		}),
	)
	if err != nil {
		t.Fatal(err)
	}

	var wg sync.WaitGroup
	wg.Add(2)
	go func() {
		defer wg.Done()
		_ = r.PrefetchViewport(context.Background(), 0, 4)
	}()
	go func() {
		defer wg.Done()
		_ = r.PrefetchViewport(context.Background(), 0, 4)
	}()
	wg.Wait()

	if n := int(calls.Load()); n > 3 {
		t.Fatalf("too many fetches: %d", n)
	}
}
