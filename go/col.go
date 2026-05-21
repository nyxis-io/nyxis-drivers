// Columnar and PAX layout read paths (OLAP.md).
package nxs

import (
	"encoding/binary"
	"fmt"
	"math"
)

const (
	flagColumnar uint16 = 0x0001
	flagPAX      uint16 = 0x0004

	footerRowBytes = 12
	footerColBytes = 20
	footerPaxBytes = 28

	colTailEntryBytes = 20
)

// Layout identifies the data-sector organization.
type Layout int

const (
	LayoutRow Layout = iota
	LayoutColumnar
	LayoutPAX
)

func (r *Reader) parseLayoutTail() error {
	if r.Flags&flagColumnar != 0 && r.Flags&flagPAX != 0 {
		return fmt.Errorf("ERR_INVALID_FLAGS: columnar and PAX both set")
	}
	if r.Flags&flagColumnar != 0 && r.TailPtr == 0 {
		return fmt.Errorf("ERR_INCOMPATIBLE_FLAGS: columnar with TailPtr=0")
	}

	if r.Flags&flagColumnar != 0 {
		r.layout = LayoutColumnar
		return r.parseColumnarFooter()
	}
	if r.Flags&flagPAX != 0 {
		r.layout = LayoutPAX
		return r.parsePAXFooter()
	}
	r.layout = LayoutRow
	if r.TailPtr == 0 {
		if len(r.data) < 44 {
			return fmt.Errorf("ERR_OUT_OF_BOUNDS: streamable footer")
		}
		r.TailPtr = binary.LittleEndian.Uint64(r.data[len(r.data)-footerRowBytes : len(r.data)-4])
	}
	if int(r.TailPtr)+4 > len(r.data) {
		return fmt.Errorf("ERR_OUT_OF_BOUNDS: tail index")
	}
	r.recordCount = binary.LittleEndian.Uint32(r.data[r.TailPtr : r.TailPtr+4])
	r.tailStart = int(r.TailPtr) + 4
	return nil
}

func (r *Reader) parseColumnarFooter() error {
	if len(r.data) < footerColBytes {
		return fmt.Errorf("ERR_OUT_OF_BOUNDS: columnar footer")
	}
	fo := len(r.data) - footerColBytes
	r.TailPtr = binary.LittleEndian.Uint64(r.data[fo : fo+8])
	r.recordCount = uint32(binary.LittleEndian.Uint64(r.data[fo+8 : fo+16]))
	r.tailStart = int(r.TailPtr)
	kc := len(r.Keys)
	r.colBufOff = make([]uint64, kc)
	r.colBufLen = make([]uint64, kc)
	for i := 0; i < kc; i++ {
		e := r.tailStart + i*colTailEntryBytes
		if e+colTailEntryBytes > len(r.data) {
			return fmt.Errorf("ERR_OUT_OF_BOUNDS: columnar tail entry")
		}
		r.colBufOff[i] = binary.LittleEndian.Uint64(r.data[e+4 : e+12])
		r.colBufLen[i] = binary.LittleEndian.Uint64(r.data[e+12 : e+20])
	}
	return nil
}

func (r *Reader) parsePAXFooter() error {
	if len(r.data) < footerPaxBytes {
		return fmt.Errorf("ERR_OUT_OF_BOUNDS: PAX footer")
	}
	fo := len(r.data) - footerPaxBytes
	r.TailPtr = binary.LittleEndian.Uint64(r.data[fo : fo+8])
	r.recordCount = uint32(binary.LittleEndian.Uint64(r.data[fo+8 : fo+16]))
	r.pageCount = binary.LittleEndian.Uint32(r.data[fo+16 : fo+20])
	r.pageSizeHint = binary.LittleEndian.Uint32(r.data[fo+20 : fo+24])
	r.tailStart = int(r.TailPtr)
	if r.pageCount > 0 {
		r.pageIndex = make([]uint32, r.pageCount)
		r.pageRecStart = make([]uint64, r.pageCount)
		r.pageRecCount = make([]uint32, r.pageCount)
		r.pageOffset = make([]uint64, r.pageCount)
		r.pageLength = make([]uint32, r.pageCount)
		for i := uint32(0); i < r.pageCount; i++ {
			e := r.tailStart + int(i)*28
			if e+28 > len(r.data) {
				return fmt.Errorf("ERR_OUT_OF_BOUNDS: PAX tail entry")
			}
			r.pageIndex[i] = binary.LittleEndian.Uint32(r.data[e:])
			r.pageRecStart[i] = binary.LittleEndian.Uint64(r.data[e+4 : e+12])
			r.pageRecCount[i] = binary.LittleEndian.Uint32(r.data[e+12 : e+16])
			r.pageOffset[i] = binary.LittleEndian.Uint64(r.data[e+16 : e+24])
			r.pageLength[i] = binary.LittleEndian.Uint32(r.data[e+24 : e+28])
		}
	}
	return nil
}

func nullBitmapBytes(n uint32) int {
	raw := int((n + 7) / 8)
	return (raw + 7) &^ 7
}

func colBit(bm []byte, rec uint32) bool {
	return (bm[rec/8]>>(rec%8))&1 == 1
}

func (r *Reader) colFieldParts(slot int) (bm []byte, vals []byte, err error) {
	if slot < 0 || slot >= len(r.colBufOff) {
		return nil, nil, fmt.Errorf("ERR_KEY_NOT_FOUND")
	}
	off := int(r.colBufOff[slot])
	length := int(r.colBufLen[slot])
	if off+length > len(r.data) {
		return nil, nil, fmt.Errorf("ERR_OUT_OF_BOUNDS: column buffer")
	}
	bmLen := nullBitmapBytes(r.recordCount)
	if length < bmLen {
		return nil, nil, fmt.Errorf("ERR_OUT_OF_BOUNDS: null bitmap")
	}
	bm = r.data[off : off+bmLen]
	vals = r.data[off+bmLen : off+length]
	return bm, vals, nil
}

// ColSumF64 sums a float64 column using the columnar/PAX buffer layout.
func (r *Reader) ColSumF64(key string) float64 {
	slot, ok := r.keyIndex[key]
	if !ok {
		panic("nxs: key not in schema")
	}
	if r.layout == LayoutRow {
		return r.SumF64(key)
	}
	if r.layout == LayoutPAX {
		return r.paxSumF64(slot)
	}
	bm, vals, err := r.colFieldParts(slot)
	if err != nil {
		return 0
	}
	n := int(r.recordCount)
	var sum float64
	for i := 0; i < n; i++ {
		if !colBit(bm, uint32(i)) {
			continue
		}
		off := i * 8
		if off+8 > len(vals) {
			break
		}
		sum += math.Float64frombits(binary.LittleEndian.Uint64(vals[off : off+8]))
	}
	return sum
}

// ColBuffer returns the raw value bytes for a column (columnar/PAX only).
func (r *Reader) ColBuffer(key string) ([]byte, bool) {
	slot, ok := r.keyIndex[key]
	if !ok || r.layout == LayoutRow {
		return nil, false
	}
	_, vals, err := r.colFieldParts(slot)
	if err != nil {
		return nil, false
	}
	return vals, true
}

// ColNullBitmap returns the null bitmap for a column (columnar/PAX only).
func (r *Reader) ColNullBitmap(key string) ([]byte, bool) {
	slot, ok := r.keyIndex[key]
	if !ok || r.layout == LayoutRow {
		return nil, false
	}
	bm, _, err := r.colFieldParts(slot)
	if err != nil {
		return nil, false
	}
	return bm, true
}

// LayoutKind reports row, columnar, or PAX organization.
func (r *Reader) LayoutKind() Layout {
	return r.layout
}

func (r *Reader) paxSumF64(slot int) float64 {
	var sum float64
	for pi := uint32(0); pi < r.pageCount; pi++ {
		bm, vals, ok := r.pageFieldParts(pi, slot)
		if !ok {
			continue
		}
		rc := r.pageRecCount[pi]
		for i := uint32(0); i < rc; i++ {
			if !colBit(bm, i) {
				continue
			}
			off := int(i) * 8
			if off+8 > len(vals) {
				break
			}
			sum += math.Float64frombits(binary.LittleEndian.Uint64(vals[off : off+8]))
		}
	}
	return sum
}

func (r *Reader) pageFieldParts(pi uint32, slot int) ([]byte, []byte, bool) {
	const magicPage uint32 = 0x4E585350
	poff := int(r.pageOffset[pi])
	if poff+24 > len(r.data) || binary.LittleEndian.Uint32(r.data[poff:]) != magicPage {
		return nil, nil, false
	}
	rc := int(r.pageRecCount[pi])
	body := poff + 24
	for fi := 0; fi < slot; fi++ {
		bmLen := nullBitmapBytes(r.pageRecCount[pi])
		body += bmLen + rc*8
	}
	bmLen := nullBitmapBytes(r.pageRecCount[pi])
	if body+bmLen+rc*8 > len(r.data) {
		return nil, nil, false
	}
	return r.data[body : body+bmLen], r.data[body+bmLen : body+bmLen+rc*8], true
}

// colSumF64Dense sums a dense columnar f64 buffer (skips null-bit checks when bitmap is all-ones).
func (r *Reader) colSumF64Dense(key string) float64 {
	slot, ok := r.keyIndex[key]
	if !ok {
		panic("nxs: key not in schema")
	}
	_, vals, err := r.colFieldParts(slot)
	if err != nil {
		return 0
	}
	n := int(r.recordCount)
	var sum float64
	for i := 0; i < n; i++ {
		off := i * 8
		if off+8 > len(vals) {
			break
		}
		sum += math.Float64frombits(binary.LittleEndian.Uint64(vals[off : off+8]))
	}
	return sum
}
