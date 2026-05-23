package nxs

import (
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

// TestWriteProducerFixture writes a minimal .nxb for cross-language checks (see nyxis rust test).
func TestWriteProducerFixture(t *testing.T) {
	out := os.Getenv("NXS_GO_PRODUCER_OUT")
	if out == "" {
		t.Skip("NXS_GO_PRODUCER_OUT not set")
	}
	data := minimalProducerNxb(t)
	if err := os.WriteFile(out, data, 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestGoProducerSchemaSigilsMatchSpec(t *testing.T) {
	schema := NewSchema([]string{"id", "name", "active"})
	w := NewWriter(schema)
	w.BeginObject()
	w.WriteI64(0, 42)
	w.WriteStr(1, "hello")
	w.WriteBool(2, true)
	w.EndObject()
	data := w.Finish()

	if len(data) < 32+3 {
		t.Fatal("file too short")
	}
	keyCount := int(data[32]) | int(data[33])<<8
	if keyCount != 3 {
		t.Fatalf("key_count = %d, want 3", keyCount)
	}
	want := []byte{sigilI64, sigilStr, sigilBool}
	for i, s := range want {
		if data[34+i] != s {
			t.Fatalf("sigil[%d] = 0x%02x, want 0x%02x", i, data[34+i], s)
		}
	}

	r, err := NewReader(data)
	if err != nil {
		t.Fatal(err)
	}
	if got, ok := r.Record(0).GetI64("id"); !ok || got != 42 {
		t.Fatalf("id = %v ok=%v", got, ok)
	}
}

func TestStreamWriterTypeManifestSigils(t *testing.T) {
	schema := NewSchema([]string{"id", "name", "score", "active"})
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
	if err := sw.Close(); err != nil {
		t.Fatal(err)
	}
	data := buf.Bytes()
	if data[34] != sigilI64 || data[35] != sigilStr || data[36] != sigilF64 || data[37] != sigilBool {
		t.Fatalf("stream sigils = %02x %02x %02x %02x", data[34], data[35], data[36], data[37])
	}
}

func minimalProducerNxb(t *testing.T) []byte {
	t.Helper()
	schema := NewSchema([]string{"id", "name", "active"})
	w := NewWriter(schema)
	w.BeginObject()
	w.WriteI64(0, 42)
	w.WriteStr(1, "hello")
	w.WriteBool(2, true)
	w.EndObject()
	return w.Finish()
}

// TestGoProducerMatchesRustConformance runs when the nyxis repo is sibling to nyxis-drivers.
func TestGoProducerMatchesRustConformance(t *testing.T) {
	conf := filepath.Join("..", "..", "nyxis", "conformance")
	if _, err := os.Stat(filepath.Join(conf, "minimal.expected.json")); err != nil {
		t.Skip("nyxis/conformance not found (monorepo layout required)")
	}
	data := minimalProducerNxb(t)
	tmp := t.TempDir()
	goNxb := filepath.Join(tmp, "go_minimal.nxb")
	if err := os.WriteFile(goNxb, data, 0o644); err != nil {
		t.Fatal(err)
	}
	rustNxb := filepath.Join(conf, "minimal.nxb")
	if _, err := os.Stat(rustNxb); err != nil {
		t.Skip("run make conformance-generate in nyxis first")
	}
	rustData, err := os.ReadFile(rustNxb)
	if err != nil {
		t.Fatal(err)
	}
	// Same keys and values → identical DictHash (schema sigils must match).
	if murmur3_64(schemaBytesFromFile(data)) != murmur3_64(schemaBytesFromFile(rustData)) {
		t.Fatal("DictHash mismatch vs Rust minimal.nxb — TypeManifest sigils differ")
	}
}

func schemaBytesFromFile(nxb []byte) []byte {
	if len(nxb) < 34 {
		return nil
	}
	n := int(uint16(nxb[32]) | uint16(nxb[33])<<8)
	start := 34
	end := start + n
	for end < len(nxb) && nxb[end] != 0 {
		for end < len(nxb) && nxb[end] != 0 {
			end++
		}
		end++
	}
	for end%8 != 0 {
		end++
	}
	if end > len(nxb) {
		end = len(nxb)
	}
	return nxb[32:end]
}

func TestGoProducerRustRoundtripViaSubprocess(t *testing.T) {
	if _, err := exec.LookPath("go"); err != nil {
		t.Skip("go not in PATH")
	}
	// Exercised from nyxis/rust/tests/go_producer_roundtrip.rs
	t.Skip("invoked via NXS_GO_PRODUCER_OUT from Rust integration test")
}
