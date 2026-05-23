package nxs

// writer.go — NxsWriter: direct-to-buffer .nxb emitter for Go.
//
// Mirrors the Rust NxsWriter API:
//   NewSchema(keys)   — precompile schema once; share across Writer instances.
//   NewWriter(schema) — slot-based hot path; no per-field map lookup during write.
//
// Usage:
//
//	schema := nxs.NewSchema([]string{"id", "username", "score"})
//	w := nxs.NewWriter(schema)
//	w.BeginObject()
//	w.WriteI64(0, 42)
//	w.WriteStr(1, "alice")
//	w.WriteF64(2, 9.5)
//	w.EndObject()
//	data := w.Finish() // []byte

import (
	"encoding/binary"
	"fmt"
	"io"
	"math"
	"sort"
)

// ── Schema ───────────────────────────────────────────────────────────────────

// Schema holds precompiled key metadata for use with Writer.
type Schema struct {
	Keys         []string
	BitmaskBytes int // number of LEB128 bytes needed for len(Keys) bits
}

// NewSchema creates a Schema from the given key list.
func NewSchema(keys []string) *Schema {
	n := len(keys)
	bm := (n + 6) / 7
	if bm < 1 {
		bm = 1
	}
	ks := make([]string, n)
	copy(ks, keys)
	return &Schema{Keys: ks, BitmaskBytes: bm}
}

func (s *Schema) Len() int { return len(s.Keys) }

// ── Frame ────────────────────────────────────────────────────────────────────

type frame struct {
	start       int
	bitmask     []byte
	offsetTable []int // relative offsets in write order
	slotOffsets []slotOff
	lastSlot    int
	needsSort   bool
}

type slotOff struct {
	slot   int
	bufOff int
}

// ── Sigil constants ───────────────────────────────────────────────────────────

// TypeManifest sigils match SPEC.md / Rust nxs::consts (not ASCII type names).
const (
	sigilStr    = 0x22 // '"'
	sigilI64    = 0x3D // '='
	sigilF64    = 0x7E // '~'
	sigilBool   = 0x3F // '?'
	sigilNull   = 0x5E // '^'
	sigilBinary = 0x3C // '<'
	sigilTime   = 0x40 // '@'
)

// ── Writer ───────────────────────────────────────────────────────────────────

// Writer emits .nxb binary data for a fixed Schema.
// Use NewWriter to construct one, then call BeginObject/WriteXxx/EndObject per
// record, and Finish() to retrieve the complete file bytes.
type Writer struct {
	schema        *Schema
	buf           []byte
	frames        []frame
	recordOffsets []int
	slotSigils    []byte // sigil per schema slot, updated on first write
	slotSeen      []bool // whether the current writer has observed a value for the slot
}

// NewWriter allocates a Writer backed by the given Schema.
func NewWriter(schema *Schema) *Writer {
	sigils := make([]byte, schema.Len())
	for i := range sigils {
		sigils[i] = sigilStr // default; overwritten on first typed write per slot
	}
	return &Writer{
		schema:        schema,
		buf:           make([]byte, 0, 4096),
		frames:        make([]frame, 0, 4),
		recordOffsets: make([]int, 0),
		slotSigils:    sigils,
		slotSeen:      make([]bool, schema.Len()),
	}
}

// NewWriterWithCapacity is like NewWriter but pre-allocates buf capacity.
func NewWriterWithCapacity(schema *Schema, cap int) *Writer {
	sigils := make([]byte, schema.Len())
	for i := range sigils {
		sigils[i] = sigilStr
	}
	return &Writer{
		schema:        schema,
		buf:           make([]byte, 0, cap),
		frames:        make([]frame, 0, 4),
		recordOffsets: make([]int, 0, 1024),
		slotSigils:    sigils,
		slotSeen:      make([]bool, schema.Len()),
	}
}

// Reset clears buffered records while retaining allocated capacity.
func (w *Writer) Reset() {
	if len(w.frames) != 0 {
		panic("nxs: Reset with unclosed objects")
	}
	w.buf = w.buf[:0]
	w.frames = w.frames[:0]
	w.recordOffsets = w.recordOffsets[:0]
	for i := range w.slotSigils {
		w.slotSigils[i] = sigilStr
	}
	for i := range w.slotSeen {
		w.slotSeen[i] = false
	}
}

// BeginObject opens a new object.  Must be balanced with EndObject.
func (w *Writer) BeginObject() {
	if len(w.frames) == 0 {
		w.recordOffsets = append(w.recordOffsets, len(w.buf))
	}

	start := len(w.buf)

	// Build bitmask with LEB128 continuation bits pre-set
	bm := make([]byte, w.schema.BitmaskBytes)
	for i := 0; i < w.schema.BitmaskBytes-1; i++ {
		bm[i] = 0x80
	}

	w.frames = append(w.frames, frame{
		start:       start,
		bitmask:     bm,
		offsetTable: make([]int, 0, w.schema.Len()),
		slotOffsets: make([]slotOff, 0, w.schema.Len()),
		lastSlot:    -1,
		needsSort:   false,
	})

	// Magic (4) + Length placeholder (4)
	w.appendU32(magicObj)
	w.appendU32(0) // back-patched

	// Reserve bitmask
	w.buf = append(w.buf, bm...)

	// Reserve offset table: u16 per schema key
	w.buf = append(w.buf, make([]byte, w.schema.Len()*2)...)

	// Align data area start to 8 bytes from object start
	for (len(w.buf)-start)%8 != 0 {
		w.buf = append(w.buf, 0)
	}
}

// EndObject closes the current object and back-patches the header.
func (w *Writer) EndObject() {
	if len(w.frames) == 0 {
		panic("nxs: EndObject without BeginObject")
	}
	f := w.frames[len(w.frames)-1]
	w.frames = w.frames[:len(w.frames)-1]

	totalLen := len(w.buf) - f.start

	// Back-patch Length at f.start + 4
	binary.LittleEndian.PutUint32(w.buf[f.start+4:], uint32(totalLen))

	// Back-patch bitmask at f.start + 8
	bmOff := f.start + 8
	copy(w.buf[bmOff:], f.bitmask)

	// Back-patch offset table
	otStart := bmOff + w.schema.BitmaskBytes
	presentCount := len(f.offsetTable)

	if !f.needsSort {
		for i, rel := range f.offsetTable {
			binary.LittleEndian.PutUint16(w.buf[otStart+i*2:], uint16(rel))
		}
	} else {
		pairs := make([]slotOff, len(f.slotOffsets))
		copy(pairs, f.slotOffsets)
		sort.Slice(pairs, func(i, j int) bool { return pairs[i].slot < pairs[j].slot })
		for i, p := range pairs {
			rel := p.bufOff - f.start
			binary.LittleEndian.PutUint16(w.buf[otStart+i*2:], uint16(rel))
		}
	}

	// Zero unused offset-table slots
	used := presentCount * 2
	reserved := w.schema.Len() * 2
	for i := used; i < reserved; i++ {
		w.buf[otStart+i] = 0
	}
}

// Finish assembles and returns the complete .nxb file bytes.
func (w *Writer) Finish() []byte {
	if len(w.frames) != 0 {
		panic("nxs: Finish with unclosed objects")
	}

	schemaBytes := buildSchemaBytes(w.schema.Keys, w.slotSigils)
	dictHash := murmur3_64(schemaBytes)
	dataStartAbs := uint64(32 + len(schemaBytes))

	dataSector := w.buf
	tailPtr := dataStartAbs + uint64(len(dataSector))
	tail := buildTailIndexRecords(dataStartAbs, w.recordOffsets, tailPtr)

	total := 32 + len(schemaBytes) + len(dataSector) + len(tail)
	out := make([]byte, total)
	p := 0

	// Preamble
	binary.LittleEndian.PutUint32(out[p:], magicFile)
	p += 4
	binary.LittleEndian.PutUint16(out[p:], 0x0101)
	p += 2 // VERSION
	binary.LittleEndian.PutUint16(out[p:], 0x0002)
	p += 2 // FLAG_SCHEMA_EMBEDDED
	binary.LittleEndian.PutUint64(out[p:], dictHash)
	p += 8
	binary.LittleEndian.PutUint64(out[p:], 0)
	p += 8
	p += 8 // reserved

	copy(out[p:], schemaBytes)
	p += len(schemaBytes)
	copy(out[p:], dataSector)
	p += len(dataSector)
	copy(out[p:], tail)

	return out
}

// StreamWriter emits a streamable sealed .nxb file to an io.Writer.
// It writes the preamble and schema immediately, buffers only one top-level
// object at a time, then writes the tail-index and v1.1 footer on Close.
type StreamWriter struct {
	schema        *Schema
	out           io.Writer
	flush         func() error
	dataStartAbs  uint64
	bytesWritten  uint64
	recordOffsets []int
	writer        *Writer
	inObject      bool
	closed        bool
	headerWritten bool
	headerSigils  []byte
}

// NewStreamWriter creates a streaming writer. The preamble and schema header are
// emitted lazily on the first EndObject, using slot sigils from that record.
func NewStreamWriter(out io.Writer, schema *Schema) (*StreamWriter, error) {
	sw := &StreamWriter{
		schema:        schema,
		out:           out,
		recordOffsets: make([]int, 0, 1024),
		writer:        NewWriterWithCapacity(schema, 4096),
	}
	if f, ok := out.(interface{ Flush() error }); ok {
		sw.flush = f.Flush
	} else if f, ok := out.(interface{ Flush() }); ok {
		sw.flush = func() error {
			f.Flush()
			return nil
		}
	}
	return sw, nil
}

func (sw *StreamWriter) writeHeader(sigils []byte) error {
	schemaBytes := buildSchemaBytes(sw.schema.Keys, sigils)
	header := make([]byte, 32+len(schemaBytes))
	p := 0
	binary.LittleEndian.PutUint32(header[p:], magicFile)
	p += 4
	binary.LittleEndian.PutUint16(header[p:], 0x0101)
	p += 2
	binary.LittleEndian.PutUint16(header[p:], flagSchemaEmbedded)
	p += 2
	binary.LittleEndian.PutUint64(header[p:], murmur3_64(schemaBytes))
	p += 8
	binary.LittleEndian.PutUint64(header[p:], 0) // TailPtr unknown until footer.
	p += 8
	p += 8
	copy(header[p:], schemaBytes)
	if _, err := sw.out.Write(header); err != nil {
		return err
	}
	sw.dataStartAbs = sw.bytesWritten + uint64(len(header))
	sw.bytesWritten += uint64(len(header))
	sw.headerWritten = true
	sw.headerSigils = append(sw.headerSigils[:0], sigils...)
	return nil
}

func (sw *StreamWriter) validateRecordSigils() error {
	if !sw.headerWritten {
		return nil
	}
	for slot, seen := range sw.writer.slotSeen {
		if !seen {
			continue
		}
		if slot >= len(sw.headerSigils) {
			return io.ErrShortWrite
		}
		if got, want := sw.writer.slotSigils[slot], sw.headerSigils[slot]; got != want {
			return fmt.Errorf("nxs: stream slot %d wrote sigil 0x%02x after header declared 0x%02x", slot, got, want)
		}
	}
	return nil
}

func (sw *StreamWriter) BeginObject() {
	if sw.closed {
		panic("nxs: BeginObject after Close")
	}
	if sw.inObject {
		panic("nxs: nested top-level stream object")
	}
	sw.writer.Reset()
	sw.writer.BeginObject()
	sw.inObject = true
}

func (sw *StreamWriter) EndObject() error {
	if !sw.inObject {
		panic("nxs: EndObject without BeginObject")
	}
	sw.writer.EndObject()
	if !sw.headerWritten {
		sigils := make([]byte, len(sw.writer.slotSigils))
		copy(sigils, sw.writer.slotSigils)
		if err := sw.writeHeader(sigils); err != nil {
			return err
		}
	}
	if err := sw.validateRecordSigils(); err != nil {
		return err
	}
	record := sw.writer.buf
	sw.recordOffsets = append(sw.recordOffsets, int(sw.bytesWritten-sw.dataStartAbs))
	if _, err := sw.out.Write(record); err != nil {
		return err
	}
	sw.bytesWritten += uint64(len(record))
	sw.inObject = false
	if sw.flush != nil {
		return sw.flush()
	}
	return nil
}

func (sw *StreamWriter) WriteI64(slot int, v int64)       { sw.writer.WriteI64(slot, v) }
func (sw *StreamWriter) WriteF64(slot int, v float64)     { sw.writer.WriteF64(slot, v) }
func (sw *StreamWriter) WriteBool(slot int, v bool)       { sw.writer.WriteBool(slot, v) }
func (sw *StreamWriter) WriteTime(slot int, unixNs int64) { sw.writer.WriteTime(slot, unixNs) }
func (sw *StreamWriter) WriteNull(slot int)               { sw.writer.WriteNull(slot) }
func (sw *StreamWriter) WriteStr(slot int, v string)      { sw.writer.WriteStr(slot, v) }
func (sw *StreamWriter) WriteBytes(slot int, data []byte) { sw.writer.WriteBytes(slot, data) }
func (sw *StreamWriter) WriteListI64(slot int, values []int64) {
	sw.writer.WriteListI64(slot, values)
}
func (sw *StreamWriter) WriteListF64(slot int, values []float64) {
	sw.writer.WriteListF64(slot, values)
}

// Close writes the tail-index and v1.1 footer. It must be called exactly once
// after all records are written.
func (sw *StreamWriter) Close() error {
	if sw.closed {
		return nil
	}
	if sw.inObject {
		panic("nxs: Close with unclosed object")
	}
	if !sw.headerWritten {
		defaultSigils := make([]byte, sw.schema.Len())
		for i := range defaultSigils {
			defaultSigils[i] = sigilStr
		}
		if err := sw.writeHeader(defaultSigils); err != nil {
			return err
		}
	}
	tailPtr := sw.bytesWritten
	tail := buildTailIndexRecords(sw.dataStartAbs, sw.recordOffsets, tailPtr)
	if _, err := sw.out.Write(tail); err != nil {
		return err
	}
	sw.bytesWritten += uint64(len(tail))
	sw.closed = true
	if sw.flush != nil {
		return sw.flush()
	}
	return nil
}

// ── Typed write methods ───────────────────────────────────────────────────────

func (w *Writer) WriteI64(slot int, v int64) {
	w.markSlot(slot, sigilI64)
	w.buf = binary.LittleEndian.AppendUint64(w.buf, uint64(v))
}

func (w *Writer) WriteF64(slot int, v float64) {
	w.markSlot(slot, sigilF64)
	w.buf = binary.LittleEndian.AppendUint64(w.buf, math.Float64bits(v))
}

func (w *Writer) WriteBool(slot int, v bool) {
	w.markSlot(slot, sigilBool)
	if v {
		w.buf = append(w.buf, 0x01)
	} else {
		w.buf = append(w.buf, 0x00)
	}
	w.buf = append(w.buf, 0, 0, 0, 0, 0, 0, 0) // 7 padding bytes
}

func (w *Writer) WriteTime(slot int, unixNs int64) {
	w.markSlot(slot, sigilTime)
	w.buf = binary.LittleEndian.AppendUint64(w.buf, uint64(unixNs))
}

func (w *Writer) WriteNull(slot int) {
	w.markSlot(slot, sigilNull)
	w.buf = append(w.buf, 0, 0, 0, 0, 0, 0, 0, 0)
}

func (w *Writer) WriteStr(slot int, v string) {
	w.markSlot(slot, sigilStr)
	b := []byte(v)
	w.buf = binary.LittleEndian.AppendUint32(w.buf, uint32(len(b)))
	w.buf = append(w.buf, b...)
	used := (4 + len(b)) % 8
	if used != 0 {
		w.buf = append(w.buf, make([]byte, 8-used)...)
	}
}

func (w *Writer) WriteBytes(slot int, data []byte) {
	w.markSlot(slot, sigilBinary)
	w.buf = binary.LittleEndian.AppendUint32(w.buf, uint32(len(data)))
	w.buf = append(w.buf, data...)
	used := (4 + len(data)) % 8
	if used != 0 {
		w.buf = append(w.buf, make([]byte, 8-used)...)
	}
}

func (w *Writer) WriteListI64(slot int, values []int64) {
	w.markSlot(slot, sigilStr) // list is var-length in the manifest
	total := 16 + len(values)*8
	w.buf = binary.LittleEndian.AppendUint32(w.buf, magicList)
	w.buf = binary.LittleEndian.AppendUint32(w.buf, uint32(total))
	w.buf = append(w.buf, 0x3D) // SIGIL_INT '='
	w.buf = binary.LittleEndian.AppendUint32(w.buf, uint32(len(values)))
	w.buf = append(w.buf, 0, 0, 0) // padding
	for _, v := range values {
		w.buf = binary.LittleEndian.AppendUint64(w.buf, uint64(v))
	}
}

func (w *Writer) WriteListF64(slot int, values []float64) {
	w.markSlot(slot, sigilStr) // list is var-length in the manifest
	total := 16 + len(values)*8
	w.buf = binary.LittleEndian.AppendUint32(w.buf, magicList)
	w.buf = binary.LittleEndian.AppendUint32(w.buf, uint32(total))
	w.buf = append(w.buf, 0x7E) // SIGIL_FLOAT '~'
	w.buf = binary.LittleEndian.AppendUint32(w.buf, uint32(len(values)))
	w.buf = append(w.buf, 0, 0, 0)
	for _, v := range values {
		w.buf = binary.LittleEndian.AppendUint64(w.buf, math.Float64bits(v))
	}
}

// ── Convenience function ──────────────────────────────────────────────────────

// FromRecords writes records from a slice of map[string]interface{} and returns
// the complete .nxb bytes.
func FromRecords(keys []string, records []map[string]interface{}) []byte {
	schema := NewSchema(keys)
	w := NewWriter(schema)
	for _, rec := range records {
		w.BeginObject()
		for i, key := range keys {
			val, ok := rec[key]
			if !ok {
				continue
			}
			switch v := val.(type) {
			case nil:
				w.WriteNull(i)
			case bool:
				w.WriteBool(i, v)
			case int:
				w.WriteI64(i, int64(v))
			case int32:
				w.WriteI64(i, int64(v))
			case int64:
				w.WriteI64(i, v)
			case float32:
				w.WriteF64(i, float64(v))
			case float64:
				w.WriteF64(i, v)
			case string:
				w.WriteStr(i, v)
			case []byte:
				w.WriteBytes(i, v)
			}
		}
		w.EndObject()
	}
	return w.Finish()
}

// ── Private helpers ───────────────────────────────────────────────────────────

func (w *Writer) markSlot(slot int, sigil byte) {
	if len(w.frames) == 0 {
		panic("nxs: write outside BeginObject/EndObject")
	}
	f := &w.frames[len(w.frames)-1]

	byteIdx := slot / 7
	bitIdx := uint(slot % 7)
	f.bitmask[byteIdx] |= 1 << bitIdx

	// Record the sigil for this slot (last write wins, consistent within a schema)
	w.slotSigils[slot] = sigil
	w.slotSeen[slot] = true

	rel := len(w.buf) - f.start

	if slot < f.lastSlot {
		f.needsSort = true
	}
	f.lastSlot = slot

	f.offsetTable = append(f.offsetTable, rel)
	f.slotOffsets = append(f.slotOffsets, slotOff{slot, len(w.buf)})
}

func (w *Writer) appendU32(v uint32) {
	w.buf = binary.LittleEndian.AppendUint32(w.buf, v)
}

func buildSchemaBytes(keys []string, sigils []byte) []byte {
	n := len(keys)
	// Compute size: KeyCount(2) + TypeManifest(n) + null-terminated strings
	size := 2 + n
	for _, k := range keys {
		size += len(k) + 1
	}
	// Pad to 8-byte boundary
	if rem := size % 8; rem != 0 {
		size += 8 - rem
	}

	buf := make([]byte, size)
	p := 0
	binary.LittleEndian.PutUint16(buf[p:], uint16(n))
	p += 2
	for i := range keys {
		buf[p] = sigils[i]
		p++
	}
	for _, k := range keys {
		copy(buf[p:], k)
		p += len(k)
		buf[p] = 0x00
		p++
	}
	// Remaining bytes are zero (make initialises to 0)
	return buf
}

func buildTailIndexRecords(dataStart uint64, recordOffsets []int, tailPtr uint64) []byte {
	n := len(recordOffsets)
	// EntryCount(4) + N * [KeyID(2) + AbsOff(8)] + FooterTailPtr(8) + MagicFooter(4)
	buf := make([]byte, 4+n*10+12)
	p := 0

	binary.LittleEndian.PutUint32(buf[p:], uint32(n))
	p += 4

	for i, relOff := range recordOffsets {
		binary.LittleEndian.PutUint16(buf[p:], uint16(i))
		p += 2
		binary.LittleEndian.PutUint64(buf[p:], dataStart+uint64(relOff))
		p += 8
	}

	binary.LittleEndian.PutUint64(buf[p:], tailPtr)
	p += 8

	// MagicFooter
	binary.LittleEndian.PutUint32(buf[p:], magicFooter)

	return buf
}
