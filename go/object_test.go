package nxs

import (
	"bytes"
	"encoding/binary"
	"testing"
)

var (
	objKeys   = []string{"n", "f", "b", "s", "bin", "t", "nul"}
	objSigils = []byte{'=', '~', '?', '"', '<', '@', '^'}
	objBin    = []byte{0x00, 0x01, 0x02, 0xff, 0x7f}
)

const objTime = int64(1_700_000_000_123_456_789)

func encodeAllSigils(t *testing.T, e *ObjectEncoder, n int64, s string) []byte {
	t.Helper()
	e.WriteI64(0, n)
	e.WriteF64(1, 2.5)
	e.WriteBool(2, true)
	e.WriteStr(3, s)
	e.WriteBytes(4, objBin)
	e.WriteTime(5, objTime)
	e.WriteNull(6)
	b, err := e.Bytes()
	if err != nil {
		t.Fatalf("Bytes: %v", err)
	}
	return append([]byte(nil), b...)
}

func assertAllSigils(t *testing.T, o *Object, n int64, s string) {
	t.Helper()
	if v, ok := o.GetI64("n"); !ok || v != n {
		t.Errorf("GetI64(n) = %d, %v; want %d", v, ok, n)
	}
	if v, ok := o.GetF64("f"); !ok || v != 2.5 {
		t.Errorf("GetF64(f) = %v, %v; want 2.5", v, ok)
	}
	if v, ok := o.GetBool("b"); !ok || !v {
		t.Errorf("GetBool(b) = %v, %v; want true", v, ok)
	}
	if v, ok := o.GetStr("s"); !ok || v != s {
		t.Errorf("GetStr(s) = %q, %v; want %q", v, ok, s)
	}
	if v, ok := o.GetBytes("bin"); !ok || !bytes.Equal(v, objBin) {
		t.Errorf("GetBytes(bin) = %x, %v; want %x", v, ok, objBin)
	}
	if v, ok := o.GetTime("t"); !ok || v != objTime {
		t.Errorf("GetTime(t) = %d, %v; want %d", v, ok, objTime)
	}
	if !o.Has("nul") {
		t.Errorf("Has(nul) = false; want true")
	}
}

func TestObjectEncoder_RoundTripAllSigils(t *testing.T) {
	e := NewObjectEncoder(objKeys, objSigils)
	b := encodeAllSigils(t, e, -42, "héllo, wal")
	if len(b)%8 != 0 {
		t.Fatalf("object length %d is not 8-byte aligned", len(b))
	}
	o, err := DecodeObject(objKeys, objSigils, b)
	if err != nil {
		t.Fatalf("DecodeObject: %v", err)
	}
	assertAllSigils(t, o, -42, "héllo, wal")
}

func TestObjectEncoder_MatchesWriterObjectBytes(t *testing.T) {
	e := NewObjectEncoder(objKeys, objSigils)
	got := encodeAllSigils(t, e, 7, "same bytes")

	w := NewWriter(NewSchema(objKeys))
	w.BeginObject()
	w.WriteI64(0, 7)
	w.WriteF64(1, 2.5)
	w.WriteBool(2, true)
	w.WriteStr(3, "same bytes")
	w.WriteBytes(4, objBin)
	w.WriteTime(5, objTime)
	w.WriteNull(6)
	w.EndObject()
	file := w.Finish()

	r, err := NewReader(file)
	if err != nil {
		t.Fatalf("NewReader: %v", err)
	}
	off := int(r.recordByteOffset(0))
	length := int(binary.LittleEndian.Uint32(file[off+4:]))
	want := file[off : off+length]
	if !bytes.Equal(got, want) {
		t.Fatalf("encoder bytes differ from writer object region\n got: %x\nwant: %x", got, want)
	}
}

func TestObjectEncoder_SigilConflictErrors(t *testing.T) {
	cases := []struct {
		name  string
		write func(e *ObjectEncoder)
	}{
		{"str on int slot", func(e *ObjectEncoder) { e.WriteStr(0, "x") }},
		{"null on str slot", func(e *ObjectEncoder) { e.WriteNull(1) }},
		{"slot out of range", func(e *ObjectEncoder) { e.WriteI64(2, 1) }},
		{"negative slot", func(e *ObjectEncoder) { e.WriteI64(-1, 1) }},
		{"slot written twice", func(e *ObjectEncoder) { e.WriteI64(0, 1); e.WriteI64(0, 2) }},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			e := NewObjectEncoder([]string{"n", "s"}, []byte{'=', '"'})
			tc.write(e)
			if _, err := e.Bytes(); err == nil {
				t.Fatal("Bytes() error = nil; want sigil/slot error")
			}
			e.Reset()
			e.WriteI64(0, 1)
			if _, err := e.Bytes(); err != nil {
				t.Fatalf("Bytes() after Reset = %v; want nil", err)
			}
		})
	}
}

func TestObjectEncoder_ResetReuse(t *testing.T) {
	e := NewObjectEncoder(objKeys, objSigils)
	a := encodeAllSigils(t, e, 1, "alpha")
	e.Reset()
	b := encodeAllSigils(t, e, 2, "a considerably longer second value")

	oa, err := DecodeObject(objKeys, objSigils, a)
	if err != nil {
		t.Fatalf("DecodeObject(a): %v", err)
	}
	ob, err := DecodeObject(objKeys, objSigils, b)
	if err != nil {
		t.Fatalf("DecodeObject(b): %v", err)
	}
	assertAllSigils(t, oa, 1, "alpha")
	assertAllSigils(t, ob, 2, "a considerably longer second value")
}

func TestDecodeObject_MissingKeyReportsAbsent(t *testing.T) {
	e := NewObjectEncoder(objKeys, objSigils)
	e.WriteI64(0, 9)
	e.WriteStr(3, "sparse")
	b, err := e.Bytes()
	if err != nil {
		t.Fatalf("Bytes: %v", err)
	}
	o, err := DecodeObject(objKeys, objSigils, b)
	if err != nil {
		t.Fatalf("DecodeObject: %v", err)
	}
	if v, ok := o.GetI64("n"); !ok || v != 9 {
		t.Errorf("GetI64(n) = %d, %v; want 9", v, ok)
	}
	if v, ok := o.GetStr("s"); !ok || v != "sparse" {
		t.Errorf("GetStr(s) = %q, %v; want sparse", v, ok)
	}
	if _, ok := o.GetF64("f"); ok {
		t.Error("GetF64(f) ok = true for an unwritten slot")
	}
	if _, ok := o.GetBytes("bin"); ok {
		t.Error("GetBytes(bin) ok = true for an unwritten slot")
	}
	if o.Has("nul") {
		t.Error("Has(nul) = true for an unwritten slot")
	}
	if _, ok := o.GetStr("not_in_schema"); ok {
		t.Error("GetStr(not_in_schema) ok = true")
	}
	if o.Has("not_in_schema") {
		t.Error("Has(not_in_schema) = true")
	}
}

func TestDecodeObject_RejectsBadMagicAndTruncation(t *testing.T) {
	e := NewObjectEncoder(objKeys, objSigils)
	valid := encodeAllSigils(t, e, 3, "truncate me")

	mutate := func(f func(b []byte) []byte) []byte {
		return f(append([]byte(nil), valid...))
	}
	cases := []struct {
		name   string
		keys   []string
		sigils []byte
		obj    []byte
	}{
		{"nil", objKeys, objSigils, nil},
		{"header only", objKeys, objSigils, valid[:4]},
		{"bad magic", objKeys, objSigils, mutate(func(b []byte) []byte { b[0] ^= 0xff; return b })},
		{"truncated body", objKeys, objSigils, valid[:len(valid)-8]},
		{"length past buffer", objKeys, objSigils, mutate(func(b []byte) []byte {
			binary.LittleEndian.PutUint32(b[4:], uint32(len(b)+64))
			return b
		})},
		{"offset past end", objKeys, objSigils, mutate(func(b []byte) []byte {
			binary.LittleEndian.PutUint16(b[8+1:], 0xfff0)
			return b
		})},
		{"string length past end", objKeys, objSigils, mutate(func(b []byte) []byte {
			rel := binary.LittleEndian.Uint16(b[8+1+3*2:])
			binary.LittleEndian.PutUint32(b[rel:], 1<<20)
			return b
		})},
		{"keys and sigils differ in length", objKeys, objSigils[:3], valid},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if _, err := DecodeObject(tc.keys, tc.sigils, tc.obj); err == nil {
				t.Fatal("DecodeObject error = nil; want error")
			}
		})
	}
}
