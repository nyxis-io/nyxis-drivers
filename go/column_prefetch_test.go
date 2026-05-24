package nxs

import (
	"os"
	"path/filepath"
	"sync/atomic"
	"testing"
)

func TestPrefetchColumnSingleFetch(t *testing.T) {
	fixture := filepath.Join("..", "..", "nyxis", "conformance", "columnar_flat8_dense_100.nxb")
	data, err := os.ReadFile(fixture)
	if err != nil {
		t.Skipf("fixture missing: %v", err)
	}
	var fetches atomic.Int32
	r, err := NewReader(data, WithFetchRange(func(off, length int64) ([]byte, error) {
		fetches.Add(1)
		return sliceInt64(data, off, length)
	}))
	if err != nil {
		t.Fatal(err)
	}
	if r.LayoutKind() != LayoutColumnar {
		t.Fatalf("layout = %v, want columnar", r.LayoutKind())
	}
	if err := r.PrefetchColumn("score"); err != nil {
		t.Fatal(err)
	}
	if got := fetches.Load(); got != 1 {
		t.Fatalf("prefetch_column fetches = %d, want 1", got)
	}
	sum := r.ColSumF64("score")
	if got := fetches.Load(); got != 1 {
		t.Fatalf("col_sum_f64 added fetches: got %d, want 1", got)
	}
	const want = 2475.0 // sum(i*0.5) for i=0..99
	if sum != want {
		t.Fatalf("col_sum_f64(score) = %v, want %v", sum, want)
	}
	if r.CacheStats().ColumnFetchesIssued != 1 {
		t.Fatalf("column_fetches_issued = %d, want 1", r.CacheStats().ColumnFetchesIssued)
	}
}
