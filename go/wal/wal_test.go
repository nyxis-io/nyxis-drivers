package wal

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"sort"
	"strings"
	"sync"
	"testing"

	nxs "github.com/nyxis-io/nyxis-drivers/go"
)

var testSchema = Schema{Fields: []Field{{Key: "id", Sigil: '='}, {Key: "body", Sigil: '"'}}}

var extendedSchema = Schema{Fields: append(append([]Field(nil), testSchema.Fields...), Field{Key: "tags", Sigil: '"'})}

func segPath(dir string, n uint64) string {
	return filepath.Join(dir, fmt.Sprintf("%016x.nxsw", n))
}

func openWAL(t *testing.T, dir string, s Schema, o Options) (*WAL, RecoveryReport) {
	t.Helper()
	w, rep, err := Open(dir, s, o)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	t.Cleanup(func() { _ = w.Close() })
	return w, rep
}

func closeWAL(t *testing.T, w *WAL) {
	t.Helper()
	if err := w.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}
}

func appendRec(w *WAL, id int64, body string) (Offset, error) {
	return w.Append(context.Background(), func(e *nxs.ObjectEncoder) {
		e.WriteI64(0, id)
		e.WriteStr(1, body)
	})
}

func mustAppend(t *testing.T, w *WAL, id int64, body string) Offset {
	t.Helper()
	off, err := appendRec(w, id, body)
	if err != nil {
		t.Fatalf("Append(%d): %v", id, err)
	}
	return off
}

// appendRange appends ids [from, to] sequentially.
func appendRange(t *testing.T, w *WAL, from, to int64) []Offset {
	t.Helper()
	offs := make([]Offset, 0, to-from+1)
	for id := from; id <= to; id++ {
		offs = append(offs, mustAppend(t, w, id, fmt.Sprintf("record-%d", id)))
	}
	return offs
}

// appendConcurrent appends ids [from, to] from several goroutines.
func appendConcurrent(t *testing.T, w *WAL, from, to int64, workers int) {
	t.Helper()
	ids := make(chan int64)
	var wg sync.WaitGroup
	errs := make(chan error, to-from+1)
	for range workers {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for id := range ids {
				if _, err := appendRec(w, id, fmt.Sprintf("record-%d", id)); err != nil {
					errs <- err
				}
			}
		}()
	}
	for id := from; id <= to; id++ {
		ids <- id
	}
	close(ids)
	wg.Wait()
	close(errs)
	for err := range errs {
		t.Fatalf("concurrent Append: %v", err)
	}
}

type scanned struct {
	off    Offset
	id     int64
	body   string
	tags   string
	hasTag bool
	schema Schema
}

func drain(s *Scanner) []scanned {
	var out []scanned
	for s.Next() {
		r := s.Record()
		id, _ := r.GetI64("id")
		body, _ := r.GetStr("body")
		tags, hasTag := r.GetStr("tags")
		out = append(out, scanned{off: s.Offset(), id: id, body: body, tags: tags, hasTag: hasTag, schema: s.Schema()})
	}
	return out
}

func scanDir(t *testing.T, dir string, after Offset) ([]scanned, error) {
	t.Helper()
	s, err := OpenScanner(dir, after)
	if err != nil {
		return nil, err
	}
	defer s.Close()
	recs := drain(s)
	return recs, s.Err()
}

func mustScan(t *testing.T, dir string, after Offset) []scanned {
	t.Helper()
	recs, err := scanDir(t, dir, after)
	if err != nil {
		t.Fatalf("scan: %v", err)
	}
	return recs
}

func ids(recs []scanned) []int64 {
	out := make([]int64, len(recs))
	for i, r := range recs {
		out[i] = r.id
	}
	return out
}

func idRange(from, to int64) []int64 {
	var out []int64
	for id := from; id <= to; id++ {
		out = append(out, id)
	}
	return out
}

func assertIDs(t *testing.T, recs []scanned, want []int64) {
	t.Helper()
	got := ids(recs)
	if len(got) == 0 && len(want) == 0 {
		return
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("scanned ids = %v; want %v", got, want)
	}
}

func fileSize(t *testing.T, path string) int64 {
	t.Helper()
	st, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	return st.Size()
}

func headerFlags(t *testing.T, path string) uint16 {
	t.Helper()
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return binary.LittleEndian.Uint16(b[6:8])
}

func setHeaderFlags(t *testing.T, path string, flags uint16) {
	t.Helper()
	f, err := os.OpenFile(path, os.O_RDWR, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	var b [2]byte
	binary.LittleEndian.PutUint16(b[:], flags)
	if _, err := f.WriteAt(b[:], 6); err != nil {
		t.Fatal(err)
	}
}

func flipByte(t *testing.T, path string, off int64) {
	t.Helper()
	f, err := os.OpenFile(path, os.O_RDWR, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	var b [1]byte
	if _, err := f.ReadAt(b[:], off); err != nil {
		t.Fatal(err)
	}
	b[0] ^= 0xff
	if _, err := f.WriteAt(b[:], off); err != nil {
		t.Fatal(err)
	}
}

// parseHeaderSchema decodes the schema embedded in a segment header
// independently of the package code.
func parseHeaderSchema(t *testing.T, b []byte) Schema {
	t.Helper()
	n := int(binary.LittleEndian.Uint32(b[8:12]))
	sb := b[12 : 12+n]
	count := int(binary.LittleEndian.Uint16(sb[0:2]))
	sigils := sb[2 : 2+count]
	p := 2 + count
	var s Schema
	for i := 0; i < count; i++ {
		end := bytes.IndexByte(sb[p:], 0)
		s.Fields = append(s.Fields, Field{Key: string(sb[p : p+end]), Sigil: sigils[i]})
		p += end + 1
	}
	return s
}

// dirSnapshot returns segment file contents keyed by name (LOCK excluded).
func dirSnapshot(t *testing.T, dir string) map[string][]byte {
	t.Helper()
	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	out := map[string][]byte{}
	for _, e := range entries {
		if e.Name() == "LOCK" {
			continue
		}
		b, err := os.ReadFile(filepath.Join(dir, e.Name()))
		if err != nil {
			t.Fatal(err)
		}
		out[e.Name()] = b
	}
	return out
}

func manual() Options { return Options{Sync: SyncManual} }

// ── Open, header, lock, schema ───────────────────────────────────────────────

func TestOpen_WritesHeaderWithSchemaAndVersion(t *testing.T) {
	dir := t.TempDir()
	w, rep := openWAL(t, dir, testSchema, Options{})
	closeWAL(t, w)
	if rep.Segments != 1 {
		t.Errorf("Segments = %d; want 1", rep.Segments)
	}
	b, err := os.ReadFile(segPath(dir, 1))
	if err != nil {
		t.Fatal(err)
	}
	if got := binary.LittleEndian.Uint32(b[0:4]); got != 0x5753584E {
		t.Errorf("magic = %#x; want NXSW", got)
	}
	if got := binary.LittleEndian.Uint16(b[4:6]); got != 0x0200 {
		t.Errorf("version = %#x; want 0x0200", got)
	}
	if got := binary.LittleEndian.Uint16(b[6:8]); got != 0x0003 {
		t.Errorf("flags = %#x; want SCHEMA_EMBEDDED|CRC_FRAMED", got)
	}
	if len(b)%8 != 0 {
		t.Errorf("header length %d not 8-byte aligned", len(b))
	}
	if got := parseHeaderSchema(t, b); !reflect.DeepEqual(got, testSchema) {
		t.Errorf("header schema = %+v; want %+v", got, testSchema)
	}
}

func TestOpen_SecondOpenerGetsErrLocked(t *testing.T) {
	dir := t.TempDir()
	w, _ := openWAL(t, dir, testSchema, Options{})

	if _, _, err := Open(dir, testSchema, Options{}); !errors.Is(err, ErrLocked) {
		t.Fatalf("second Open in process: err = %v; want ErrLocked", err)
	}

	cmd := exec.Command(os.Args[0], "-test.run=^TestLockHelper$")
	cmd.Env = append(os.Environ(), "WAL_LOCK_HELPER_DIR="+dir)
	out, _ := cmd.CombinedOutput()
	if !strings.Contains(string(out), "RESULT=ErrLocked") {
		t.Fatalf("child Open did not report ErrLocked:\n%s", out)
	}

	closeWAL(t, w)
	w2, _ := openWAL(t, dir, testSchema, Options{})
	closeWAL(t, w2)
}

func TestLockHelper(t *testing.T) {
	dir := os.Getenv("WAL_LOCK_HELPER_DIR")
	if dir == "" {
		t.Skip("helper for TestOpen_SecondOpenerGetsErrLocked")
	}
	w, _, err := Open(dir, testSchema, Options{})
	if errors.Is(err, ErrLocked) {
		fmt.Println("RESULT=ErrLocked")
		return
	}
	if err == nil {
		_ = w.Close()
	}
	fmt.Printf("RESULT=%v\n", err)
}

func TestSchema_IsAdditiveOf(t *testing.T) {
	cases := []struct {
		name string
		s    Schema
		want bool
	}{
		{"equal", testSchema, true},
		{"one trailing field", extendedSchema, true},
		{"renamed", Schema{Fields: []Field{{"id", '='}, {"text", '"'}}}, false},
		{"retyped", Schema{Fields: []Field{{"id", '"'}, {"body", '"'}}}, false},
		{"removed", Schema{Fields: []Field{{"id", '='}}}, false},
		{"inserted in middle", Schema{Fields: []Field{{"id", '='}, {"x", '='}, {"body", '"'}}}, false},
	}
	for _, tc := range cases {
		if got := tc.s.IsAdditiveOf(testSchema); got != tc.want {
			t.Errorf("%s: IsAdditiveOf = %v; want %v", tc.name, got, tc.want)
		}
	}
}

func TestOpen_AdditiveSchemaRotatesSegment(t *testing.T) {
	dir := t.TempDir()
	w, _ := openWAL(t, dir, testSchema, manual())
	appendRange(t, w, 1, 3)
	closeWAL(t, w)

	w2, rep := openWAL(t, dir, extendedSchema, manual())
	closeWAL(t, w2)
	if !rep.SchemaRotated {
		t.Fatal("SchemaRotated = false; want true")
	}
	if headerFlags(t, segPath(dir, 1))&0x0004 == 0 {
		t.Error("segment 1 not sealed after additive schema change")
	}
	b, err := os.ReadFile(segPath(dir, 2))
	if err != nil {
		t.Fatalf("segment 2 missing: %v", err)
	}
	if got := parseHeaderSchema(t, b); !reflect.DeepEqual(got, extendedSchema) {
		t.Errorf("segment 2 schema = %+v; want %+v", got, extendedSchema)
	}
}

func TestOpen_NonAdditiveSchemaErrors(t *testing.T) {
	cases := map[string]Schema{
		"renamed field":      {Fields: []Field{{"id", '='}, {"text", '"'}}},
		"retyped field":      {Fields: []Field{{"id", '"'}, {"body", '"'}}},
		"removed field":      {Fields: []Field{{"id", '='}}},
		"inserted in middle": {Fields: []Field{{"id", '='}, {"x", '='}, {"body", '"'}}},
	}
	for name, bad := range cases {
		t.Run(name, func(t *testing.T) {
			dir := t.TempDir()
			w, _ := openWAL(t, dir, testSchema, manual())
			appendRange(t, w, 1, 3)
			closeWAL(t, w)
			if err := os.WriteFile(filepath.Join(dir, "0000000000000002.nxsw.tmp"), []byte("x"), 0o644); err != nil {
				t.Fatal(err)
			}
			before := dirSnapshot(t, dir)

			if _, _, err := Open(dir, bad, manual()); !errors.Is(err, ErrSchemaMismatch) {
				t.Fatalf("Open err = %v; want ErrSchemaMismatch", err)
			}
			if after := dirSnapshot(t, dir); !reflect.DeepEqual(after, before) {
				t.Fatal("files changed after ErrSchemaMismatch")
			}
			w2, _ := openWAL(t, dir, testSchema, manual())
			closeWAL(t, w2)
		})
	}
}

func TestScanner_CrossesSchemaBoundary(t *testing.T) {
	dir := t.TempDir()
	w, _ := openWAL(t, dir, testSchema, manual())
	appendRange(t, w, 1, 2)
	closeWAL(t, w)

	w2, _ := openWAL(t, dir, extendedSchema, manual())
	for id := int64(3); id <= 4; id++ {
		if _, err := w2.Append(context.Background(), func(e *nxs.ObjectEncoder) {
			e.WriteI64(0, id)
			e.WriteStr(1, "new")
			e.WriteStr(2, "tag")
		}); err != nil {
			t.Fatal(err)
		}
	}
	closeWAL(t, w2)

	recs := mustScan(t, dir, Offset{})
	assertIDs(t, recs, []int64{1, 2, 3, 4})
	for _, r := range recs[:2] {
		if !reflect.DeepEqual(r.schema, testSchema) {
			t.Errorf("id %d schema = %+v; want old schema", r.id, r.schema)
		}
		if r.hasTag {
			t.Errorf("id %d: old record reports tags present", r.id)
		}
	}
	for _, r := range recs[2:] {
		if !reflect.DeepEqual(r.schema, extendedSchema) {
			t.Errorf("id %d schema = %+v; want extended schema", r.id, r.schema)
		}
		if !r.hasTag || r.tags != "tag" {
			t.Errorf("id %d tags = %q, %v; want tag", r.id, r.tags, r.hasTag)
		}
	}
}

// ── Append ───────────────────────────────────────────────────────────────────

func TestAppend_OffsetsMonotonic(t *testing.T) {
	dir := t.TempDir()
	w, _ := openWAL(t, dir, testSchema, manual())
	offs := appendRange(t, w, 1, 1000)
	if err := w.Sync(); err != nil {
		t.Fatal(err)
	}
	for i := 1; i < len(offs); i++ {
		if !offs[i-1].Less(offs[i]) {
			t.Fatalf("offset %d (%+v) not after %+v", i, offs[i], offs[i-1])
		}
	}
	if got := w.Last(); got != offs[len(offs)-1] {
		t.Errorf("Last() = %+v; want %+v", got, offs[len(offs)-1])
	}
}

func TestAppend_RecordTooLarge(t *testing.T) {
	dir := t.TempDir()
	w, _ := openWAL(t, dir, testSchema, Options{Sync: SyncManual, MaxRecordBytes: 1024})
	if _, err := appendRec(w, 1, strings.Repeat("x", 2000)); !errors.Is(err, ErrRecordTooLarge) {
		t.Fatalf("Append err = %v; want ErrRecordTooLarge", err)
	}
	mustAppend(t, w, 2, "fits")
	if err := w.Sync(); err != nil {
		t.Fatal(err)
	}
	closeWAL(t, w)
	assertIDs(t, mustScan(t, dir, Offset{}), []int64{2})
}

func TestAppend_EncodingErrorLeavesWALUsable(t *testing.T) {
	dir := t.TempDir()
	w, _ := openWAL(t, dir, testSchema, manual())
	_, err := w.Append(context.Background(), func(e *nxs.ObjectEncoder) { e.WriteStr(0, "not an int") })
	if err == nil {
		t.Fatal("Append with sigil conflict: err = nil")
	}
	mustAppend(t, w, 1, "ok")
}

func TestAppend_CanceledContext(t *testing.T) {
	dir := t.TempDir()
	w, _ := openWAL(t, dir, testSchema, manual())
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	_, err := w.Append(ctx, func(e *nxs.ObjectEncoder) { e.WriteI64(0, 1) })
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("Append err = %v; want context.Canceled", err)
	}
	closeWAL(t, w)
	assertIDs(t, mustScan(t, dir, Offset{}), nil)
}

func TestClose_ThenAppendReturnsErrClosed(t *testing.T) {
	w, _ := openWAL(t, t.TempDir(), testSchema, Options{})
	closeWAL(t, w)
	if _, err := appendRec(w, 1, "late"); !errors.Is(err, ErrClosed) {
		t.Fatalf("Append after Close: err = %v; want ErrClosed", err)
	}
	if err := w.Close(); err != nil {
		t.Fatalf("second Close = %v; want nil", err)
	}
}

// failNext returns a fault that fails the first matching op (and every later
// one when sticky is true).
func failNext(match func(op fsOp) bool, allow func(op fsOp) int) func(op fsOp) (int, error) {
	var mu sync.Mutex
	fired := false
	return func(op fsOp) (int, error) {
		mu.Lock()
		defer mu.Unlock()
		if fired || !match(op) {
			return 0, nil
		}
		fired = true
		a := 0
		if allow != nil {
			a = allow(op)
		}
		return a, errInjected
	}
}

// isFrameWrite matches writes past the fixed header fields (the SEALED flag
// write lands at offset 6, a new header at 0).
func isFrameWrite(op fsOp) bool { return op.kind == opWrite && isSegmentName(op.name) && op.off >= 8 }
func isLiveDataSync(op fsOp) bool {
	return op.kind == opDataSync && isSegmentName(op.name)
}

func appendBatch(w *WAL, ids []int64) []error {
	errs := make([]error, len(ids))
	var wg sync.WaitGroup
	for i, id := range ids {
		wg.Add(1)
		go func() {
			defer wg.Done()
			_, errs[i] = appendRec(w, id, "batch")
		}()
	}
	wg.Wait()
	return errs
}

func assertPoisoned(t *testing.T, w *WAL) {
	t.Helper()
	if _, err := appendRec(w, 10_000, "after failure"); !errors.Is(err, ErrWALFailed) {
		t.Errorf("Append after failure: err = %v; want ErrWALFailed", err)
	}
	if err := w.Sync(); !errors.Is(err, ErrWALFailed) {
		t.Errorf("Sync after failure: err = %v; want ErrWALFailed", err)
	}
	if _, err := w.Rotate(); !errors.Is(err, ErrWALFailed) {
		t.Errorf("Rotate after failure: err = %v; want ErrWALFailed", err)
	}
}

func TestAppend_ShortWritePoisonsWAL(t *testing.T) {
	dir := t.TempDir()
	ffs := newFaultFS()
	w, _ := openWAL(t, dir, testSchema, Options{fs: ffs})
	appendRange(t, w, 1, 5)

	ffs.setFault(failNext(isFrameWrite, func(op fsOp) int { return op.n / 2 }))
	for i, err := range appendBatch(w, []int64{100, 101, 102, 103}) {
		if err == nil {
			t.Errorf("batch append %d: err = nil after injected short write", i)
		}
	}
	assertPoisoned(t, w)
}

func TestAppend_FsyncErrorFailsBatchAndPoisons(t *testing.T) {
	dir := t.TempDir()
	ffs := newFaultFS()
	w, _ := openWAL(t, dir, testSchema, Options{fs: ffs})
	appendRange(t, w, 1, 5)

	ffs.setFault(failNext(isLiveDataSync, nil))
	for i, err := range appendBatch(w, []int64{100, 101, 102, 103}) {
		if err == nil {
			t.Errorf("batch append %d: err = nil after injected fsync error", i)
		}
	}
	assertPoisoned(t, w)
	ffs.setFault(nil)
	closeWAL(t, w)
	assertIDs(t, mustScan(t, dir, Offset{}), idRange(1, 5))
}

// ── Recovery ─────────────────────────────────────────────────────────────────

func TestRecover_FramesAckedBeforeFailureSurviveReopen(t *testing.T) {
	cases := map[string]func(op fsOp) (int, error){
		"short write": failNext(isFrameWrite, func(op fsOp) int { return op.n - 3 }),
		"fsync error": failNext(isLiveDataSync, nil),
	}
	for name, fault := range cases {
		t.Run(name, func(t *testing.T) {
			dir := t.TempDir()
			ffs := newFaultFS()
			w, _, err := Open(dir, testSchema, Options{fs: ffs})
			if err != nil {
				t.Fatal(err)
			}
			appendConcurrent(t, w, 1, 100, 10)

			ffs.setFault(fault)
			if _, err := appendRec(w, 999, "never acked"); err == nil {
				t.Fatal("Append err = nil after injected failure")
			}
			ffs.setFault(nil)
			closeWAL(t, w)

			w2, rep := openWAL(t, dir, testSchema, Options{})
			closeWAL(t, w2)
			if rep.Records != 100 {
				t.Errorf("Records = %d; want 100", rep.Records)
			}
			recs := mustScan(t, dir, Offset{})
			got := ids(recs)
			sort.Slice(got, func(i, j int) bool { return got[i] < got[j] })
			if !reflect.DeepEqual(got, idRange(1, 100)) {
				t.Fatalf("recovered ids = %v; want 1..100", got)
			}
		})
	}
}

func TestReopenAfterFsyncError_DropsFullyWrittenUnackedFrames(t *testing.T) {
	dir := t.TempDir()
	ffs := newFaultFS()
	w, _, err := Open(dir, testSchema, Options{fs: ffs})
	if err != nil {
		t.Fatal(err)
	}
	appendRange(t, w, 1, 100)
	durableEnd := fileSize(t, segPath(dir, 1))

	ffs.setFault(failNext(isLiveDataSync, nil))
	if _, err := appendRec(w, 999, "fully written, never synced"); err == nil {
		t.Fatal("Append err = nil after injected fsync error")
	}
	if fileSize(t, segPath(dir, 1)) <= durableEnd {
		t.Fatal("failed batch frame was not written; test does not exercise the case")
	}
	ffs.setFault(nil)
	closeWAL(t, w)
	if got := fileSize(t, segPath(dir, 1)); got != durableEnd {
		t.Fatalf("size after failed-state Close = %d; want durableEnd %d", got, durableEnd)
	}

	w2, rep := openWAL(t, dir, testSchema, manual())
	if rep.Records != 100 || rep.TruncatedBytes != 0 {
		t.Errorf("Records = %d, TruncatedBytes = %d; want 100, 0", rep.Records, rep.TruncatedBytes)
	}
	appendRange(t, w2, 101, 105)
	closeWAL(t, w2)
	assertIDs(t, mustScan(t, dir, Offset{}), idRange(1, 105))
}

func TestClose_TruncateFailureKeepsWALFailed(t *testing.T) {
	dir := t.TempDir()
	ffs := newFaultFS()
	w, _, err := Open(dir, testSchema, Options{fs: ffs})
	if err != nil {
		t.Fatal(err)
	}
	appendRange(t, w, 1, 3)
	ffs.setFault(failNext(isLiveDataSync, nil))
	if _, err := appendRec(w, 999, "x"); err == nil {
		t.Fatal("Append err = nil after injected fsync error")
	}

	ffs.setFault(failNext(func(op fsOp) bool { return op.kind == opTruncate }, nil))
	if err := w.Close(); err == nil {
		t.Fatal("Close err = nil with failing truncate")
	}
	if _, err := appendRec(w, 4, "x"); !errors.Is(err, ErrWALFailed) {
		t.Fatalf("Append after failed Close: err = %v; want ErrWALFailed", err)
	}

	ffs.setFault(failNext(func(op fsOp) bool { return op.kind == opSync || op.kind == opDataSync }, nil))
	if err := w.Close(); err == nil {
		t.Fatal("Close err = nil with failing sync after truncate")
	}
	if _, err := appendRec(w, 4, "x"); !errors.Is(err, ErrWALFailed) {
		t.Fatalf("Append after failed Close: err = %v; want ErrWALFailed", err)
	}

	ffs.setFault(nil)
	if err := w.Close(); err != nil {
		t.Fatalf("Close with healed seam: %v", err)
	}
	if err := w.Close(); err != nil {
		t.Fatalf("Close after success: %v; want nil", err)
	}
	assertIDs(t, mustScan(t, dir, Offset{}), idRange(1, 3))
}

func TestScanner_ReadsAllInOrder(t *testing.T) {
	dir := t.TempDir()
	w, _ := openWAL(t, dir, testSchema, manual())
	offs := appendRange(t, w, 1, 50)
	closeWAL(t, w)
	recs := mustScan(t, dir, Offset{})
	assertIDs(t, recs, idRange(1, 50))
	for i, r := range recs {
		if r.off != offs[i] {
			t.Fatalf("record %d offset = %+v; want %+v", i, r.off, offs[i])
		}
		if want := fmt.Sprintf("record-%d", r.id); r.body != want {
			t.Fatalf("record %d body = %q; want %q", i, r.body, want)
		}
	}
}

func TestScanner_AfterIsExclusive(t *testing.T) {
	dir := t.TempDir()
	w, _ := openWAL(t, dir, testSchema, manual())
	offs := appendRange(t, w, 1, 50)
	closeWAL(t, w)
	recs := mustScan(t, dir, offs[19])
	assertIDs(t, recs, idRange(21, 50))
	if recs[0].off != offs[20] {
		t.Fatalf("first offset = %+v; want %+v", recs[0].off, offs[20])
	}
	assertIDs(t, mustScan(t, dir, offs[49]), nil)
}

func TestRecover_TruncatesTornTail(t *testing.T) {
	dir := t.TempDir()
	w, _ := openWAL(t, dir, testSchema, manual())
	appendRange(t, w, 1, 10)
	closeWAL(t, w)
	end := fileSize(t, segPath(dir, 1))

	f, err := os.OpenFile(segPath(dir, 1), os.O_WRONLY|os.O_APPEND, 0)
	if err != nil {
		t.Fatal(err)
	}
	torn := make([]byte, 8+20)
	binary.LittleEndian.PutUint32(torn[0:4], 64)
	if _, err := f.Write(torn); err != nil {
		t.Fatal(err)
	}
	f.Close()

	w2, rep := openWAL(t, dir, testSchema, manual())
	closeWAL(t, w2)
	if rep.Records != 10 {
		t.Errorf("Records = %d; want 10", rep.Records)
	}
	if rep.TruncatedBytes != int64(len(torn)) {
		t.Errorf("TruncatedBytes = %d; want %d", rep.TruncatedBytes, len(torn))
	}
	if got := fileSize(t, segPath(dir, 1)); got != end {
		t.Errorf("size = %d; want %d (end of frame 10)", got, end)
	}
}

func TestRecover_CRCMismatchInLiveTailTruncates(t *testing.T) {
	dir := t.TempDir()
	w, _ := openWAL(t, dir, testSchema, manual())
	offs := appendRange(t, w, 1, 10)
	closeWAL(t, w)
	flipByte(t, segPath(dir, 1), int64(offs[9].Pos)+8+12)

	w2, rep := openWAL(t, dir, testSchema, manual())
	closeWAL(t, w2)
	if rep.Records != 9 {
		t.Errorf("Records = %d; want 9", rep.Records)
	}
	if got := fileSize(t, segPath(dir, 1)); got != int64(offs[9].Pos) {
		t.Errorf("size = %d; want %d", got, offs[9].Pos)
	}
	assertIDs(t, mustScan(t, dir, Offset{}), idRange(1, 9))
}

func TestSealedSegment_CorruptionIsErrorNotTruncation(t *testing.T) {
	dir := t.TempDir()
	w, _ := openWAL(t, dir, testSchema, manual())
	offs := appendRange(t, w, 1, 5)
	if _, err := w.Rotate(); err != nil {
		t.Fatal(err)
	}
	appendRange(t, w, 6, 10)
	closeWAL(t, w)
	flipByte(t, segPath(dir, 1), int64(offs[2].Pos)+8+12)
	before := dirSnapshot(t, dir)

	w2, _, err := Open(dir, testSchema, manual())
	if err == nil {
		closeWAL(t, w2)
	} else if !errors.Is(err, ErrCorruptSegment) {
		t.Fatalf("Open err = %v; want nil or ErrCorruptSegment", err)
	}
	recs, err := scanDir(t, dir, Offset{})
	if !errors.Is(err, ErrCorruptSegment) {
		t.Fatalf("scan err = %v; want ErrCorruptSegment", err)
	}
	assertIDs(t, recs, idRange(1, 2))
	if after := dirSnapshot(t, dir); !reflect.DeepEqual(after[filepath.Base(segPath(dir, 1))], before[filepath.Base(segPath(dir, 1))]) {
		t.Fatal("sealed segment bytes changed")
	}
}

// ── Rotation ─────────────────────────────────────────────────────────────────

// assertSubsequence checks that want appears in ops in order (other ops may
// be interleaved).
func assertSubsequence(t *testing.T, ops []fsOp, want []fsOp) {
	t.Helper()
	i := 0
	for _, op := range ops {
		if i < len(want) && op.kind == want[i].kind && op.name == want[i].name {
			i++
		}
	}
	if i != len(want) {
		var got []string
		for _, op := range ops {
			got = append(got, op.String())
		}
		t.Fatalf("op order missing %v (step %d)\nops:\n  %s", want[i], i, strings.Join(got, "\n  "))
	}
}

func TestRotate_SealsAndContinues(t *testing.T) {
	dir := t.TempDir()
	ffs := newFaultFS()
	w, _ := openWAL(t, dir, testSchema, Options{fs: ffs})
	offs := appendRange(t, w, 1, 3)

	ffs.resetOps()
	sealed, err := w.Rotate()
	if err != nil {
		t.Fatal(err)
	}
	if sealed != offs[2] {
		t.Errorf("Rotate() = %+v; want last record %+v", sealed, offs[2])
	}
	seg1, seg2 := filepath.Base(segPath(dir, 1)), filepath.Base(segPath(dir, 2))
	assertSubsequence(t, ffs.snapshot(), []fsOp{
		{kind: opDataSync, name: seg1},
		{kind: opWrite, name: seg1},
		{kind: opSync, name: seg1},
		{kind: opWrite, name: seg2 + ".tmp"},
		{kind: opSync, name: seg2 + ".tmp"},
		{kind: opRename, name: seg2 + ".tmp->" + seg2},
		{kind: opSyncDir, name: filepath.Base(dir)},
	})
	if headerFlags(t, segPath(dir, 1))&0x0004 == 0 {
		t.Error("segment 1 SEALED flag not set")
	}
	off := mustAppend(t, w, 4, "after rotate")
	if off.Segment != 2 {
		t.Errorf("post-rotate offset segment = %d; want 2", off.Segment)
	}
	closeWAL(t, w)
	assertIDs(t, mustScan(t, dir, Offset{}), idRange(1, 4))
}

func TestRotate_BySegmentSize(t *testing.T) {
	dir := t.TempDir()
	w, _ := openWAL(t, dir, testSchema, Options{Sync: SyncManual, SegmentBytes: 1024})
	appendRange(t, w, 1, 100)
	closeWAL(t, w)
	if _, err := os.Stat(segPath(dir, 3)); err != nil {
		t.Fatalf("expected at least 3 segments with SegmentBytes=1024: %v", err)
	}
	assertIDs(t, mustScan(t, dir, Offset{}), idRange(1, 100))
}

func TestRecover_CrashAfterSealBeforeNextSegment(t *testing.T) {
	dir := t.TempDir()
	w, _ := openWAL(t, dir, testSchema, manual())
	offs := appendRange(t, w, 1, 3)
	closeWAL(t, w)
	setHeaderFlags(t, segPath(dir, 1), 0x0007)

	w2, rep := openWAL(t, dir, testSchema, manual())
	if !rep.CreatedLive {
		t.Error("CreatedLive = false; want true")
	}
	if w2.Last() != offs[2] {
		t.Errorf("Last() = %+v; want %+v", w2.Last(), offs[2])
	}
	off := mustAppend(t, w2, 4, "live again")
	if off.Segment != 2 {
		t.Errorf("append went to segment %d; want 2", off.Segment)
	}
	closeWAL(t, w2)
	assertIDs(t, mustScan(t, dir, Offset{}), idRange(1, 4))
}

func TestRecover_CrashDuringNextSegmentCreate(t *testing.T) {
	dir := t.TempDir()
	w, _ := openWAL(t, dir, testSchema, manual())
	appendRange(t, w, 1, 3)
	closeWAL(t, w)
	setHeaderFlags(t, segPath(dir, 1), 0x0007)
	empty := segPath(dir, 2) + ".tmp"
	partial := segPath(dir, 3) + ".tmp"
	if err := os.WriteFile(empty, nil, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(partial, []byte{0x4E, 0x58, 0x53, 0x57, 0x00}, 0o644); err != nil {
		t.Fatal(err)
	}

	w2, rep := openWAL(t, dir, testSchema, manual())
	closeWAL(t, w2)
	for _, p := range []string{empty, partial} {
		if _, err := os.Stat(p); !os.IsNotExist(err) {
			t.Errorf("%s still present after Open", filepath.Base(p))
		}
	}
	if !rep.CreatedLive {
		t.Error("CreatedLive = false; want true")
	}
	if _, err := os.Stat(segPath(dir, 2)); err != nil {
		t.Errorf("live segment 2 not created: %v", err)
	}
	assertIDs(t, mustScan(t, dir, Offset{}), idRange(1, 3))
}

func TestRecover_CrashBeforeSealFlag(t *testing.T) {
	build := func(t *testing.T) (string, []Offset) {
		dir := t.TempDir()
		w, _ := openWAL(t, dir, testSchema, manual())
		offs := appendRange(t, w, 1, 3)
		if _, err := w.Rotate(); err != nil {
			t.Fatal(err)
		}
		appendRange(t, w, 4, 5)
		closeWAL(t, w)
		setHeaderFlags(t, segPath(dir, 1), 0x0003)
		return dir, offs
	}

	t.Run("valid segment is sealed", func(t *testing.T) {
		dir, _ := build(t)
		w, rep := openWAL(t, dir, testSchema, manual())
		closeWAL(t, w)
		if !reflect.DeepEqual(rep.SealedOnOpen, []uint64{1}) {
			t.Errorf("SealedOnOpen = %v; want [1]", rep.SealedOnOpen)
		}
		if headerFlags(t, segPath(dir, 1))&0x0004 == 0 {
			t.Error("segment 1 not sealed on Open")
		}
		assertIDs(t, mustScan(t, dir, Offset{}), idRange(1, 5))
	})

	t.Run("bad frame is corruption", func(t *testing.T) {
		dir, offs := build(t)
		flipByte(t, segPath(dir, 1), int64(offs[1].Pos)+8+12)
		before := dirSnapshot(t, dir)
		if _, _, err := Open(dir, testSchema, manual()); !errors.Is(err, ErrCorruptSegment) {
			t.Fatalf("Open err = %v; want ErrCorruptSegment", err)
		}
		if after := dirSnapshot(t, dir); !reflect.DeepEqual(after, before) {
			t.Fatal("files changed after ErrCorruptSegment")
		}
	})
}

// ── Live scanning and durability ordering ────────────────────────────────────

func TestLiveScanner_StopsAtDurableEnd(t *testing.T) {
	dir := t.TempDir()
	w, _ := openWAL(t, dir, testSchema, manual())
	s, err := w.NewLiveScanner(Offset{})
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()

	mustAppend(t, w, 1, "unsynced")
	if s.Next() {
		t.Fatal("Next() = true before Sync")
	}
	if err := w.Sync(); err != nil {
		t.Fatal(err)
	}
	if !s.Next() {
		t.Fatalf("Next() = false after Sync (err %v)", s.Err())
	}
	if id, _ := s.Record().GetI64("id"); id != 1 {
		t.Fatalf("id = %d; want 1", id)
	}
	if s.Next() {
		t.Fatal("Next() = true past durable end")
	}
	mustAppend(t, w, 2, "second")
	if err := w.Sync(); err != nil {
		t.Fatal(err)
	}
	if !s.Next() {
		t.Fatalf("Next() = false after more durable appends (err %v)", s.Err())
	}
	if id, _ := s.Record().GetI64("id"); id != 2 {
		t.Fatalf("id = %d; want 2", id)
	}
	if _, err := w.Rotate(); err != nil {
		t.Fatal(err)
	}
	mustAppend(t, w, 3, "next segment")
	if err := w.Sync(); err != nil {
		t.Fatal(err)
	}
	if !s.Next() {
		t.Fatalf("Next() = false across rotation (err %v)", s.Err())
	}
	if id, _ := s.Record().GetI64("id"); id != 3 {
		t.Fatalf("id = %d; want 3", id)
	}
	if err := s.Err(); err != nil {
		t.Fatal(err)
	}
}

func TestSyncGroup_FsyncBeforeAck(t *testing.T) {
	dir := t.TempDir()
	ffs := newFaultFS()
	w, _ := openWAL(t, dir, testSchema, Options{fs: ffs})

	var wg sync.WaitGroup
	for g := range 8 {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := range 20 {
				off, err := appendRec(w, int64(g*100+i), "group")
				if err != nil {
					t.Error(err)
					return
				}
				ffs.note(fmt.Sprintf("ack %d %d", off.Segment, off.Pos))
			}
		}()
	}
	wg.Wait()
	closeWAL(t, w)

	recs := mustScan(t, dir, Offset{})
	frameEnd := map[Offset]int64{}
	size := fileSize(t, segPath(dir, 1))
	for i, r := range recs {
		end := size
		if i+1 < len(recs) {
			end = int64(recs[i+1].off.Pos)
		}
		frameEnd[r.off] = end
	}

	seg := filepath.Base(segPath(dir, 1))
	var synced int64
	acks := 0
	for _, op := range ffs.snapshot() {
		switch op.kind {
		case opDataSync, opSync:
			if op.name == seg && op.extent > synced {
				synced = op.extent
			}
		case opNote:
			var off Offset
			if _, err := fmt.Sscanf(op.note, "ack %d %d", &off.Segment, &off.Pos); err != nil {
				t.Fatal(err)
			}
			acks++
			end, ok := frameEnd[off]
			if !ok {
				t.Fatalf("acked offset %+v not found by scan", off)
			}
			if end > synced {
				t.Fatalf("offset %+v acked before a sync covered it (frame end %d, synced %d)", off, end, synced)
			}
		}
	}
	if acks != 160 {
		t.Fatalf("acks = %d; want 160", acks)
	}
}
