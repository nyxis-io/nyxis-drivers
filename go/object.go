package nxs

// object.go — standalone NYXO objects outside a .nxb file.
//
// ObjectEncoder produces the same object bytes Writer places in a data sector,
// so frames written by an append-only log (see go/wal) decode with the same
// reader code. Sigils are fixed per slot up front because a standalone object
// carries no schema header.

import (
	"encoding/binary"
	"errors"
	"fmt"
	"math"
)

// ErrObjectTooLarge reports a field that starts beyond the 16-bit offset range
// of the NYXO offset table.
var ErrObjectTooLarge = errors.New("nxs: object field offset exceeds 65535 bytes")

// ObjectEncoder emits a single NYXO object for a fixed schema, independent of
// a file preamble or tail-index. Every write must match the slot's sigil and
// each slot may be written at most once; violations surface from Bytes.
type ObjectEncoder struct {
	keys   []string
	sigils []byte
	w      *Writer
	cfgErr error
	err    error
	open   bool
	done   bool
}

// NewObjectEncoder returns an encoder for objects with the given keys and
// per-slot sigils ('=', '~', '?', '"', '<', '@', '^').
func NewObjectEncoder(keys []string, sigils []byte) *ObjectEncoder {
	ks := append([]string(nil), keys...)
	e := &ObjectEncoder{
		keys:   ks,
		sigils: append([]byte(nil), sigils...),
		w:      NewWriter(NewSchema(ks)),
	}
	if len(keys) != len(sigils) {
		e.cfgErr = fmt.Errorf("nxs: %d keys but %d sigils", len(keys), len(sigils))
	}
	return e
}

// Reset discards the current object so the encoder can build the next one.
func (e *ObjectEncoder) Reset() {
	e.w.reset()
	e.err = nil
	e.open = false
	e.done = false
}

func (e *ObjectEncoder) begin(slot int, sigil byte) bool {
	if e.cfgErr != nil || e.err != nil {
		return false
	}
	if e.done {
		e.err = errors.New("nxs: ObjectEncoder write after Bytes without Reset")
		return false
	}
	if slot < 0 || slot >= len(e.keys) {
		e.err = fmt.Errorf("nxs: slot %d out of range [0, %d)", slot, len(e.keys))
		return false
	}
	if e.sigils[slot] != sigil {
		e.err = fmt.Errorf("nxs: slot %d (%s) has sigil %q, write used %q", slot, e.keys[slot], e.sigils[slot], sigil)
		return false
	}
	e.ensureOpen()
	if e.w.slotSeen[slot] {
		e.err = fmt.Errorf("nxs: slot %d (%s) written twice", slot, e.keys[slot])
		return false
	}
	return true
}

func (e *ObjectEncoder) ensureOpen() {
	if !e.open {
		e.w.reset()
		e.w.BeginObject()
		e.open = true
	}
}

func (e *ObjectEncoder) WriteI64(slot int, v int64) {
	if e.begin(slot, sigilI64) {
		e.w.WriteI64(slot, v)
	}
}

func (e *ObjectEncoder) WriteF64(slot int, v float64) {
	if e.begin(slot, sigilF64) {
		e.w.WriteF64(slot, v)
	}
}

func (e *ObjectEncoder) WriteBool(slot int, v bool) {
	if e.begin(slot, sigilBool) {
		e.w.WriteBool(slot, v)
	}
}

func (e *ObjectEncoder) WriteStr(slot int, v string) {
	if e.begin(slot, sigilStr) {
		e.w.WriteStr(slot, v)
	}
}

func (e *ObjectEncoder) WriteBytes(slot int, v []byte) {
	if e.begin(slot, sigilBinary) {
		e.w.WriteBytes(slot, v)
	}
}

func (e *ObjectEncoder) WriteTime(slot int, unixNs int64) {
	if e.begin(slot, sigilTime) {
		e.w.WriteTime(slot, unixNs)
	}
}

func (e *ObjectEncoder) WriteNull(slot int) {
	if e.begin(slot, sigilNull) {
		e.w.WriteNull(slot)
	}
}

// Bytes closes the object and returns its bytes (a multiple of 8 long). The
// slice is valid until Reset. It returns the first sigil, slot, or size error.
func (e *ObjectEncoder) Bytes() ([]byte, error) {
	if e.cfgErr != nil {
		return nil, e.cfgErr
	}
	if e.err != nil {
		return nil, e.err
	}
	if !e.done {
		e.ensureOpen()
		f := e.w.frames[len(e.w.frames)-1]
		if n := len(f.offsetTable); n > 0 && f.offsetTable[n-1] > math.MaxUint16 {
			e.err = ErrObjectTooLarge
			return nil, e.err
		}
		e.w.EndObject()
		e.open = false
		e.done = true
	}
	return e.w.buf, nil
}

// ObjectDecoder decodes standalone NYXO objects that share one schema.
type ObjectDecoder struct {
	keys     []string
	sigils   []byte
	keyIndex map[string]int
}

// NewObjectDecoder validates the schema and returns a reusable decoder.
func NewObjectDecoder(keys []string, sigils []byte) (*ObjectDecoder, error) {
	if len(keys) != len(sigils) {
		return nil, fmt.Errorf("nxs: %d keys but %d sigils", len(keys), len(sigils))
	}
	idx := make(map[string]int, len(keys))
	for i, k := range keys {
		idx[k] = i
	}
	return &ObjectDecoder{
		keys:     append([]string(nil), keys...),
		sigils:   append([]byte(nil), sigils...),
		keyIndex: idx,
	}, nil
}

// Decode validates obj against the schema and returns a read view over it.
// The view aliases obj. Every present field is bounds-checked here, so
// getters on the returned Object cannot read past the object.
func (d *ObjectDecoder) Decode(obj []byte) (*Object, error) {
	n, err := validateObject(d.sigils, obj)
	if err != nil {
		return nil, err
	}
	r := &Reader{
		data:      obj[:n],
		Keys:      d.keys,
		KeySigils: d.sigils,
		keyIndex:  d.keyIndex,
		layout:    LayoutRow,
	}
	return &Object{reader: r}, nil
}

// DecodeObject returns a read view over one standalone NYXO object. Getters
// are by key name; a key absent from the schema or the object reports ok=false.
func DecodeObject(keys []string, sigils []byte, obj []byte) (*Object, error) {
	d, err := NewObjectDecoder(keys, sigils)
	if err != nil {
		return nil, err
	}
	return d.Decode(obj)
}

// validateObject checks the NYXO header, bitmask, offset table, and the extent
// of every present field. It returns the object length.
func validateObject(sigils []byte, obj []byte) (int, error) {
	if len(obj) < 8 {
		return 0, fmt.Errorf("ERR_OUT_OF_BOUNDS: object header")
	}
	if binary.LittleEndian.Uint32(obj[0:4]) != magicObj {
		return 0, fmt.Errorf("ERR_BAD_MAGIC: object")
	}
	length := int(binary.LittleEndian.Uint32(obj[4:8]))
	if length > len(obj) {
		return 0, fmt.Errorf("ERR_OUT_OF_BOUNDS: object length %d exceeds buffer %d", length, len(obj))
	}
	keyCount := len(sigils)
	bmBytes := NewSchema(make([]string, keyCount)).BitmaskBytes
	otStart := 8 + bmBytes
	dataStart := otStart + keyCount*2
	if rem := dataStart % 8; rem != 0 {
		dataStart += 8 - rem
	}
	if dataStart > length {
		return 0, fmt.Errorf("ERR_OUT_OF_BOUNDS: object header %d exceeds length %d", dataStart, length)
	}
	rank := 0
	for i := 0; i < bmBytes; i++ {
		b := obj[8+i]
		if (b&0x80 != 0) != (i < bmBytes-1) {
			return 0, fmt.Errorf("ERR_BAD_BITMASK: length does not match %d keys", keyCount)
		}
		for bit := 0; bit < 7; bit++ {
			if (b>>bit)&1 == 0 {
				continue
			}
			slot := i*7 + bit
			if slot >= keyCount {
				return 0, fmt.Errorf("ERR_BAD_BITMASK: slot %d beyond %d keys", slot, keyCount)
			}
			rel := int(binary.LittleEndian.Uint16(obj[otStart+rank*2:]))
			rank++
			if rel < dataStart || rel+8 > length {
				return 0, fmt.Errorf("ERR_OUT_OF_BOUNDS: slot %d offset %d", slot, rel)
			}
			if sigils[slot] == sigilStr || sigils[slot] == sigilBinary {
				n := int(binary.LittleEndian.Uint32(obj[rel : rel+4]))
				if n > length-rel-4 {
					return 0, fmt.Errorf("ERR_OUT_OF_BOUNDS: slot %d value length %d", slot, n)
				}
			}
		}
	}
	return length, nil
}

// GetBytes returns a binary ('<') field. Row-layout and standalone objects
// return a slice aliasing the underlying buffer.
func (o *Object) GetBytes(key string) ([]byte, bool) {
	slot, ok := o.reader.keyIndex[key]
	if !ok {
		return nil, false
	}
	if o.usesColumnarFieldAccess() {
		return o.reader.ColGetBinary(key, o.recordIndex)
	}
	off := o.resolveSlot(slot)
	if off < 0 {
		return nil, false
	}
	if o.reader.extSchema != nil {
		return []byte(materialiseStrAt(o.reader.data, off, slot, o.reader.extSchema)), true
	}
	n := int(binary.LittleEndian.Uint32(o.reader.data[off : off+4]))
	return o.reader.data[off+4 : off+4+n], true
}

// GetTime returns a time ('@') field as Unix nanoseconds.
func (o *Object) GetTime(key string) (int64, bool) {
	return o.GetI64(key)
}

// Has reports whether key is in the schema and present in this object. For a
// null ('^') slot, presence is the value.
func (o *Object) Has(key string) bool {
	slot, ok := o.reader.keyIndex[key]
	if !ok {
		return false
	}
	if o.reader.KeySigils[slot] == sigilBool {
		_, ok = o.GetBoolBySlot(slot)
		return ok
	}
	if o.usesColumnarFieldAccess() {
		switch o.reader.KeySigils[slot] {
		case sigilStr:
			_, ok = o.reader.ColGetStr(key, o.recordIndex)
		case sigilBinary:
			_, ok = o.reader.ColGetBinary(key, o.recordIndex)
		default:
			_, ok = o.reader.colNumericBytes(o.recordIndex, slot)
		}
		return ok
	}
	return o.resolveSlot(slot) >= 0
}
