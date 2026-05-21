package nxs

import (
	"encoding/binary"
	"math"
	"os"
	"path/filepath"
	"runtime"
	"testing"
)

func conformanceDir(t *testing.T) string {
	t.Helper()
	_, file, _, _ := runtime.Caller(0)
	// nyxis-drivers/go/col_test.go → ../../nyxis/conformance
	return filepath.Join(filepath.Dir(file), "..", "..", "nyxis", "conformance")
}

func TestColumnarDenseConformance(t *testing.T) {
	path := filepath.Join(conformanceDir(t), "columnar_flat8_dense_100.nxb")
	data, err := os.ReadFile(path)
	if err != nil {
		t.Skip("conformance vector missing; run: make -C nyxis conformance-generate")
	}
	r, err := NewReader(data)
	if err != nil {
		t.Fatal(err)
	}
	if r.LayoutKind() != LayoutColumnar {
		t.Fatalf("layout: got %v", r.LayoutKind())
	}
	if r.RecordCount() != 100 {
		t.Fatalf("records: got %d", r.RecordCount())
	}
	sum := r.ColSumF64("score")
	want := 0.0
	for i := 0; i < 100; i++ {
		want += float64(i) * 0.5
	}
	if !closeEnough(sum, want) {
		t.Fatalf("ColSumF64(score): got %v want %v", sum, want)
	}
	buf, ok := r.ColBuffer("score")
	if !ok || len(buf) != 100*8 {
		t.Fatalf("ColBuffer: ok=%v len=%d", ok, len(buf))
	}
	rec := r.Record(0)
	v, ok := rec.GetF64("score")
	if !ok || !closeEnough(v, 0) {
		t.Fatalf("record 0 score: %v %v", v, ok)
	}
}

func TestPAXDenseConformance(t *testing.T) {
	path := filepath.Join(conformanceDir(t), "pax_flat8_dense_p256_1000.nxb")
	data, err := os.ReadFile(path)
	if err != nil {
		t.Skip("conformance vector missing")
	}
	r, err := NewReader(data)
	if err != nil {
		t.Fatal(err)
	}
	if r.LayoutKind() != LayoutPAX {
		t.Fatalf("layout: got %v", r.LayoutKind())
	}
	if r.RecordCount() != 1000 {
		t.Fatalf("records: got %d", r.RecordCount())
	}
	sum := r.SumF64("score")
	var want float64
	for i := 0; i < 1000; i++ {
		want += float64(i) * 0.5
	}
	if !closeEnough(sum, want) {
		t.Fatalf("SumF64: got %v want %v", sum, want)
	}
}

// buildColumnarFlat8Dense builds a dense columnar .nxb matching conformance flat8 vectors.
func buildColumnarFlat8Dense(n int) []byte {
	keys := []string{"id", "score", "active", "ts"}
	sigils := []byte{'=', '~', '?', '@'}
	schemaBytes := buildSchemaBytesSigils(keys, sigils)
	dictHash := murmur3_64(schemaBytes)

	bmLen := nullBitmapBytes(uint32(n))
	denseBM := func() []byte {
		bm := make([]byte, bmLen)
		for i := 0; i < n; i++ {
			bm[i/8] |= 1 << (i % 8)
		}
		return bm
	}

	idCol := append(denseBM(), make([]byte, n*8)...)
	for i := 0; i < n; i++ {
		binary.LittleEndian.PutUint64(idCol[bmLen+i*8:], uint64(i))
	}
	scoreCol := append(denseBM(), make([]byte, n*8)...)
	for i := 0; i < n; i++ {
		binary.LittleEndian.PutUint64(scoreCol[bmLen+i*8:], math.Float64bits(float64(i)*0.5))
	}
	activeCol := append(denseBM(), make([]byte, n*8)...)
	for i := 0; i < n; i++ {
		if i%2 == 0 {
			activeCol[bmLen+i*8] = 1
		}
	}
	tsCol := append(denseBM(), make([]byte, n*8)...)
	for i := 0; i < n; i++ {
		binary.LittleEndian.PutUint64(tsCol[bmLen+i*8:], uint64(i)*1_000_000)
	}
	fields := [][]byte{idCol, scoreCol, activeCol, tsCol}

	var data []byte
	tailEntries := make([]struct{ off, length uint64 }, len(fields))
	dataBase := uint64(32 + len(schemaBytes))
	for fi, col := range fields {
		tailEntries[fi].off = dataBase + uint64(len(data))
		tailEntries[fi].length = uint64(len(col))
		data = append(data, col...)
	}

	tailIndexOff := dataBase + uint64(len(data))
	var tail []byte
	for fi, e := range tailEntries {
		tail = binary.LittleEndian.AppendUint16(tail, uint16(fi))
		tail = binary.LittleEndian.AppendUint16(tail, 0)
		tail = binary.LittleEndian.AppendUint64(tail, e.off)
		tail = binary.LittleEndian.AppendUint64(tail, e.length)
	}
	tail = binary.LittleEndian.AppendUint64(tail, tailIndexOff)
	tail = binary.LittleEndian.AppendUint64(tail, uint64(n))
	tail = binary.LittleEndian.AppendUint32(tail, magicFooter)

	flags := flagSchemaEmbedded | flagColumnar
	out := make([]byte, 0, int(dataBase)+len(data)+len(tail))
	out = binary.LittleEndian.AppendUint32(out, magicFile)
	out = binary.LittleEndian.AppendUint16(out, 0x0101)
	out = binary.LittleEndian.AppendUint16(out, flags)
	out = binary.LittleEndian.AppendUint64(out, dictHash)
	out = binary.LittleEndian.AppendUint64(out, tailIndexOff)
	out = append(out, make([]byte, 8)...)
	out = append(out, schemaBytes...)
	out = append(out, data...)
	out = append(out, tail...)
	return out
}

func buildSchemaBytesSigils(keys []string, sigils []byte) []byte {
	n := len(keys)
	size := 2 + n
	for _, k := range keys {
		size += len(k) + 1
	}
	if rem := size % 8; rem != 0 {
		size += 8 - rem
	}
	buf := make([]byte, size)
	p := 2
	for i := range keys {
		s := byte('"')
		if i < len(sigils) && sigils[i] != 0 {
			s = sigils[i]
		}
		buf[p] = s
		p++
	}
	for _, k := range keys {
		copy(buf[p:], k)
		p += len(k) + 1
	}
	binary.LittleEndian.PutUint16(buf, uint16(n))
	return buf
}

func loadColumnarConformance(b *testing.B) []byte {
	b.Helper()
	path := filepath.Join(conformanceDirB(b), "columnar_flat8_dense_100.nxb")
	data, err := os.ReadFile(path)
	if err != nil {
		b.Skip("conformance vector missing; run: make -C nyxis conformance-generate")
	}
	return data
}

func conformanceDirB(b *testing.B) string {
	b.Helper()
	_, file, _, _ := runtime.Caller(0)
	return filepath.Join(filepath.Dir(file), "..", "..", "nyxis", "conformance")
}

func benchColumnarReader(b *testing.B, nxb []byte) *Reader {
	b.Helper()
	r, err := NewReader(nxb)
	if err != nil {
		b.Fatal(err)
	}
	if r.LayoutKind() != LayoutColumnar {
		b.Fatalf("layout: got %v", r.LayoutKind())
	}
	return r
}

func BenchmarkColumnarSumF64_Conformance100(b *testing.B) {
	r := benchColumnarReader(b, loadColumnarConformance(b))
	b.ResetTimer()
	for b.Loop() {
		_ = r.SumF64("score")
	}
}

func BenchmarkColumnarColSumF64_Conformance100(b *testing.B) {
	r := benchColumnarReader(b, loadColumnarConformance(b))
	b.ResetTimer()
	for b.Loop() {
		_ = r.ColSumF64("score")
	}
}

func BenchmarkColumnarSumF64_100k(b *testing.B) {
	r := benchColumnarReader(b, buildColumnarFlat8Dense(100_000))
	b.ResetTimer()
	for b.Loop() {
		_ = r.SumF64("score")
	}
}

func BenchmarkColumnarColSumF64_100k(b *testing.B) {
	r := benchColumnarReader(b, buildColumnarFlat8Dense(100_000))
	b.ResetTimer()
	for b.Loop() {
		_ = r.ColSumF64("score")
	}
}

func TestColumnarInvalidFlags(t *testing.T) {
	path := filepath.Join(conformanceDir(t), "columnar_invalid_flags_both.nxb")
	data, err := os.ReadFile(path)
	if err != nil {
		t.Skip("conformance vector missing")
	}
	_, err = NewReader(data)
	if err == nil {
		t.Fatal("expected open error")
	}
}
