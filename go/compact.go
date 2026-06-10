// NXS v1.3 compact row decode — mirrors js/compact.js and rust/src/compact.rs.

package nxs

import (
	"encoding/binary"
	"fmt"
	"math"
	"sort"
)

const (
	flagDenseFrames      uint16 = 0x0010
	flagPackedBools      uint16 = 0x0020
	flagNarrowCells      uint16 = 0x0040
	flagDeltaTail        uint16 = 0x0080
	flagDenseWireReorder uint16 = 0x0100
	recordHdrDense       uint8  = 0x01
	fieldAttrPromoted    uint8  = 0x01
	fieldAttrU16Len      uint8  = 0x02

	sigilKeyword byte = '$'
)

type extendedSchema struct {
	keys       []string
	sigils     []byte
	widths     []byte
	fieldAttrs []byte
	valuePool  []string
}

type rowCellPlan struct {
	packedBools           bool
	narrow                bool
	denseAllowed          bool
	denseWireReorder      bool
	boolSlots             []int
	firstBool             int // -1 if none
	wireOrder             []int
	denseFixedBodyOffsets []int // -1 if unset
	denseVarBodyStart     int   // -1 if unset
}

type deltaTailLayout struct {
	tailPtr     int
	recordCount uint32
	blockSize   uint32
	singleKeyID bool
	anchorsOff  int
	deltasOff   int
}

func alignTo(pos, align int) int {
	if align == 0 {
		return pos
	}
	return (pos + align - 1) &^ (align - 1)
}

func (s *extendedSchema) isPromoted(slot int) bool {
	return slot < len(s.fieldAttrs) && s.fieldAttrs[slot]&fieldAttrPromoted != 0
}

func (s *extendedSchema) strLenPrefix(slot int) int {
	if s.isPromoted(slot) {
		return 0
	}
	if slot < len(s.fieldAttrs) && s.fieldAttrs[slot]&fieldAttrU16Len != 0 {
		return 2
	}
	return 4
}

func (s *extendedSchema) cellWidth(slot int) byte {
	if s.isPromoted(slot) || (slot < len(s.sigils) && s.sigils[slot] == sigilKeyword) {
		return 2
	}
	if slot < len(s.widths) && s.widths[slot] != 0 {
		return s.widths[slot]
	}
	return 8
}

func boolSlotsFromSchema(s *extendedSchema) []int {
	var out []int
	for i, sig := range s.sigils {
		if sig == sigilBool {
			out = append(out, i)
		}
	}
	return out
}

func parseExtendedSchema(data []byte, pos int, flags uint16) (*extendedSchema, int, error) {
	if pos+2 > len(data) {
		return nil, 0, fmtError("ERR_OUT_OF_BOUNDS", "schema header")
	}
	keyCount := int(binary.LittleEndian.Uint16(data[pos : pos+2]))
	pos += 2
	if pos+keyCount > len(data) {
		return nil, 0, fmtError("ERR_OUT_OF_BOUNDS", "type manifest")
	}
	sigils := make([]byte, keyCount)
	copy(sigils, data[pos:pos+keyCount])
	pos += keyCount

	keys := make([]string, 0, keyCount)
	for i := 0; i < keyCount; i++ {
		start := pos
		for pos < len(data) && data[pos] != 0 {
			pos++
		}
		if pos >= len(data) {
			return nil, 0, fmtError("ERR_OUT_OF_BOUNDS", "string pool")
		}
		keys = append(keys, string(data[start:pos]))
		pos++
	}
	if rem := pos % 8; rem != 0 {
		pos += 8 - rem
	}

	widths := make([]byte, keyCount)
	if flags&flagNarrowCells != 0 {
		if pos+keyCount > len(data) {
			return nil, 0, fmtError("ERR_OUT_OF_BOUNDS", "narrow widths")
		}
		copy(widths, data[pos:pos+keyCount])
		pos += keyCount
	}

	fieldAttrs := make([]byte, keyCount)
	if flags&flagV13CompactMask != 0 {
		if pos+keyCount > len(data) {
			return nil, 0, fmtError("ERR_OUT_OF_BOUNDS", "field attrs")
		}
		copy(fieldAttrs, data[pos:pos+keyCount])
		pos += keyCount
	}

	var valuePool []string
	if pos+2 <= len(data) {
		valueCount := int(binary.LittleEndian.Uint16(data[pos : pos+2]))
		pos += 2
		for i := 0; i < valueCount; i++ {
			start := pos
			for pos < len(data) && data[pos] != 0 {
				pos++
			}
			if pos >= len(data) {
				return nil, 0, fmtError("ERR_OUT_OF_BOUNDS", "value pool")
			}
			valuePool = append(valuePool, string(data[start:pos]))
			pos++
		}
		if valueCount > 0 {
			if rem := pos % 8; rem != 0 {
				pos += 8 - rem
			}
		}
	}

	return &extendedSchema{
		keys:       keys,
		sigils:     sigils,
		widths:     widths,
		fieldAttrs: fieldAttrs,
		valuePool:  valuePool,
	}, pos, nil
}

func fmtError(code, msg string) error {
	return fmt.Errorf("%s: %s", code, msg)
}

func (p *rowCellPlan) boolWordBytes() int {
	if !p.packedBools || len(p.boolSlots) == 0 {
		return 0
	}
	n := (len(p.boolSlots) + 7) / 8
	if n < 1 {
		return 1
	}
	return n
}

func denseCellAlignWidth(fi int, s *extendedSchema, p *rowCellPlan) int {
	if p.packedBools && containsInt(p.boolSlots, fi) {
		return p.boolWordBytes()
	}
	sig := s.sigils[fi]
	if isVarSigil(sig) {
		return 0
	}
	if s.isPromoted(fi) || sig == sigilKeyword {
		return 2
	}
	if sig == sigilI64 || sig == sigilF64 {
		if p.narrow {
			return int(s.cellWidth(fi))
		}
		return 8
	}
	if sig == sigilTime {
		return 8
	}
	if sig == sigilBool && !p.packedBools {
		return 8
	}
	if sig == sigilNull {
		return 1
	}
	return 8
}

func denseWireOrder(s *extendedSchema, p *rowCellPlan) []int {
	if !p.denseWireReorder {
		out := make([]int, len(s.keys))
		for i := range out {
			out[i] = i
		}
		return out
	}
	type pair struct {
		w, fi int
	}
	var fixed []pair
	var vars []int
	for fi := range s.keys {
		sig := s.sigils[fi]
		if isVarSigil(sig) {
			vars = append(vars, fi)
			continue
		}
		if p.packedBools && containsInt(p.boolSlots, fi) {
			if p.firstBool == fi {
				fixed = append(fixed, pair{p.boolWordBytes(), fi})
			}
			continue
		}
		fixed = append(fixed, pair{denseCellAlignWidth(fi, s, p), fi})
	}
	sort.Slice(fixed, func(i, j int) bool {
		if fixed[i].w != fixed[j].w {
			return fixed[i].w > fixed[j].w
		}
		return fixed[i].fi < fixed[j].fi
	})
	out := make([]int, 0, len(s.keys))
	for _, pr := range fixed {
		out = append(out, pr.fi)
	}
	out = append(out, vars...)
	return out
}

func newRowCellPlan(s *extendedSchema, flags uint16) *rowCellPlan {
	p := &rowCellPlan{
		packedBools:      flags&flagPackedBools != 0,
		narrow:           flags&flagNarrowCells != 0,
		denseAllowed:     flags&flagDenseFrames != 0,
		denseWireReorder: flags&flagDenseWireReorder != 0,
		boolSlots:        boolSlotsFromSchema(s),
		firstBool:        -1,
	}
	if len(p.boolSlots) > 0 {
		p.firstBool = p.boolSlots[0]
	}
	p.wireOrder = denseWireOrder(s, p)
	if p.denseWireReorder && p.denseAllowed {
		p.denseFixedBodyOffsets, p.denseVarBodyStart = precomputeDenseFixedOffsets(s, p)
	} else {
		p.denseVarBodyStart = -1
	}
	return p
}

func containsInt(sl []int, v int) bool {
	for _, x := range sl {
		if x == v {
			return true
		}
	}
	return false
}

func indexOfInt(sl []int, v int) int {
	for i, x := range sl {
		if x == v {
			return i
		}
	}
	return -1
}

func advancePastBoolWord(pos int, p *rowCellPlan) int {
	bw := p.boolWordBytes()
	return alignTo(pos, bw) + bw
}

func advanceDensePastCellFixed(pos, fi int, s *extendedSchema, p *rowCellPlan) int {
	sig := s.sigils[fi]
	w := 8
	if p.narrow {
		w = int(s.cellWidth(fi))
	}
	if p.packedBools && containsInt(p.boolSlots, fi) {
		return advancePastBoolWord(pos, p)
	}
	if (sig == sigilI64 || sig == sigilF64 || sig == sigilBool) && (!p.packedBools || sig != sigilBool) {
		return alignTo(pos, w) + w
	}
	if sig == sigilKeyword {
		return alignTo(pos, 2) + 2
	}
	return alignTo(pos, 8) + 8
}

func precomputeDenseFixedOffsets(s *extendedSchema, p *rowCellPlan) ([]int, int) {
	n := len(s.keys)
	out := make([]int, n)
	for i := range out {
		out[i] = -1
	}
	pos := 0
	varStart := -1
	for _, fi := range p.wireOrder {
		if p.packedBools && containsInt(p.boolSlots, fi) {
			if p.firstBool == fi {
				bw := p.boolWordBytes()
				base := alignTo(pos, bw)
				for idx, bs := range p.boolSlots {
					out[bs] = base + idx/8
				}
				pos = base + bw
			}
			continue
		}
		sig := s.sigils[fi]
		if isVarSigil(sig) {
			varStart = pos
			break
		}
		w := 8
		if p.narrow {
			w = int(s.cellWidth(fi))
		}
		off := alignTo(pos, w)
		if s.isPromoted(fi) || sig == sigilKeyword {
			off = alignTo(pos, 2)
		}
		out[fi] = off
		pos = advanceDensePastCellFixed(pos, fi, s, p)
	}
	return out, varStart
}

func readStrCellLen(data []byte, off, prefixLen int) int {
	switch prefixLen {
	case 2:
		return int(binary.LittleEndian.Uint16(data[off : off+2]))
	case 4:
		return int(binary.LittleEndian.Uint32(data[off : off+4]))
	default:
		return -1
	}
}

func advancePastStrCell(pos, payloadLen, prefixLen int) int {
	cellBytes := prefixLen + payloadLen
	pad := (8 - cellBytes%8) % 8
	return pos + cellBytes + pad
}

func advanceDensePastCell(data []byte, bodyBase, pos, fi int, s *extendedSchema, p *rowCellPlan) int {
	sig := s.sigils[fi]
	w := 8
	if p.narrow {
		w = int(s.cellWidth(fi))
	}
	if p.packedBools && containsInt(p.boolSlots, fi) {
		return advancePastBoolWord(pos, p)
	}
	if (sig == sigilI64 || sig == sigilF64 || sig == sigilBool) && (!p.packedBools || sig != sigilBool) {
		return alignTo(pos, w) + w
	}
	if sig == sigilStr || sig == sigilBinary {
		if s.isPromoted(fi) {
			return alignTo(pos, 2) + 2
		}
		prefix := s.strLenPrefix(fi)
		abs := bodyBase + pos
		l := readStrCellLen(data, abs, prefix)
		return advancePastStrCell(pos, l, prefix)
	}
	if sig == sigilKeyword {
		return alignTo(pos, 2) + 2
	}
	return alignTo(pos, 8) + 8
}

func decodeIntCell(data []byte, offset int, width byte) int64 {
	switch width {
	case 1:
		v := int8(data[offset])
		return int64(v)
	case 2:
		return int64(int16(binary.LittleEndian.Uint16(data[offset : offset+2])))
	case 4:
		return int64(int32(binary.LittleEndian.Uint32(data[offset : offset+4])))
	case 8:
		return int64(binary.LittleEndian.Uint64(data[offset : offset+8]))
	default:
		return 0
	}
}

func decodeF64Cell(data []byte, offset int, width byte) float64 {
	switch width {
	case 4:
		bits := binary.LittleEndian.Uint32(data[offset : offset+4])
		return float64(math.Float32frombits(bits))
	case 8:
		return math.Float64frombits(binary.LittleEndian.Uint64(data[offset : offset+8]))
	default:
		return 0
	}
}

func isDenseRecord(data []byte, offset int) bool {
	if offset+9 > len(data) {
		return false
	}
	if binary.LittleEndian.Uint32(data[offset:]) != magicObj {
		return false
	}
	return data[offset+8]&recordHdrDense != 0
}

func denseFieldOffset(data []byte, objOffset, slot int, s *extendedSchema, p *rowCellPlan) int {
	bodyBase := objOffset + 9
	if p.denseFixedBodyOffsets != nil {
		if p.denseFixedBodyOffsets[slot] >= 0 {
			return bodyBase + p.denseFixedBodyOffsets[slot]
		}
		if p.denseVarBodyStart >= 0 {
			pos := p.denseVarBodyStart
			for _, fi := range p.wireOrder {
				if !isVarSigil(s.sigils[fi]) {
					continue
				}
				if fi == slot {
					return bodyBase + pos
				}
				pos = advanceDensePastCell(data, bodyBase, pos, fi, s, p)
			}
			return -1
		}
	}
	pos := 0
	for _, fi := range p.wireOrder {
		if p.packedBools && containsInt(p.boolSlots, fi) {
			if containsInt(p.boolSlots, slot) && fi == p.firstBool {
				byteIdx := indexOfInt(p.boolSlots, slot) / 8
				return bodyBase + alignTo(pos, p.boolWordBytes()) + byteIdx
			}
			if fi == p.firstBool {
				pos = advancePastBoolWord(pos, p)
			}
			continue
		}
		sig := s.sigils[fi]
		w := 8
		if p.narrow {
			w = int(s.cellWidth(fi))
		}
		if fi == slot {
			off := pos
			if s.isPromoted(fi) || sig == sigilKeyword {
				off = alignTo(pos, 2)
			} else if !isVarSigil(sig) {
				off = alignTo(pos, w)
			}
			return bodyBase + off
		}
		pos = advanceDensePastCell(data, bodyBase, pos, fi, s, p)
	}
	return -1
}

func resolveSlotV12(data []byte, objOffset, slot int) int {
	p := objOffset + 8
	cur := 0
	tableIdx := 0
	found := false
	var b byte
	for {
		if p >= len(data) {
			return -1
		}
		b = data[p]
		p++
		bits := b & 0x7F
		for bit := 0; bit < 7; bit++ {
			if cur == slot {
				if (bits>>bit)&1 == 0 {
					return -1
				}
				found = true
			} else if cur < slot && (bits>>bit)&1 == 1 {
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
	for b&0x80 != 0 {
		if p >= len(data) {
			return -1
		}
		b = data[p]
		p++
	}
	rel := binary.LittleEndian.Uint16(data[p+tableIdx*2:])
	return objOffset + int(rel)
}

func sparseOffsetTableSlots(presentSlots []int, p *rowCellPlan) []int {
	if !p.packedBools {
		out := make([]int, len(presentSlots))
		copy(out, presentSlots)
		return out
	}
	var out []int
	boolWordAdded := false
	for _, fi := range presentSlots {
		if containsInt(p.boolSlots, fi) {
			if !boolWordAdded {
				out = append(out, fi)
				boolWordAdded = true
			}
		} else {
			out = append(out, fi)
		}
	}
	return out
}

func resolveSlotV13Sparse(data []byte, objOffset, slot int, s *extendedSchema, p *rowCellPlan) int {
	pos := objOffset + 9
	cur := 0
	slotPresent := false
	var presentSlots []int
	fieldCount := len(s.keys)

	done := false
	for !done {
		if pos >= len(data) {
			return -1
		}
		b := data[pos]
		pos++
		bits := b & 0x7F
		for bit := 0; bit < 7; bit++ {
			if (bits>>bit)&1 == 1 {
				presentSlots = append(presentSlots, cur)
				if cur == slot {
					slotPresent = true
				}
			}
			cur++
			if cur >= fieldCount {
				done = true
				break
			}
		}
		if b&0x80 == 0 {
			break
		}
	}
	if !slotPresent {
		return -1
	}
	otSlots := sparseOffsetTableSlots(presentSlots, p)
	tableBase := pos
	if p.packedBools && containsInt(p.boolSlots, slot) {
		boolTableFi := -1
		for _, fi := range otSlots {
			if containsInt(p.boolSlots, fi) {
				boolTableFi = fi
				break
			}
		}
		tableIdx := indexOfInt(otSlots, boolTableFi)
		rel := binary.LittleEndian.Uint16(data[tableBase+tableIdx*2:])
		base := objOffset + int(rel)
		bitInWord := indexOfInt(p.boolSlots, slot)
		return base + bitInWord/8
	}
	tableIdx := indexOfInt(otSlots, slot)
	rel := binary.LittleEndian.Uint16(data[tableBase+tableIdx*2:])
	return objOffset + int(rel)
}

func resolveFieldOffset(data []byte, objOffset, slot int, s *extendedSchema, p *rowCellPlan, denseFrames bool) int {
	if denseFrames {
		if data[objOffset+8]&recordHdrDense != 0 {
			return denseFieldOffset(data, objOffset, slot, s, p)
		}
		return resolveSlotV13Sparse(data, objOffset, slot, s, p)
	}
	return resolveSlotV12(data, objOffset, slot)
}

func readPackedBool(data []byte, objOffset, slot int, s *extendedSchema, p *rowCellPlan) (bool, bool) {
	off := resolveFieldOffset(data, objOffset, slot, s, p, true)
	if off < 0 {
		return false, false
	}
	bitPos := indexOfInt(p.boolSlots, slot)
	if bitPos < 0 {
		return false, false
	}
	b := data[off]
	return (b>>(bitPos%8))&1 == 1, true
}

func parseDeltaTailLayout(data []byte, tailPtr int) (*deltaTailLayout, error) {
	if tailPtr+12 > len(data) {
		return nil, fmtError("ERR_OUT_OF_BOUNDS", "delta tail header")
	}
	recordCount := binary.LittleEndian.Uint32(data[tailPtr : tailPtr+4])
	blockSize := binary.LittleEndian.Uint32(data[tailPtr+4 : tailPtr+8])
	tiFlags := binary.LittleEndian.Uint16(data[tailPtr+8 : tailPtr+10])
	anchorCount := int(binary.LittleEndian.Uint16(data[tailPtr+10 : tailPtr+12]))
	anchorsOff := tailPtr + alignTo(12, 8)
	deltasOff := anchorsOff + anchorCount*8
	return &deltaTailLayout{
		tailPtr:     tailPtr,
		recordCount: recordCount,
		blockSize:   blockSize,
		singleKeyID: tiFlags&0x0001 != 0,
		anchorsOff:  anchorsOff,
		deltasOff:   deltasOff,
	}, nil
}

func deltaRecordOffset(data []byte, layout *deltaTailLayout, index int) int64 {
	if uint32(index) >= layout.recordCount {
		return -1
	}
	a := layout.blockSize
	if a == 0 {
		a = 1
	}
	anchorIdx := index / int(a)
	anchorOff := layout.anchorsOff + anchorIdx*8
	anchor := int64(binary.LittleEndian.Uint64(data[anchorOff : anchorOff+8]))
	deltaOff := layout.deltasOff + index*4
	delta := int64(binary.LittleEndian.Uint32(data[deltaOff : deltaOff+4]))
	return anchor + delta
}

func materialiseStrAt(data []byte, off, slot int, s *extendedSchema) string {
	if s.isPromoted(slot) {
		idx := binary.LittleEndian.Uint16(data[off : off+2])
		return s.valuePool[idx]
	}
	prefix := s.strLenPrefix(slot)
	l := readStrCellLen(data, off, prefix)
	return string(data[off+prefix : off+prefix+l])
}
