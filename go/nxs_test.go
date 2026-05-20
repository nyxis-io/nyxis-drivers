package nxs

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"math"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const fixtureDir = "../js/fixtures"

type record struct {
	ID       int64   `json:"id"`
	Username string  `json:"username"`
	Email    string  `json:"email"`
	Age      int64   `json:"age"`
	Balance  float64 `json:"balance"`
	Active   bool    `json:"active"`
	Score    float64 `json:"score"`
}

func loadFixturesRaw(n int) ([]byte, error) {
	return os.ReadFile(filepath.Join(fixtureDir, fmtRecordsNxb(n)))
}

func loadFixtures(t *testing.T, n int) ([]byte, []record) {
	t.Helper()
	nxb, err := loadFixturesRaw(n)
	if err != nil {
		t.Skipf("nxb fixture missing: %v", err)
	}
	raw, err := os.ReadFile(filepath.Join(fixtureDir, fmtRecordsJson(n)))
	if err != nil {
		t.Skipf("json fixture missing: %v", err)
	}
	var recs []record
	if err := json.Unmarshal(raw, &recs); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	return nxb, recs
}

func fmtRecordsNxb(n int) string  { return "records_" + itoa(n) + ".nxb" }
func fmtRecordsJson(n int) string { return "records_" + itoa(n) + ".json" }

func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	neg := n < 0
	if neg {
		n = -n
	}
	var buf [20]byte
	pos := len(buf)
	for n > 0 {
		pos--
		buf[pos] = byte('0' + n%10)
		n /= 10
	}
	if neg {
		pos--
		buf[pos] = '-'
	}
	return string(buf[pos:])
}

func TestReaderOpens(t *testing.T) {
	nxb, _ := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}
	if r.RecordCount() != 1000 {
		t.Errorf("record count = %d, want 1000", r.RecordCount())
	}
}

func TestSchemaKeys(t *testing.T) {
	nxb, _ := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"id", "username", "email", "age", "score", "active"} {
		found := false
		for _, k := range r.Keys {
			if k == want {
				found = true
				break
			}
		}
		if !found {
			t.Errorf("missing key %q (got %v)", want, r.Keys)
		}
	}
}

func TestRecordsMatchJSON(t *testing.T) {
	nxb, js := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}
	for _, i := range []int{0, 7, 42, 500, 999} {
		o := r.Record(i)
		if got, _ := o.GetI64("id"); got != js[i].ID {
			t.Errorf("record %d id=%d want %d", i, got, js[i].ID)
		}
		if got, _ := o.GetStr("username"); got != js[i].Username {
			t.Errorf("record %d username=%q want %q", i, got, js[i].Username)
		}
		if got, _ := o.GetF64("score"); !closeEnough(got, js[i].Score) {
			t.Errorf("record %d score=%v want %v", i, got, js[i].Score)
		}
		if got, _ := o.GetBool("active"); got != js[i].Active {
			t.Errorf("record %d active=%v want %v", i, got, js[i].Active)
		}
	}
}

func TestSumF64(t *testing.T) {
	nxb, js := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}
	var want float64
	for _, x := range js {
		want += x.Score
	}
	if got := r.SumF64("score"); !closeEnough(got, want) {
		t.Errorf("sum = %v, want %v", got, want)
	}
}

func TestSumI64(t *testing.T) {
	nxb, js := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}
	var want int64
	for _, x := range js {
		want += x.Age
	}
	if got := r.SumI64("age"); got != want {
		t.Errorf("sum = %v, want %v", got, want)
	}
}

func TestMinMaxF64(t *testing.T) {
	nxb, js := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}
	wantMin, wantMax := math.Inf(1), math.Inf(-1)
	for _, x := range js {
		if x.Score < wantMin {
			wantMin = x.Score
		}
		if x.Score > wantMax {
			wantMax = x.Score
		}
	}
	if m, ok := r.MinF64("score"); !ok || !closeEnough(m, wantMin) {
		t.Errorf("min = %v, want %v", m, wantMin)
	}
	if m, ok := r.MaxF64("score"); !ok || !closeEnough(m, wantMax) {
		t.Errorf("max = %v, want %v", m, wantMax)
	}
}

func closeEnough(a, b float64) bool {
	return math.Abs(a-b) < 1e-6
}

func TestBadMagic(t *testing.T) {
	nxb, _ := loadFixtures(t, 1000)
	bad := append([]byte{}, nxb...)
	bad[0] = 0x00
	_, err := NewReader(bad)
	if err == nil || !strings.Contains(err.Error(), "ERR_BAD_MAGIC") {
		t.Errorf("expected ERR_BAD_MAGIC, got %v", err)
	}
}

func TestTruncatedFile(t *testing.T) {
	_, err := NewReader([]byte{0x4E, 0x58, 0x53, 0x42, 0x00, 0x01})
	if err == nil {
		t.Error("expected error on truncated file")
	}
}

func TestDictHashMismatch(t *testing.T) {
	nxb, _ := loadFixtures(t, 1000)
	bad := append([]byte{}, nxb...)
	bad[8] ^= 0xFF
	_, err := NewReader(bad)
	if err == nil || !strings.Contains(err.Error(), "ERR_DICT_MISMATCH") {
		t.Errorf("expected ERR_DICT_MISMATCH, got %v", err)
	}
}

// ── Writer tests ──────────────────────────────────────────────────────────────

func TestWriterRoundTrip(t *testing.T) {
	type rec struct {
		id       int64
		username string
		score    float64
		active   bool
	}
	recs := []rec{
		{1, "alice", 9.5, true},
		{2, "bob", 7.2, false},
		{3, "carol", 8.8, true},
	}

	schema := NewSchema([]string{"id", "username", "score", "active"})
	w := NewWriter(schema)
	for _, r := range recs {
		w.BeginObject()
		w.WriteI64(0, r.id)
		w.WriteStr(1, r.username)
		w.WriteF64(2, r.score)
		w.WriteBool(3, r.active)
		w.EndObject()
	}
	data := w.Finish()
	if tailPtr := binary.LittleEndian.Uint64(data[16:24]); tailPtr != 0 {
		t.Fatalf("preamble TailPtr = %d, want 0 for streamable v1.1", tailPtr)
	}

	rd, err := NewReader(data)
	if err != nil {
		t.Fatal(err)
	}
	if rd.RecordCount() != len(recs) {
		t.Fatalf("record count = %d, want %d", rd.RecordCount(), len(recs))
	}
	for i, want := range recs {
		obj := rd.Record(i)
		if got, ok := obj.GetI64("id"); !ok || got != want.id {
			t.Errorf("record %d id = %v (ok=%v), want %v", i, got, ok, want.id)
		}
		if got, ok := obj.GetStr("username"); !ok || got != want.username {
			t.Errorf("record %d username = %q (ok=%v), want %q", i, got, ok, want.username)
		}
		if got, ok := obj.GetF64("score"); !ok || !closeEnough(got, want.score) {
			t.Errorf("record %d score = %v (ok=%v), want %v", i, got, ok, want.score)
		}
		if got, ok := obj.GetBool("active"); !ok || got != want.active {
			t.Errorf("record %d active = %v (ok=%v), want %v", i, got, ok, want.active)
		}
	}
}

func TestStreamWriterRoundTrip(t *testing.T) {
	schema := NewSchema([]string{"id", "username", "score", "active"})
	var buf bytes.Buffer
	sw, err := NewStreamWriter(&buf, schema)
	if err != nil {
		t.Fatal(err)
	}

	sw.BeginObject()
	sw.WriteI64(0, 1)
	sw.WriteStr(1, "alice")
	sw.WriteF64(2, 9.5)
	sw.WriteBool(3, true)
	if err := sw.EndObject(); err != nil {
		t.Fatal(err)
	}
	if buf.Len() == 0 {
		t.Fatal("expected streamed bytes before Close")
	}
	reusedWriter := sw.writer

	sw.BeginObject()
	if sw.writer != reusedWriter {
		t.Fatal("stream writer did not reuse its per-record writer")
	}
	sw.WriteI64(0, 2)
	sw.WriteStr(1, "bob")
	sw.WriteF64(2, 7.25)
	sw.WriteBool(3, false)
	if err := sw.EndObject(); err != nil {
		t.Fatal(err)
	}

	if err := sw.Close(); err != nil {
		t.Fatal(err)
	}

	data := buf.Bytes()
	if tailPtr := binary.LittleEndian.Uint64(data[16:24]); tailPtr != 0 {
		t.Fatalf("stream preamble TailPtr = %d, want 0", tailPtr)
	}
	footerTailPtr := binary.LittleEndian.Uint64(data[len(data)-12 : len(data)-4])
	if footerTailPtr == 0 {
		t.Fatal("footer tail pointer is zero")
	}

	r, err := NewReader(data)
	if err != nil {
		t.Fatal(err)
	}
	if r.RecordCount() != 2 {
		t.Fatalf("record count = %d, want 2", r.RecordCount())
	}
	if got, ok := r.Record(1).GetStr("username"); !ok || got != "bob" {
		t.Fatalf("record 1 username = %q ok=%v, want bob", got, ok)
	}
}

func TestWriterFromRecords(t *testing.T) {
	keys := []string{"id", "name", "value"}
	records := []map[string]interface{}{
		{"id": int64(10), "name": "foo", "value": 1.5},
		{"id": int64(20), "name": "bar", "value": 2.5},
	}
	data := FromRecords(keys, records)
	r, err := NewReader(data)
	if err != nil {
		t.Fatal(err)
	}
	if r.RecordCount() != 2 {
		t.Fatalf("record count = %d, want 2", r.RecordCount())
	}
	name, ok := r.Record(1).GetStr("name")
	if !ok || name != "bar" {
		t.Errorf("record 1 name = %q (ok=%v), want \"bar\"", name, ok)
	}
}

func TestWriterNullField(t *testing.T) {
	schema := NewSchema([]string{"a", "b"})
	w := NewWriter(schema)
	w.BeginObject()
	w.WriteI64(0, 99)
	w.WriteNull(1)
	w.EndObject()
	r, err := NewReader(w.Finish())
	if err != nil {
		t.Fatal(err)
	}
	got, ok := r.Record(0).GetI64("a")
	if !ok || got != 99 {
		t.Errorf("a = %v (ok=%v), want 99", got, ok)
	}
}

func TestWriterBoolField(t *testing.T) {
	schema := NewSchema([]string{"flag"})
	w := NewWriter(schema)
	w.BeginObject()
	w.WriteBool(0, true)
	w.EndObject()
	w.BeginObject()
	w.WriteBool(0, false)
	w.EndObject()
	r, err := NewReader(w.Finish())
	if err != nil {
		t.Fatal(err)
	}
	if v, ok := r.Record(0).GetBool("flag"); !ok || !v {
		t.Errorf("record 0 flag = %v (ok=%v), want true", v, ok)
	}
	if v, ok := r.Record(1).GetBool("flag"); !ok || v {
		t.Errorf("record 1 flag = %v (ok=%v), want false", v, ok)
	}
}

func TestWriterUnicodeString(t *testing.T) {
	schema := NewSchema([]string{"msg"})
	w := NewWriter(schema)
	w.BeginObject()
	w.WriteStr(0, "héllo wörld")
	w.EndObject()
	r, err := NewReader(w.Finish())
	if err != nil {
		t.Fatal(err)
	}
	got, ok := r.Record(0).GetStr("msg")
	if !ok || got != "héllo wörld" {
		t.Errorf("msg = %q (ok=%v), want \"héllo wörld\"", got, ok)
	}
}

func TestWriterSchemaEvolution(t *testing.T) {
	// Write with schema ["a","b","c"]
	schema := NewSchema([]string{"a", "b", "c"})
	w := NewWriter(schema)
	w.BeginObject()
	w.WriteI64(0, 100)
	w.WriteI64(1, 200)
	w.WriteI64(2, 300)
	w.EndObject()
	data := w.Finish()

	// Full reader — all three fields present
	r, err := NewReader(data)
	if err != nil {
		t.Fatal(err)
	}
	obj := r.Record(0)
	if v, ok := obj.GetI64("a"); !ok || v != 100 {
		t.Errorf("a = %v (ok=%v), want 100", v, ok)
	}
	if v, ok := obj.GetI64("b"); !ok || v != 200 {
		t.Errorf("b = %v (ok=%v), want 200", v, ok)
	}
	if v, ok := obj.GetI64("c"); !ok || v != 300 {
		t.Errorf("c = %v (ok=%v), want 300", v, ok)
	}

	// Simulate old reader: unknown key returns (zero, false) — absent, not an error
	if _, ok := obj.GetI64("nonexistent_field"); ok {
		t.Error("expected absent (false) for unknown key, got present")
	}
}

func TestWriterManyFields(t *testing.T) {
	// Test LEB128 bitmask with more than 7 fields (requires 2+ bitmask bytes)
	keys := []string{"f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8"}
	schema := NewSchema(keys)
	w := NewWriter(schema)
	w.BeginObject()
	for i := range keys {
		w.WriteI64(i, int64(i*100))
	}
	w.EndObject()
	r, err := NewReader(w.Finish())
	if err != nil {
		t.Fatal(err)
	}
	for i, k := range keys {
		got, ok := r.Record(0).GetI64(k)
		if !ok || got != int64(i*100) {
			t.Errorf("field %s = %v (ok=%v), want %d", k, got, ok, i*100)
		}
	}
}

func TestIsUniform(t *testing.T) {
	nxb, _ := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}
	if !r.IsUniform() {
		t.Error("fixture should be uniform across all records")
	}
}

func TestSumF64FastMatchesSafe(t *testing.T) {
	nxb, js := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}
	var want float64
	for _, x := range js {
		want += x.Score
	}
	fast := r.SumF64Fast("score")
	safe := r.SumF64("score")
	if !closeEnough(fast, safe) {
		t.Errorf("fast=%v safe=%v", fast, safe)
	}
	if !closeEnough(fast, want) {
		t.Errorf("fast=%v want=%v", fast, want)
	}
}

func TestSumI64FastMatchesSafe(t *testing.T) {
	nxb, js := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}
	var want int64
	for _, x := range js {
		want += x.Age
	}
	if got := r.SumI64Fast("age"); got != want {
		t.Errorf("SumI64Fast = %d want %d", got, want)
	}
}

func TestSumF64FastParMatchesSerial(t *testing.T) {
	nxb, _ := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}
	serial := r.SumF64Fast("score")
	for _, w := range []int{1, 2, 4, 8} {
		par := r.SumF64FastPar("score", w)
		if !closeEnough(par, serial) {
			t.Errorf("workers=%d par=%v serial=%v", w, par, serial)
		}
	}
}

func TestSumI64FastParMatchesSerial(t *testing.T) {
	nxb, _ := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}
	serial := r.SumI64Fast("age")
	for _, w := range []int{1, 2, 4, 8} {
		par := r.SumI64FastPar("age", w)
		if par != serial {
			t.Errorf("workers=%d par=%v serial=%v", w, par, serial)
		}
	}
}

func TestMinMaxF64Fast(t *testing.T) {
	nxb, js := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}
	wantMin, wantMax := math.Inf(1), math.Inf(-1)
	for _, x := range js {
		if x.Score < wantMin {
			wantMin = x.Score
		}
		if x.Score > wantMax {
			wantMax = x.Score
		}
	}
	if m, ok := r.MinF64Fast("score"); !ok || !closeEnough(m, wantMin) {
		t.Errorf("MinF64Fast = %v want %v", m, wantMin)
	}
	if m, ok := r.MaxF64Fast("score"); !ok || !closeEnough(m, wantMax) {
		t.Errorf("MaxF64Fast = %v want %v", m, wantMax)
	}
}

func TestFieldIndexMatchesFast(t *testing.T) {
	nxb, _ := loadFixtures(t, 1000)
	r, err := NewReader(nxb)
	if err != nil {
		t.Fatal(err)
	}

	fast := r.SumF64Fast("score")
	idx, ok := r.BuildFieldIndex("score")
	if !ok {
		t.Fatal("BuildFieldIndex failed")
	}
	indexed := r.SumF64Indexed(idx)
	if !closeEnough(fast, indexed) {
		t.Errorf("SumF64Indexed=%v SumF64Fast=%v", indexed, fast)
	}

	mn, _ := r.MinF64Indexed(idx)
	mx, _ := r.MaxF64Indexed(idx)
	mnFast, _ := r.MinF64Fast("score")
	mxFast, _ := r.MaxF64Fast("score")
	if !closeEnough(mn, mnFast) {
		t.Errorf("min mismatch")
	}
	if !closeEnough(mx, mxFast) {
		t.Errorf("max mismatch")
	}
}
