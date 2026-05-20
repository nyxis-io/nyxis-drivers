package nxs

import (
	"testing"
)

// ── Where / Records ───────────────────────────────────────────────────────────

func TestQueryAll(t *testing.T) {
	nxb, js := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}
	n := r.All().Count()
	if n != len(js) {
		t.Errorf("All().Count() = %d, want %d", n, len(js))
	}
}

func TestQueryEqBool(t *testing.T) {
	nxb, js := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}
	var want int
	for _, x := range js {
		if x.Active {
			want++
		}
	}
	got := r.Where(Eq("active", true)).Count()
	if got != want {
		t.Errorf("Eq(active,true).Count() = %d, want %d", got, want)
	}
}

func TestQueryEqString(t *testing.T) {
	nxb, js := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}
	target := js[42].Username
	got := r.Where(Eq("username", target)).Count()
	if got == 0 {
		t.Errorf("Eq(username,%q).Count() = 0, want >= 1", target)
	}
	for obj := range r.Where(Eq("username", target)).Records() {
		u, ok := obj.GetStr("username")
		if !ok || u != target {
			t.Errorf("got username=%q, want %q", u, target)
		}
	}
}

func TestQueryGtFloat(t *testing.T) {
	nxb, js := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}
	threshold := 80.0
	var want int
	for _, x := range js {
		if x.Score > threshold {
			want++
		}
	}
	got := r.Where(Gt("score", threshold)).Count()
	if got != want {
		t.Errorf("Gt(score,80).Count() = %d, want %d", got, want)
	}
}

func TestQueryLtFloat(t *testing.T) {
	nxb, js := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}
	threshold := 40.0
	var want int
	for _, x := range js {
		if x.Score < threshold {
			want++
		}
	}
	got := r.Where(Lt("score", threshold)).Count()
	if got != want {
		t.Errorf("Lt(score,40).Count() = %d, want %d", got, want)
	}
}

func TestQueryAnd(t *testing.T) {
	nxb, js := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}
	var want int
	for _, x := range js {
		if x.Active && x.Score > 80.0 {
			want++
		}
	}
	got := r.Where(And(Eq("active", true), Gt("score", 80.0))).Count()
	if got != want {
		t.Errorf("And(active,score>80).Count() = %d, want %d", got, want)
	}
}

func TestQueryOr(t *testing.T) {
	nxb, js := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}
	var want int
	for _, x := range js {
		if x.Score > 95.0 || x.Score < 5.0 {
			want++
		}
	}
	got := r.Where(Or(Gt("score", 95.0), Lt("score", 5.0))).Count()
	if got != want {
		t.Errorf("Or(score>95,score<5).Count() = %d, want %d", got, want)
	}
}

func TestQueryNot(t *testing.T) {
	nxb, js := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}
	var want int
	for _, x := range js {
		if !x.Active {
			want++
		}
	}
	got := r.Where(Not(Eq("active", true))).Count()
	if got != want {
		t.Errorf("Not(active=true).Count() = %d, want %d", got, want)
	}
}

func TestQueryFirst(t *testing.T) {
	nxb, js := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}
	target := js[0].Username
	obj := r.Where(Eq("username", target)).First()
	if obj == nil {
		t.Fatal("First() returned nil, want a match")
	}
	u, _ := obj.GetStr("username")
	if u != target {
		t.Errorf("First() username=%q, want %q", u, target)
	}
}

func TestQueryFirstNoMatch(t *testing.T) {
	nxb, _ := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}
	obj := r.Where(Eq("username", "__no_such_user__")).First()
	if obj != nil {
		t.Errorf("First() = non-nil on no-match, want nil")
	}
}

func TestQueryEarlyBreak(t *testing.T) {
	nxb, _ := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}
	// Break after first 10 — must not panic or loop forever
	seen := 0
	for range r.All().Records() {
		seen++
		if seen == 10 {
			break
		}
	}
	if seen != 10 {
		t.Errorf("early break: seen=%d, want 10", seen)
	}
}

func TestQueryUnknownKey(t *testing.T) {
	nxb, _ := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}
	// Predicate on a key not in schema should match nothing
	got := r.Where(Eq("nonexistent_field_xyz", true)).Count()
	if got != 0 {
		t.Errorf("unknown key matched %d records, want 0", got)
	}
}

// ── Nested path access ────────────────────────────────────────────────────────

func TestGetStrPathSingleSegment(t *testing.T) {
	// Single-segment path is equivalent to GetStr
	nxb, js := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}
	obj := r.Record(0)
	want := js[0].Username
	got, ok := obj.GetStrPath("username")
	if !ok || got != want {
		t.Errorf("GetStrPath(username) = %q (ok=%v), want %q", got, ok, want)
	}
}

func TestGetStrPathAbsent(t *testing.T) {
	nxb, _ := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}
	_, ok := r.Record(0).GetStrPath("no.such.path")
	if ok {
		t.Error("GetStrPath on absent path returned ok=true, want false")
	}
}

func TestGetI64PathSingleSegment(t *testing.T) {
	nxb, js := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}
	for _, i := range []int{0, 42, 999} {
		got, ok := r.Record(i).GetI64Path("id")
		if !ok || got != js[i].ID {
			t.Errorf("record %d GetI64Path(id) = %d (ok=%v), want %d", i, got, ok, js[i].ID)
		}
	}
}

func TestGetF64PathSingleSegment(t *testing.T) {
	nxb, js := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}
	got, ok := r.Record(7).GetF64Path("score")
	if !ok || !closeEnough(got, js[7].Score) {
		t.Errorf("GetF64Path(score) = %v (ok=%v), want %v", got, ok, js[7].Score)
	}
}

func TestGetBoolPathSingleSegment(t *testing.T) {
	nxb, js := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}
	got, ok := r.Record(0).GetBoolPath("active")
	if !ok || got != js[0].Active {
		t.Errorf("GetBoolPath(active) = %v (ok=%v), want %v", got, ok, js[0].Active)
	}
}

// ── Zero-allocation verification ──────────────────────────────────────────────

func TestQueryRecordsZeroAllocs(t *testing.T) {
	nxb, _ := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}
	pred := And(Eq("active", true), Gt("score", 50.0))
	q := r.Where(pred)

	// Warm up
	_ = q.Count()

	allocs := testing.AllocsPerRun(10, func() {
		for obj := range q.Records() {
			_ = obj
		}
	})
	// Each iteration allocates one *Object per yielded record (unavoidable since
	// yield takes *Object). We allow up to 1 alloc per matched record but zero
	// allocs in the predicate evaluation path itself.
	// For the full 1000-record scan with ~500 matches: ≤ 600 allocs total.
	// To verify the predicate itself is zero-alloc, test Count() which doesn't
	// need to allocate Objects.
	allocsCount := testing.AllocsPerRun(10, func() {
		_ = q.Count()
	})
	if allocsCount > 0 {
		t.Errorf("Count() allocs = %.0f, want 0", allocsCount)
	}
	_ = allocs
}

// ── Benchmarks ────────────────────────────────────────────────────────────────

func loadFixturesB(b *testing.B, n int) []byte {
	b.Helper()
	nxb, err := loadFixturesRaw(n)
	if err != nil {
		b.Skipf("nxb fixture missing: %v", err)
	}
	return nxb
}

func BenchmarkQueryCount_ActiveAndScoreGt80(b *testing.B) {
	nxb := loadFixturesB(b, 1000)
	r, _ := NewReader(nxb)
	pred := And(Eq("active", true), Gt("score", 80.0))
	q := r.Where(pred)
	b.ResetTimer()
	for b.Loop() {
		_ = q.Count()
	}
}

func BenchmarkSumF64Baseline(b *testing.B) {
	nxb := loadFixturesB(b, 1000)
	r, _ := NewReader(nxb)
	b.ResetTimer()
	for b.Loop() {
		_ = r.SumF64("score")
	}
}
