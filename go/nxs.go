// Package nxs is a zero-copy reader for the Nyxis (.nxb) binary format.
//
// Usage:
//
//	buf, _ := os.ReadFile("data.nxb")
//	r, err := nxs.NewReader(buf)
//	if err != nil { ... }
//	fmt.Println(r.RecordCount())
//	obj := r.Record(42)
//	u := obj.GetStr("username")   // decodes one field
package nxs

import (
	"encoding/binary"
	"fmt"
	"math"
)

// ── Format constants ─────────────────────────────────────────────────────────

const (
	magicFile   uint32 = 0x4E595842 // NYXB
	magicObj    uint32 = 0x4E59584F // NYXO
	magicList   uint32 = 0x4E59584C // NYXL
	magicFooter uint32 = 0x2153584E // NXS!

	flagSchemaEmbedded uint16 = 0x0002
)

// Layout values are defined in col.go (LayoutRow, LayoutColumnar, LayoutPAX).

// ── Reader ───────────────────────────────────────────────────────────────────

// Reader parses the preamble, schema, and tail-index of a .nxb buffer.
// The data sector is not walked — records load lazily via Record(i).
type Reader struct {
	data         []byte
	Version      uint16
	Flags        uint16
	DictHash     uint64
	TailPtr      uint64
	Keys         []string
	KeySigils    []byte
	keyIndex     map[string]int
	recordCount  uint32
	tailStart    int
	layout       Layout
	colBufOff    []uint64
	colBufLen    []uint64
	pageCount    uint32
	pageSizeHint uint32
	pageIndex    []uint32
	pageRecStart []uint64
	pageRecCount []uint32
	pageOffset   []uint64
	pageLength   []uint32
}

// NewReader validates the file header and extracts the schema and tail-index
// location. It does not parse any record data.
func NewReader(data []byte) (*Reader, error) {
	if len(data) < 32 {
		return nil, fmt.Errorf("ERR_OUT_OF_BOUNDS: file too small")
	}
	if binary.LittleEndian.Uint32(data[0:4]) != magicFile {
		return nil, fmt.Errorf("ERR_BAD_MAGIC: preamble")
	}
	if binary.LittleEndian.Uint32(data[len(data)-4:]) != magicFooter {
		return nil, fmt.Errorf("ERR_BAD_MAGIC: footer")
	}

	r := &Reader{
		data:     data,
		Version:  binary.LittleEndian.Uint16(data[4:6]),
		Flags:    binary.LittleEndian.Uint16(data[6:8]),
		DictHash: binary.LittleEndian.Uint64(data[8:16]),
		TailPtr:  binary.LittleEndian.Uint64(data[16:24]),
	}
	// Schema
	if r.Flags&flagSchemaEmbedded != 0 {
		schemaEnd, err := r.readSchema(32)
		if err != nil {
			return nil, err
		}
		if murmur3_64(data[32:schemaEnd]) != r.DictHash {
			return nil, fmt.Errorf("ERR_DICT_MISMATCH: schema hash mismatch")
		}
	}

	if err := r.parseLayoutTail(); err != nil {
		return nil, err
	}
	return r, nil
}

func (r *Reader) readSchema(offset int) (int, error) {
	if offset+2 > len(r.data) {
		return 0, fmt.Errorf("ERR_OUT_OF_BOUNDS: schema header")
	}
	keyCount := int(binary.LittleEndian.Uint16(r.data[offset : offset+2]))
	offset += 2

	// TypeManifest
	if offset+keyCount > len(r.data) {
		return 0, fmt.Errorf("ERR_OUT_OF_BOUNDS: type manifest")
	}
	r.KeySigils = make([]byte, keyCount)
	copy(r.KeySigils, r.data[offset:offset+keyCount])
	offset += keyCount

	// StringPool — null-terminated UTF-8
	r.Keys = make([]string, 0, keyCount)
	r.keyIndex = make(map[string]int, keyCount)
	for i := 0; i < keyCount; i++ {
		start := offset
		for offset < len(r.data) && r.data[offset] != 0 {
			offset++
		}
		if offset >= len(r.data) {
			return 0, fmt.Errorf("ERR_OUT_OF_BOUNDS: string pool")
		}
		k := string(r.data[start:offset])
		r.Keys = append(r.Keys, k)
		r.keyIndex[k] = i
		offset++ // skip null terminator
	}
	// Pad to 8-byte boundary
	if rem := offset % 8; rem != 0 {
		offset += 8 - rem
	}
	return offset, nil
}

// murmur3_64 is the hash function used to compute and verify DictHash.
func murmur3_64(data []byte) uint64 {
	var h uint64 = 0x93681D6255313A99
	for i := 0; i < len(data); i += 8 {
		chunk := data[i:]
		if len(chunk) > 8 {
			chunk = chunk[:8]
		}
		var k uint64
		for j, b := range chunk {
			k |= uint64(b) << (j * 8)
		}
		k *= 0xFF51AFD7ED558CCD
		k ^= k >> 33
		h ^= k
		h *= 0xC4CEB9FE1A85EC53
		h ^= h >> 33
	}
	h ^= uint64(len(data))
	h ^= h >> 33
	h *= 0xFF51AFD7ED558CCD
	h ^= h >> 33
	return h
}

// RecordCount returns the total number of top-level records.
func (r *Reader) RecordCount() int { return int(r.recordCount) }

// Slot resolves a key name to an integer slot handle for use with *BySlot
// accessors on Object. Panics if the key is not in the schema.
func (r *Reader) Slot(key string) int {
	s, ok := r.keyIndex[key]
	if !ok {
		panic(fmt.Sprintf("nxs: key %q not in schema", key))
	}
	return s
}

// Record returns a lazy view of the object at top-level index i.
// The object header is not parsed until a field accessor is called.
func (r *Reader) Record(i int) *Object {
	if i < 0 || i >= int(r.recordCount) {
		panic(fmt.Sprintf("nxs: record %d out of [0, %d)", i, r.recordCount))
	}
	if r.layout != LayoutRow {
		return &Object{reader: r, offset: i, recordIndex: uint32(i)}
	}
	abs := binary.LittleEndian.Uint64(r.data[r.tailStart+i*10+2 : r.tailStart+i*10+10])
	return &Object{reader: r, offset: int(abs)}
}

// ── Object ───────────────────────────────────────────────────────────────────

// Object is a lazy view over a single NXS object.
type Object struct {
	reader           *Reader
	offset           int
	recordIndex      uint32 // columnar/PAX record index
	stage            int    // 0=untouched, 1=bitmask located, 2=rank cached
	bitmaskStart     int
	offsetTableStart int
	// Stage 2 caches:
	present []uint8 // 1 byte per slot
	rank    []uint16
	// Stage 1 optimization: remember first slot accessed to detect 2nd call
	firstSlot int
}

func (o *Object) locateBitmask() error {
	if o.stage >= 1 {
		return nil
	}
	data := o.reader.data
	p := o.offset
	if p+8 > len(data) {
		return fmt.Errorf("ERR_OUT_OF_BOUNDS: object header")
	}
	if binary.LittleEndian.Uint32(data[p:p+4]) != magicObj {
		return fmt.Errorf("ERR_BAD_MAGIC: object at %d", p)
	}
	p += 8 // skip Magic + Length
	o.bitmaskStart = p
	// Walk past continuation bytes
	for {
		if p >= len(data) {
			return fmt.Errorf("ERR_OUT_OF_BOUNDS: bitmask")
		}
		b := data[p]
		p++
		if b&0x80 == 0 {
			break
		}
	}
	o.offsetTableStart = p
	o.firstSlot = -1
	o.stage = 1
	return nil
}

func (o *Object) buildRank() {
	if o.stage >= 2 {
		return
	}
	if o.stage < 1 {
		if err := o.locateBitmask(); err != nil {
			return
		}
	}
	keyCount := len(o.reader.Keys)
	o.present = make([]uint8, keyCount)
	o.rank = make([]uint16, keyCount+1)

	data := o.reader.data
	p := o.bitmaskStart
	slot := 0
	for slot < keyCount {
		b := data[p]
		p++
		bits := b & 0x7F
		for i := 0; i < 7 && slot < keyCount; i++ {
			o.present[slot] = (bits >> i) & 1
			slot++
		}
		if b&0x80 == 0 {
			break
		}
	}
	var acc uint16
	for i := 0; i < keyCount; i++ {
		o.rank[i] = acc
		acc += uint16(o.present[i])
	}
	o.rank[keyCount] = acc
	o.stage = 2
}

// inlineRank walks the bitmask from the start, returning (present, tableIdx)
// for slot s. Used on the first access — cheaper than building the full rank.
func (o *Object) inlineRank(slot int) (bool, int) {
	data := o.reader.data
	p := o.bitmaskStart
	cur := 0
	tableIdx := 0
	for {
		b := data[p]
		p++
		bits := b & 0x7F
		for i := 0; i < 7; i++ {
			if cur == slot {
				return (bits>>i)&1 == 1, tableIdx
			}
			if cur < slot && (bits>>i)&1 == 1 {
				tableIdx++
			}
			cur++
		}
		if b&0x80 == 0 {
			return false, 0
		}
	}
}

// resolveSlot returns the absolute byte offset of slot's value, or -1 if absent.
func (o *Object) resolveSlot(slot int) int {
	if o.stage == 2 {
		if o.present[slot] == 0 {
			return -1
		}
		rel := binary.LittleEndian.Uint16(o.reader.data[o.offsetTableStart+int(o.rank[slot])*2:])
		return o.offset + int(rel)
	}
	if o.stage == 0 {
		if err := o.locateBitmask(); err != nil {
			return -1
		}
	}
	// First-access path
	if o.firstSlot == -1 {
		o.firstSlot = slot
		present, idx := o.inlineRank(slot)
		if !present {
			return -1
		}
		rel := binary.LittleEndian.Uint16(o.reader.data[o.offsetTableStart+idx*2:])
		return o.offset + int(rel)
	}
	// Second distinct access → promote to rank cache
	if slot != o.firstSlot {
		o.buildRank()
		if o.present[slot] == 0 {
			return -1
		}
		rel := binary.LittleEndian.Uint16(o.reader.data[o.offsetTableStart+int(o.rank[slot])*2:])
		return o.offset + int(rel)
	}
	// Same slot again — re-walk inline
	present, idx := o.inlineRank(slot)
	if !present {
		return -1
	}
	rel := binary.LittleEndian.Uint16(o.reader.data[o.offsetTableStart+idx*2:])
	return o.offset + int(rel)
}

// ── Typed accessors ──────────────────────────────────────────────────────────

func (o *Object) GetI64(key string) (int64, bool) {
	slot, ok := o.reader.keyIndex[key]
	if !ok {
		return 0, false
	}
	return o.GetI64BySlot(slot)
}

func (o *Object) GetF64(key string) (float64, bool) {
	slot, ok := o.reader.keyIndex[key]
	if !ok {
		return 0, false
	}
	return o.GetF64BySlot(slot)
}

func (o *Object) GetBool(key string) (bool, bool) {
	slot, ok := o.reader.keyIndex[key]
	if !ok {
		return false, false
	}
	return o.GetBoolBySlot(slot)
}

func (o *Object) GetStr(key string) (string, bool) {
	slot, ok := o.reader.keyIndex[key]
	if !ok {
		return "", false
	}
	return o.GetStrBySlot(slot)
}

func (o *Object) GetI64BySlot(slot int) (int64, bool) {
	if o.reader.layout != LayoutRow {
		cell, ok := o.reader.colNumericBytes(o.recordIndex, slot)
		if !ok {
			return 0, false
		}
		return int64(binary.LittleEndian.Uint64(cell)), true
	}
	off := o.resolveSlot(slot)
	if off < 0 {
		return 0, false
	}
	return int64(binary.LittleEndian.Uint64(o.reader.data[off : off+8])), true
}

func (o *Object) GetF64BySlot(slot int) (float64, bool) {
	if o.reader.layout != LayoutRow {
		cell, ok := o.reader.colNumericBytes(o.recordIndex, slot)
		if !ok {
			return 0, false
		}
		return math.Float64frombits(binary.LittleEndian.Uint64(cell)), true
	}
	off := o.resolveSlot(slot)
	if off < 0 {
		return 0, false
	}
	return math.Float64frombits(binary.LittleEndian.Uint64(o.reader.data[off : off+8])), true
}

func (o *Object) GetBoolBySlot(slot int) (bool, bool) {
	if o.reader.layout != LayoutRow {
		cell, ok := o.reader.colNumericBytes(o.recordIndex, slot)
		if !ok {
			return false, false
		}
		return cell[0] != 0, true
	}
	off := o.resolveSlot(slot)
	if off < 0 {
		return false, false
	}
	return o.reader.data[off] != 0, true
}

func (o *Object) GetStrBySlot(slot int) (string, bool) {
	if o.reader.layout != LayoutRow {
		if slot < 0 || slot >= len(o.reader.KeySigils) || o.reader.KeySigils[slot] != '"' {
			return "", false
		}
		return o.reader.ColGetStr(o.reader.Keys[slot], o.recordIndex)
	}
	off := o.resolveSlot(slot)
	if off < 0 {
		return "", false
	}
	length := int(binary.LittleEndian.Uint32(o.reader.data[off : off+4]))
	return string(o.reader.data[off+4 : off+4+length]), true
}

// ── Bulk scan / reducers ─────────────────────────────────────────────────────

// scanOffset locates slot's byte offset in the object at objOffset without
// maintaining any per-object state. Used by bulk reducers where each record
// is touched exactly once.
func scanOffset(data []byte, objOffset, slot int) int {
	p := objOffset + 8 // skip Magic + Length
	cur := 0
	tableIdx := 0
	found := false
	var b byte
	for {
		b = data[p]
		p++
		bits := b & 0x7F
		for i := 0; i < 7; i++ {
			if cur == slot {
				if (bits>>i)&1 == 0 {
					return -1
				}
				found = true
			} else if cur < slot && (bits>>i)&1 == 1 {
				tableIdx++
			}
			cur++
		}
		if found && b&0x80 == 0 {
			break
		}
		if cur > slot && found {
			break
		}
		if b&0x80 == 0 {
			return -1
		}
	}
	// Skip remaining continuation bytes
	for b&0x80 != 0 {
		b = data[p]
		p++
	}
	rel := binary.LittleEndian.Uint16(data[p+tableIdx*2:])
	return objOffset + int(rel)
}

// SumF64 returns the sum of the f64 field `key` across every top-level record.
// Allocation-free hot loop.
func (r *Reader) SumF64(key string) float64 {
	if r.layout != LayoutRow {
		return r.ColSumF64(key)
	}
	slot, ok := r.keyIndex[key]
	if !ok {
		panic("nxs: key not in schema")
	}
	data := r.data
	tail := r.tailStart
	n := int(r.recordCount)
	var sum float64
	for i := 0; i < n; i++ {
		abs := int(binary.LittleEndian.Uint64(data[tail+i*10+2 : tail+i*10+10]))
		off := scanOffset(data, abs, slot)
		if off < 0 {
			continue
		}
		sum += math.Float64frombits(binary.LittleEndian.Uint64(data[off : off+8]))
	}
	return sum
}

// SumI64 returns the sum of the i64 field `key` across every record.
func (r *Reader) SumI64(key string) int64 {
	slot, ok := r.keyIndex[key]
	if !ok {
		panic("nxs: key not in schema")
	}
	data := r.data
	tail := r.tailStart
	n := int(r.recordCount)
	var sum int64
	for i := 0; i < n; i++ {
		abs := int(binary.LittleEndian.Uint64(data[tail+i*10+2 : tail+i*10+10]))
		off := scanOffset(data, abs, slot)
		if off < 0 {
			continue
		}
		sum += int64(binary.LittleEndian.Uint64(data[off : off+8]))
	}
	return sum
}

// MinF64 returns the minimum and whether at least one value was present.
func (r *Reader) MinF64(key string) (float64, bool) {
	slot, ok := r.keyIndex[key]
	if !ok {
		panic("nxs: key not in schema")
	}
	data := r.data
	tail := r.tailStart
	n := int(r.recordCount)
	var m float64
	have := false
	for i := 0; i < n; i++ {
		abs := int(binary.LittleEndian.Uint64(data[tail+i*10+2 : tail+i*10+10]))
		off := scanOffset(data, abs, slot)
		if off < 0 {
			continue
		}
		v := math.Float64frombits(binary.LittleEndian.Uint64(data[off : off+8]))
		if !have || v < m {
			m = v
			have = true
		}
	}
	return m, have
}

// MaxF64 returns the maximum and whether at least one value was present.
func (r *Reader) MaxF64(key string) (float64, bool) {
	slot, ok := r.keyIndex[key]
	if !ok {
		panic("nxs: key not in schema")
	}
	data := r.data
	tail := r.tailStart
	n := int(r.recordCount)
	var m float64
	have := false
	for i := 0; i < n; i++ {
		abs := int(binary.LittleEndian.Uint64(data[tail+i*10+2 : tail+i*10+10]))
		off := scanOffset(data, abs, slot)
		if off < 0 {
			continue
		}
		v := math.Float64frombits(binary.LittleEndian.Uint64(data[off : off+8]))
		if !have || v > m {
			m = v
			have = true
		}
	}
	return m, have
}
