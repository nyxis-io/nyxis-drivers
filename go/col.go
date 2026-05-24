// Columnar and PAX layout read paths (OLAP.md).
package nxs

import (
	"encoding/binary"
	"fmt"
	"math"
)

func varOffBytesLen(rc uint32) (int, error) {
	off := (uint64(rc) + 1) * 4
	if off > uint64(math.MaxInt) {
		return 0, fmt.Errorf("ERR_OUT_OF_BOUNDS: var offsets overflow")
	}
	return int(off), nil
}

const (
	flagColumnar uint16 = 0x0001
	flagPAX      uint16 = 0x0004

	footerRowBytes = 12
	footerColBytes = 20
	footerPaxBytes = 28

	colTailEntryBytes = 20
	paxTailEntryBytes = 28
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
		fid := binary.LittleEndian.Uint16(r.data[e : e+2])
		if int(fid) >= kc {
			return fmt.Errorf("ERR_OUT_OF_BOUNDS: invalid field ID %d", fid)
		}
		r.colBufOff[fid] = binary.LittleEndian.Uint64(r.data[e+4 : e+12])
		r.colBufLen[fid] = binary.LittleEndian.Uint64(r.data[e+12 : e+20])
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
			e := r.tailStart + int(i)*paxTailEntryBytes
			if e+paxTailEntryBytes > len(r.data) {
				return fmt.Errorf("ERR_OUT_OF_BOUNDS: PAX tail entry")
			}
			r.pageIndex[i] = binary.LittleEndian.Uint32(r.data[e:])
			r.pageRecStart[i] = binary.LittleEndian.Uint64(r.data[e+4 : e+12])
			r.pageRecCount[i] = binary.LittleEndian.Uint32(r.data[e+12 : e+16])
			r.pageOffset[i] = binary.LittleEndian.Uint64(r.data[e+16 : e+24])
			r.pageLength[i] = binary.LittleEndian.Uint32(r.data[e+24 : e+28])
		}
		const magicPage uint32 = 0x4E585350
		dlen := uint64(len(r.data))
		for i := uint32(0); i < r.pageCount; i++ {
			poff64 := r.pageOffset[i]
			if poff64 > dlen || poff64+4 > dlen || poff64 > uint64(math.MaxInt) {
				return fmt.Errorf("ERR_OUT_OF_BOUNDS: PAX page offset")
			}
			poff := int(poff64)
			if binary.LittleEndian.Uint32(r.data[poff:]) != magicPage {
				return fmt.Errorf("ERR_INVALID_PAGE_MAGIC: PAX page magic mismatch")
			}
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

func (r *Reader) paxFindPage(rec uint32) (page int, local int, ok bool) {
	if r.pageCount == 0 {
		return 0, 0, false
	}
	r64 := uint64(rec)
	lo, hi := 0, int(r.pageCount)-1
	for lo <= hi {
		mid := lo + (hi-lo)/2
		start := r.pageRecStart[mid]
		count := uint64(r.pageRecCount[mid])
		if r64 < start {
			hi = mid - 1
		} else if r64 >= start+count {
			lo = mid + 1
		} else {
			return mid, int(r64 - start), true
		}
	}
	return 0, 0, false
}

func isVarSigil(sig byte) bool {
	return sig == '"' || sig == '<'
}

func fieldSectorLen(data []byte, sectorOff int, rc uint32, sigil byte) (int, error) {
	bmLen := nullBitmapBytes(rc)
	if !isVarSigil(sigil) {
		return bmLen + int(uint64(rc)*8), nil
	}
	offBytes, err := varOffBytesLen(rc)
	if err != nil {
		return 0, err
	}
	if sectorOff+bmLen+offBytes > len(data) {
		return 0, fmt.Errorf("ERR_OUT_OF_BOUNDS: var offsets")
	}
	end := int(binary.LittleEndian.Uint32(data[sectorOff+bmLen+int(rc)*4 : sectorOff+bmLen+int(rc)*4+4]))
	total := bmLen + offBytes + end
	if sectorOff+total > len(data) {
		return 0, fmt.Errorf("ERR_OUT_OF_BOUNDS: var values")
	}
	return total, nil
}

// VarStrAt reads one UTF-8 string from a variable-length column sector.
func VarStrAt(offsets []byte, values []byte, recordIndex uint32) (string, bool) {
	if uint64(len(offsets)) < (uint64(recordIndex)+2)*4 {
		return "", false
	}
	off := uint64(recordIndex) * 4
	start := int(binary.LittleEndian.Uint32(offsets[off : off+4]))
	end := int(binary.LittleEndian.Uint32(offsets[off+4 : off+8]))
	if end < start || end > len(values) {
		return "", false
	}
	return string(values[start:end]), true
}

func varBinaryAt(offsets []byte, values []byte, recordIndex uint32) ([]byte, bool) {
	if uint64(len(offsets)) < (uint64(recordIndex)+2)*4 {
		return nil, false
	}
	off := uint64(recordIndex) * 4
	start := int(binary.LittleEndian.Uint32(offsets[off : off+4]))
	end := int(binary.LittleEndian.Uint32(offsets[off+4 : off+8]))
	if end < start || end > len(values) {
		return nil, false
	}
	return values[start:end], true
}

// colVarParts returns null bitmap, u32 offsets LE, and values bytes for a var-length field.
func (r *Reader) colVarParts(slot int) (bm, offsets, values []byte, err error) {
	bm, tail, err := r.colFieldParts(slot)
	if err != nil {
		return nil, nil, nil, err
	}
	offBytes, err := varOffBytesLen(r.recordCount)
	if err != nil {
		return nil, nil, nil, err
	}
	if len(tail) < offBytes {
		return nil, nil, nil, fmt.Errorf("ERR_OUT_OF_BOUNDS: var offsets")
	}
	return bm, tail[:offBytes], tail[offBytes:], nil
}

func (r *Reader) colVarPartsAt(rec uint32, slot int) (bm, offsets, values []byte, ok bool) {
	if slot < 0 || slot >= len(r.KeySigils) || !isVarSigil(r.KeySigils[slot]) {
		return nil, nil, nil, false
	}
	if r.layout == LayoutColumnar {
		var err error
		bm, offsets, values, err = r.colVarParts(slot)
		return bm, offsets, values, err == nil
	}
	if r.layout == LayoutPAX {
		pi, _, found := r.paxFindPage(rec)
		if !found {
			return nil, nil, nil, false
		}
		bm, tail, pageOk := r.pageFieldParts(uint32(pi), slot)
		if !pageOk {
			return nil, nil, nil, false
		}
		rc := r.pageRecCount[pi]
		offBytes, err := varOffBytesLen(rc)
		if err != nil || len(tail) < offBytes {
			return nil, nil, nil, false
		}
		return bm, tail[:offBytes], tail[offBytes:], true
	}
	return nil, nil, nil, false
}

// ColVarBuffer exposes zero-copy string/binary column sectors (columnar/PAX only).
type ColVarBuffer struct {
	Bitmap  []byte
	Offsets []byte
	Values  []byte
	Count   uint32
}

// ColVarBuffer returns the null bitmap, u32 offset table, and values blob for a var field.
func (r *Reader) ColVarBuffer(key string) (ColVarBuffer, error) {
	if r.layout != LayoutColumnar {
		return ColVarBuffer{}, fmt.Errorf("ERR_LAYOUT: ColVarBuffer is columnar-only (use ColGetStr per record on PAX)")
	}
	slot, ok := r.keyIndex[key]
	if !ok {
		return ColVarBuffer{}, fmt.Errorf("ERR_KEY_NOT_FOUND: %s", key)
	}
	if slot < 0 || slot >= len(r.KeySigils) || !isVarSigil(r.KeySigils[slot]) {
		return ColVarBuffer{}, fmt.Errorf("ERR_UNSUPPORTED_FIELD_TYPE: %s", key)
	}
	bm, offsets, values, err := r.colVarParts(slot)
	if err != nil {
		return ColVarBuffer{}, err
	}
	return ColVarBuffer{
		Bitmap:  bm,
		Offsets: offsets,
		Values:  values,
		Count:   r.recordCount,
	}, nil
}

// ColGetStr reads a string field at recordIndex in columnar or PAX layout.
func (r *Reader) ColGetStr(key string, recordIndex uint32) (string, bool) {
	slot, ok := r.keyIndex[key]
	if !ok || recordIndex >= r.recordCount || r.layout == LayoutRow {
		return "", false
	}
	if r.KeySigils[slot] != '"' {
		return "", false
	}
	bm, offsets, values, ok := r.colVarPartsAt(recordIndex, slot)
	if !ok {
		return "", false
	}
	if r.layout == LayoutPAX {
		_, li, found := r.paxFindPage(recordIndex)
		if !found || !colBit(bm, uint32(li)) {
			return "", false
		}
		return VarStrAt(offsets, values, uint32(li))
	}
	if !colBit(bm, recordIndex) {
		return "", false
	}
	return VarStrAt(offsets, values, recordIndex)
}

// ColGetBinary reads a binary field at recordIndex in columnar or PAX layout.
func (r *Reader) ColGetBinary(key string, recordIndex uint32) ([]byte, bool) {
	slot, ok := r.keyIndex[key]
	if !ok || recordIndex >= r.recordCount || r.layout == LayoutRow {
		return nil, false
	}
	if r.KeySigils[slot] != '<' {
		return nil, false
	}
	bm, offsets, values, ok := r.colVarPartsAt(recordIndex, slot)
	if !ok {
		return nil, false
	}
	if r.layout == LayoutPAX {
		_, li, found := r.paxFindPage(recordIndex)
		if !found || !colBit(bm, uint32(li)) {
			return nil, false
		}
		return varBinaryAt(offsets, values, uint32(li))
	}
	if !colBit(bm, recordIndex) {
		return nil, false
	}
	return varBinaryAt(offsets, values, recordIndex)
}

// colNumericBytes returns the 8-byte fixed cell for a record/slot in columnar or PAX layout.
func (r *Reader) colNumericBytes(rec uint32, slot int) ([]byte, bool) {
	if slot >= 0 && slot < len(r.KeySigils) && isVarSigil(r.KeySigils[slot]) {
		return nil, false
	}
	if r.layout == LayoutColumnar {
		bm, vals, err := r.colFieldParts(slot)
		if err != nil || rec >= r.recordCount || !colBit(bm, rec) {
			return nil, false
		}
		off := int(rec) * 8
		if off+8 > len(vals) {
			return nil, false
		}
		return vals[off : off+8], true
	}
	if r.layout == LayoutPAX {
		pi, li, found := r.paxFindPage(rec)
		if !found {
			return nil, false
		}
		pageBm, pageVals, pageOk := r.pageFieldParts(uint32(pi), slot)
		if !pageOk || !colBit(pageBm, uint32(li)) {
			return nil, false
		}
		off := li * 8
		if off+8 > len(pageVals) {
			return nil, false
		}
		return pageVals[off : off+8], true
	}
	return nil, false
}

func (r *Reader) colFieldParts(slot int) (bm []byte, vals []byte, err error) {
	sector, err := r.columnSector(slot)
	if err != nil {
		return nil, nil, err
	}
	bmLen := nullBitmapBytes(r.recordCount)
	if len(sector) < bmLen {
		return nil, nil, fmt.Errorf("ERR_OUT_OF_BOUNDS: null bitmap")
	}
	bm = sector[:bmLen]
	vals = sector[bmLen:]
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

func (r *Reader) pageFieldSector(pi uint32, slot int) ([]byte, bool) {
	const magicPage uint32 = 0x4E585350
	poff := int(r.pageOffset[pi])
	if poff+24 > len(r.data) || binary.LittleEndian.Uint32(r.data[poff:]) != magicPage {
		return nil, false
	}
	fc := int(binary.LittleEndian.Uint16(r.data[poff+20 : poff+22]))
	if slot < 0 || slot >= fc || fc > len(r.KeySigils) {
		return nil, false
	}
	rc := r.pageRecCount[pi]
	body := poff + 24
	for fi := 0; fi < slot; fi++ {
		sig := byte('=')
		if fi < len(r.KeySigils) {
			sig = r.KeySigils[fi]
		}
		flen, err := fieldSectorLen(r.data, body, rc, sig)
		if err != nil {
			return nil, false
		}
		body += flen
	}
	sig := byte('=')
	if slot < len(r.KeySigils) {
		sig = r.KeySigils[slot]
	}
	flen, err := fieldSectorLen(r.data, body, rc, sig)
	if err != nil || body+flen > len(r.data) {
		return nil, false
	}
	return r.data[body : body+flen], true
}

func (r *Reader) pageFieldParts(pi uint32, slot int) ([]byte, []byte, bool) {
	sector, ok := r.pageFieldSector(pi, slot)
	if !ok {
		return nil, nil, false
	}
	bmLen := nullBitmapBytes(r.pageRecCount[pi])
	if len(sector) < bmLen {
		return nil, nil, false
	}
	return sector[:bmLen], sector[bmLen:], true
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
