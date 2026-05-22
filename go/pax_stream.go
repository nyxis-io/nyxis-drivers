package nxs

import (
	"encoding/binary"
	"fmt"
	"math"
)

const magicPage uint32 = 0x4E585350 // NXSP

func addU64(a, b uint64) (uint64, bool) {
	if a > math.MaxUint64-b {
		return 0, false
	}
	return a + b, true
}

func mulU64(a, b uint64) (uint64, bool) {
	if a != 0 && b > math.MaxUint64/a {
		return 0, false
	}
	return a * b, true
}

func nullBitmapBytesU64(n uint32) uint64 {
	raw := (uint64(n) + 7) / 8
	return (raw + 7) &^ 7
}

// paxPageLenAligned returns wire page length and 8-byte-aligned span, or ok=false.
func paxPageLenAligned(data []byte, off int, fieldCount uint16) (pageLen, aligned uint64, ok bool) {
	if off < 0 || fieldCount == 0 || off+28 > len(data) {
		return 0, 0, false
	}
	if binary.LittleEndian.Uint32(data[off:]) != magicPage {
		return 0, 0, false
	}
	rc := binary.LittleEndian.Uint32(data[off+16 : off+20])
	bl := nullBitmapBytesU64(rc)
	cells, ok := mulU64(uint64(rc), 8)
	if !ok {
		return 0, 0, false
	}
	stride, ok := addU64(bl, cells)
	if !ok {
		return 0, 0, false
	}
	fields, ok := mulU64(stride, uint64(fieldCount))
	if !ok {
		return 0, 0, false
	}
	body, ok := addU64(24, fields)
	if !ok {
		return 0, 0, false
	}
	pageLen, ok = addU64(body, 4)
	if !ok {
		return 0, 0, false
	}
	aligned = (pageLen + 7) &^ 7
	if pageLen > uint64(len(data)) || aligned > uint64(len(data)) {
		return 0, 0, false
	}
	if pageLen > uint64(math.MaxInt) || aligned > uint64(math.MaxInt) {
		return 0, 0, false
	}
	plen := int(pageLen)
	aln := int(aligned)
	if off+plen > len(data) || off+aln > len(data) {
		return 0, 0, false
	}
	if binary.LittleEndian.Uint32(data[off+plen-4:off+plen]) != uint32(pageLen) {
		return 0, 0, false
	}
	return pageLen, aligned, true
}

// PaxStreamReader reads unsealed PAX streams (preamble TailPtr=0) by polling for
// complete NXSP pages before the file footer is present (OLAP.md §4.5).
type PaxStreamReader struct {
	data      []byte
	Version   uint16
	Flags     uint16
	DictHash  uint64
	Keys      []string
	KeySigils []byte
	keyIndex  map[string]int

	dataStart        int
	scanCursor       int
	sealed           bool
	pageCount        uint32
	recordsAvailable uint64
	pageIndex        []uint32
	pageRecStart     []uint64
	pageRecCount     []uint32
	pageOffset       []uint64
	pageLength       []uint32
}

// PaxCompletePageAt returns the 8-byte-aligned length of a complete NXSP page at off,
// or 0 if the page is not yet fully present. fieldCount must match the schema.
func PaxCompletePageAt(data []byte, off int, fieldCount uint16) int {
	_, aligned, ok := paxPageLenAligned(data, off, fieldCount)
	if !ok {
		return 0
	}
	return int(aligned)
}

// OpenPaxStream opens FLAG_PAX with preamble TailPtr=0 (growing / unsealed file).
func OpenPaxStream(data []byte) (*PaxStreamReader, error) {
	if len(data) < 32 {
		return nil, fmt.Errorf("ERR_OUT_OF_BOUNDS: file too small")
	}
	if binary.LittleEndian.Uint32(data[0:4]) != magicFile {
		return nil, fmt.Errorf("ERR_BAD_MAGIC: preamble")
	}
	flags := binary.LittleEndian.Uint16(data[6:8])
	if flags&flagPAX == 0 {
		return nil, fmt.Errorf("ERR_INVALID_FLAGS: not PAX")
	}
	if binary.LittleEndian.Uint64(data[16:24]) != 0 {
		return nil, fmt.Errorf("ERR_INCOMPATIBLE_FLAGS: PAX stream requires TailPtr=0")
	}
	sr := &PaxStreamReader{data: data}
	sr.Version = binary.LittleEndian.Uint16(data[4:6])
	sr.Flags = flags
	sr.DictHash = binary.LittleEndian.Uint64(data[8:16])
	if flags&flagSchemaEmbedded == 0 {
		return nil, fmt.Errorf("ERR_OUT_OF_BOUNDS: PAX stream requires embedded schema")
	}
	schemaEnd, err := sr.readSchema(32)
	if err != nil {
		return nil, err
	}
	if murmur3_64(data[32:schemaEnd]) != sr.DictHash {
		return nil, fmt.Errorf("ERR_DICT_MISMATCH: schema hash mismatch")
	}
	sr.dataStart = schemaEnd
	sr.scanCursor = schemaEnd
	if tailOff, ok := paxStreamDetectSealed(data); ok {
		if err := sr.loadSealedTail(tailOff); err != nil {
			return nil, err
		}
	}
	return sr, nil
}

func (sr *PaxStreamReader) readSchema(offset int) (int, error) {
	if offset+2 > len(sr.data) {
		return 0, fmt.Errorf("ERR_OUT_OF_BOUNDS: schema header")
	}
	keyCount := int(binary.LittleEndian.Uint16(sr.data[offset : offset+2]))
	offset += 2
	if offset+keyCount > len(sr.data) {
		return 0, fmt.Errorf("ERR_OUT_OF_BOUNDS: type manifest")
	}
	sr.KeySigils = make([]byte, keyCount)
	copy(sr.KeySigils, sr.data[offset:offset+keyCount])
	offset += keyCount
	sr.Keys = make([]string, 0, keyCount)
	sr.keyIndex = make(map[string]int, keyCount)
	for i := 0; i < keyCount; i++ {
		start := offset
		for offset < len(sr.data) && sr.data[offset] != 0 {
			offset++
		}
		if offset >= len(sr.data) {
			return 0, fmt.Errorf("ERR_OUT_OF_BOUNDS: string pool")
		}
		sr.Keys = append(sr.Keys, string(sr.data[start:offset]))
		sr.keyIndex[sr.Keys[i]] = i
		offset++
	}
	end := (offset + 7) &^ 7
	return end, nil
}

func paxStreamDetectSealed(data []byte) (uint64, bool) {
	if len(data) < footerPaxBytes {
		return 0, false
	}
	if binary.LittleEndian.Uint32(data[len(data)-4:]) != magicFooter {
		return 0, false
	}
	tp := binary.LittleEndian.Uint64(data[len(data)-footerPaxBytes : len(data)-footerPaxBytes+8])
	dlen := uint64(len(data))
	if tp == 0 || tp >= dlen || dlen-tp < uint64(footerPaxBytes) || tp > uint64(math.MaxInt) {
		return 0, false
	}
	return tp, true
}

func (sr *PaxStreamReader) loadSealedTail(tailOff uint64) error {
	sr.pageIndex = nil
	sr.pageRecStart = nil
	sr.pageRecCount = nil
	sr.pageOffset = nil
	sr.pageLength = nil
	sr.pageCount = 0
	sr.recordsAvailable = 0
	fo := len(sr.data) - footerPaxBytes
	pc := binary.LittleEndian.Uint32(sr.data[fo+16 : fo+20])
	sr.scanCursor = len(sr.data)
	sr.sealed = true
	if pc == 0 {
		return nil
	}
	sr.pageIndex = make([]uint32, pc)
	sr.pageRecStart = make([]uint64, pc)
	sr.pageRecCount = make([]uint32, pc)
	sr.pageOffset = make([]uint64, pc)
	sr.pageLength = make([]uint32, pc)
	dlen := uint64(len(sr.data))
	for i := uint32(0); i < pc; i++ {
		e := tailOff + uint64(i)*uint64(paxTailEntryBytes)
		if e > dlen || e+uint64(paxTailEntryBytes) > dlen || e > uint64(math.MaxInt) {
			return fmt.Errorf("ERR_OUT_OF_BOUNDS: PAX tail entry")
		}
		ee := int(e)
		sr.pageIndex[i] = binary.LittleEndian.Uint32(sr.data[ee:])
		sr.pageRecStart[i] = binary.LittleEndian.Uint64(sr.data[ee+4 : ee+12])
		sr.pageRecCount[i] = binary.LittleEndian.Uint32(sr.data[ee+12 : ee+16])
		sr.pageOffset[i] = binary.LittleEndian.Uint64(sr.data[ee+16 : ee+24])
		sr.pageLength[i] = binary.LittleEndian.Uint32(sr.data[ee+24 : ee+28])
		sr.recordsAvailable += uint64(sr.pageRecCount[i])
	}
	sr.pageCount = pc
	return nil
}

// Poll scans for newly complete pages after the buffer grows.
func (sr *PaxStreamReader) Poll() uint32 {
	before := sr.pageCount
	if !sr.sealed {
		if tailOff, ok := paxStreamDetectSealed(sr.data); ok {
			_ = sr.loadSealedTail(tailOff)
			return sr.pageCount - before
		}
	}
	if sr.sealed {
		return 0
	}
	fc := uint16(len(sr.Keys))
	for sr.scanCursor+28 <= len(sr.data) {
		if binary.LittleEndian.Uint32(sr.data[sr.scanCursor:]) != magicPage {
			break
		}
		plen := PaxCompletePageAt(sr.data, sr.scanCursor, fc)
		if plen == 0 {
			break
		}
		pidx := binary.LittleEndian.Uint32(sr.data[sr.scanCursor+4:])
		rstart := binary.LittleEndian.Uint64(sr.data[sr.scanCursor+8:])
		rc := binary.LittleEndian.Uint32(sr.data[sr.scanCursor+16:])
		pageLen, _, ok := paxPageLenAligned(sr.data, sr.scanCursor, fc)
		if !ok {
			break
		}
		sr.pageIndex = append(sr.pageIndex, pidx)
		sr.pageRecStart = append(sr.pageRecStart, rstart)
		sr.pageRecCount = append(sr.pageRecCount, rc)
		sr.pageOffset = append(sr.pageOffset, uint64(sr.scanCursor))
		sr.pageLength = append(sr.pageLength, uint32(pageLen))
		sr.pageCount++
		sr.recordsAvailable += uint64(rc)
		sr.scanCursor += plen
	}
	return sr.pageCount - before
}

func (sr *PaxStreamReader) IsSealed() bool { return sr.sealed }

func (sr *PaxStreamReader) RecordsAvailable() uint64 { return sr.recordsAvailable }

// ColSumF64 sums a field across records in complete pages only.
func (sr *PaxStreamReader) ColSumF64(key string) float64 {
	slot, ok := sr.keyIndex[key]
	if !ok {
		return 0
	}
	var sum float64
	for pi := uint32(0); pi < sr.pageCount; pi++ {
		bm, vals, ok := sr.streamPageFieldParts(pi, slot)
		if !ok {
			continue
		}
		rc := sr.pageRecCount[pi]
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

func (sr *PaxStreamReader) streamPageFieldParts(pi uint32, slot int) ([]byte, []byte, bool) {
	poff64 := sr.pageOffset[pi]
	dlen := uint64(len(sr.data))
	if poff64 > dlen || poff64+24 > dlen || poff64 > uint64(math.MaxInt) {
		return nil, nil, false
	}
	poff := int(poff64)
	if binary.LittleEndian.Uint32(sr.data[poff:]) != magicPage {
		return nil, nil, false
	}
	fc := int(binary.LittleEndian.Uint16(sr.data[poff+20 : poff+22]))
	if slot < 0 || slot >= fc {
		return nil, nil, false
	}
	rc := sr.pageRecCount[pi]
	bl := nullBitmapBytesU64(rc)
	cells, ok := mulU64(uint64(rc), 8)
	if !ok {
		return nil, nil, false
	}
	stride, ok := addU64(bl, cells)
	if !ok {
		return nil, nil, false
	}
	skip, ok := mulU64(stride, uint64(slot))
	if !ok {
		return nil, nil, false
	}
	body, ok := addU64(uint64(poff)+24, skip)
	if !ok {
		return nil, nil, false
	}
	mid, ok := addU64(body, bl)
	if !ok {
		return nil, nil, false
	}
	end, ok := addU64(mid, cells)
	if !ok || end > dlen || end > uint64(math.MaxInt) {
		return nil, nil, false
	}
	bodyI := int(body)
	endI := int(end)
	return sr.data[bodyI : bodyI+int(bl)], sr.data[bodyI+int(bl) : endI], true
}
