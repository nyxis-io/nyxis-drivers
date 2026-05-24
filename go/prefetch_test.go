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
			return sliceInt64(buf, off, length)
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
			return sliceInt64(buf, off, length)
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

func TestPatternSequential(t *testing.T) {
	d := NewAccessPatternDetector()
	for i := 0; i < 8; i++ {
		d.Observe(i)
	}
	if got := d.Pattern(); got != PatternUnknown {
		t.Fatalf("after 8 obs: got %v, want unknown", got)
	}
	for i := 8; i < 20; i++ {
		d.Observe(i)
	}
	if got := d.Pattern(); got != PatternSequential {
		t.Fatalf("got %v, want sequential", got)
	}
	if got := d.PredictNext(4, 100); len(got) != 4 || got[0] != 20 {
		t.Fatalf("predict_next: %v", got)
	}
}

func buildCompactRecords(t *testing.T, n int) []byte {
	t.Helper()
	schema := NewSchema([]string{"id", "tag"})
	w := NewWriter(schema)
	for i := 0; i < n; i++ {
		w.BeginObject()
		w.WriteI64(0, int64(i))
		w.WriteStr(1, "r"+itoa(i))
		w.EndObject()
	}
	return w.Finish()
}

func TestSequentialUpgrade(t *testing.T) {
	buf := buildCompactRecords(t, 200)
	r, err := NewReader(buf)
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()
	for i := 0; i < 150; i++ {
		_ = r.Record(i)
	}
	r.Warmup()
	stats := r.CacheStats()
	if stats.Strategy != "eager" {
		t.Fatalf("strategy = %q, want eager", stats.Strategy)
	}
	if stats.Pattern != "sequential" {
		t.Fatalf("pattern = %q, want sequential", stats.Pattern)
	}
}

func TestPauseStopsSpeculative(t *testing.T) {
	buf := buildCompactRecords(t, 200)
	r, err := NewReader(buf)
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()
	for i := 0; i < 25; i++ {
		_ = r.Record(i)
	}
	if stats := r.CacheStats(); stats.Pattern != "sequential" {
		t.Fatalf("pattern = %q, want sequential", stats.Pattern)
	}
	before := r.CacheStats().FetchesIssued
	r.PausePrefetch()
	_ = r.Record(26)
	if got := r.CacheStats().FetchesIssued; got != before {
		t.Fatalf("fetches while paused: got %d, want %d", got, before)
	}
	r.ResumePrefetch()
	_ = r.Record(27)
	if got := r.CacheStats().FetchesIssued; got < before {
		t.Fatalf("fetches after resume: got %d, before pause %d", got, before)
	}
}

func TestHintFullEager(t *testing.T) {
	buf := buildCompactRecords(t, 200)
	if len(buf) > EagerThresholdMB*1024*1024 {
		t.Fatalf("fixture too large for eager threshold: %d bytes", len(buf))
	}
	r, err := NewReader(buf, WithHint(HintFull))
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()
	r.Warmup()
	if got := r.CacheStats().Strategy; got != "eager" {
		t.Fatalf("strategy = %q, want eager at open", got)
	}
}

func TestOpenOptionsZeroPageSize(t *testing.T) {
	buf := buildCompactRecords(t, 10)
	_, err := NewReader(buf, WithPageSize(0))
	if err == nil {
		t.Fatal("expected error for page_size=0")
	}
}
