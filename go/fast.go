// Fast-path reducers for uniform-schema datasets.
//
// A dataset is "uniform" when every top-level record declares the same set
// of fields (same bitmask bytes). When that holds, the location of a given
// slot's value inside each record is predictable: the bitmask length and
// the slot's index in the offset table are constant.
//
// The *Fast reducers assume uniformity. They precompute the slot's
// offset-table position once from record 0, then scan every record with no
// bitmask walk, no counting, and no function calls for byte reads — they
// use unsafe pointer math to load u16/u64/f64 directly.
//
// If you are unsure, call reader.IsUniform(slot) first, or use the non-Fast
// reducers (SumF64 etc.) which handle per-record variation correctly.
package nxs

import (
	"math"
	"runtime"
	"sync"
	"unsafe"
)

// fastLayout caches per-slot constants for uniform-schema scans.
type fastLayout struct {
	// bytes into the object header, starting at the object's Magic, where
	// the bitmask begins. Always 8.
	bitmaskStart int
	// length in bytes of the LEB128 bitmask for this schema.
	bitmaskLen int
	// offset-table index for the target slot (0-based).
	tableIdx int
	// True if the slot is actually present in record 0.
	present bool
}

// computeFastLayout inspects record 0 to determine where `slot`'s value lives
// within each object. Assumes the schema/bitmask is identical across records.
func (r *Reader) computeFastLayout(slot int) fastLayout {
	if r.recordCount == 0 {
		return fastLayout{}
	}
	data := r.data
	abs := int(u64(data, r.tailStart+2))
	p := abs + 8 // skip Magic + Length
	bitmaskStart := p

	// Walk the bitmask, tracking:
	//   - which bit position we're on (curSlot)
	//   - how many present bits precede `slot` (tableIdx)
	//   - whether the target slot is present
	curSlot := 0
	tableIdx := 0
	present := false
	for {
		b := data[p]
		p++
		bits := b & 0x7F
		for i := 0; i < 7; i++ {
			if curSlot == slot {
				present = (bits>>i)&1 == 1
			} else if curSlot < slot && (bits>>i)&1 == 1 {
				tableIdx++
			}
			curSlot++
		}
		if b&0x80 == 0 {
			break
		}
	}

	return fastLayout{
		bitmaskStart: 8,
		bitmaskLen:   p - bitmaskStart,
		tableIdx:     tableIdx,
		present:      present,
	}
}

// IsUniform reports whether every record shares the same bitmask bytes as
// record 0, for the leading `bitmaskLen` bytes of the schema. It runs in
// O(records) so it's only worth calling if you plan many fast scans.
func (r *Reader) IsUniform() bool {
	if r.recordCount == 0 {
		return true
	}
	data := r.data
	abs0 := int(u64(data, r.tailStart+2))
	// Scan bitmask length of record 0
	p := abs0 + 8
	for {
		b := data[p]
		p++
		if b&0x80 == 0 {
			break
		}
	}
	maskBytes := data[abs0+8 : p]
	maskLen := len(maskBytes)

	// Compare every subsequent record's bitmask to record 0's.
	for i := 1; i < int(r.recordCount); i++ {
		abs := int(u64(data, r.tailStart+i*10+2))
		if !bytesEq(data[abs+8:abs+8+maskLen], maskBytes) {
			return false
		}
	}
	return true
}

func bytesEq(a, b []byte) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// ── Unsafe LE readers (bounds-check-free when caller has validated) ─────────

//go:nosplit
func u16(b []byte, off int) uint16 {
	return *(*uint16)(unsafe.Pointer(&b[off]))
}

//go:nosplit
func u64(b []byte, off int) uint64 {
	return *(*uint64)(unsafe.Pointer(&b[off]))
}

//go:nosplit
func f64(b []byte, off int) float64 {
	return *(*float64)(unsafe.Pointer(&b[off]))
}

// ── Fast reducers ────────────────────────────────────────────────────────────

// SumF64Fast returns the sum of `key`'s f64 values assuming every record
// has a uniform schema. If uniformity does not hold, the result is undefined
// (field values from other slots may be summed). Use SumF64 for the safe path.
func (r *Reader) SumF64Fast(key string) float64 {
	slot, ok := r.keyIndex[key]
	if !ok {
		panic("nxs: key not in schema")
	}
	layout := r.computeFastLayout(slot)
	if !layout.present {
		return 0
	}
	data := r.data
	tail := r.tailStart
	n := int(r.recordCount)
	offsetTablePos := 8 + layout.bitmaskLen + layout.tableIdx*2

	var sum float64
	for i := 0; i < n; i++ {
		abs := int(u64(data, tail+i*10+2))
		rel := int(u16(data, abs+offsetTablePos))
		sum += f64(data, abs+rel)
	}
	return sum
}

// SumI64Fast returns the sum of `key`'s i64 values under the same uniformity
// assumption as SumF64Fast.
func (r *Reader) SumI64Fast(key string) int64 {
	slot, ok := r.keyIndex[key]
	if !ok {
		panic("nxs: key not in schema")
	}
	layout := r.computeFastLayout(slot)
	if !layout.present {
		return 0
	}
	data := r.data
	tail := r.tailStart
	n := int(r.recordCount)
	offsetTablePos := 8 + layout.bitmaskLen + layout.tableIdx*2

	var sum int64
	for i := 0; i < n; i++ {
		abs := int(u64(data, tail+i*10+2))
		rel := int(u16(data, abs+offsetTablePos))
		sum += int64(u64(data, abs+rel))
	}
	return sum
}

// MinF64Fast returns (min, have) under uniformity.
func (r *Reader) MinF64Fast(key string) (float64, bool) {
	slot, ok := r.keyIndex[key]
	if !ok {
		panic("nxs: key not in schema")
	}
	layout := r.computeFastLayout(slot)
	if !layout.present || r.recordCount == 0 {
		return 0, false
	}
	data := r.data
	tail := r.tailStart
	n := int(r.recordCount)
	offsetTablePos := 8 + layout.bitmaskLen + layout.tableIdx*2

	abs0 := int(u64(data, tail+2))
	rel0 := int(u16(data, abs0+offsetTablePos))
	m := f64(data, abs0+rel0)
	for i := 1; i < n; i++ {
		abs := int(u64(data, tail+i*10+2))
		rel := int(u16(data, abs+offsetTablePos))
		v := f64(data, abs+rel)
		if v < m {
			m = v
		}
	}
	return m, true
}

// MaxF64Fast returns (max, have) under uniformity.
func (r *Reader) MaxF64Fast(key string) (float64, bool) {
	slot, ok := r.keyIndex[key]
	if !ok {
		panic("nxs: key not in schema")
	}
	layout := r.computeFastLayout(slot)
	if !layout.present || r.recordCount == 0 {
		return 0, false
	}
	data := r.data
	tail := r.tailStart
	n := int(r.recordCount)
	offsetTablePos := 8 + layout.bitmaskLen + layout.tableIdx*2

	abs0 := int(u64(data, tail+2))
	rel0 := int(u16(data, abs0+offsetTablePos))
	m := f64(data, abs0+rel0)
	for i := 1; i < n; i++ {
		abs := int(u64(data, tail+i*10+2))
		rel := int(u16(data, abs+offsetTablePos))
		v := f64(data, abs+rel)
		if v > m {
			m = v
		}
	}
	return m, true
}

// ── Parallel reducers ────────────────────────────────────────────────────────

// SumF64FastPar is the parallel counterpart to SumF64Fast. The record range
// is split across `workers` goroutines (0 → GOMAXPROCS). Each goroutine scans
// a disjoint slice of the tail-index using the same unsafe-pointer hot loop,
// then the main goroutine sums the partials.
//
// Same uniformity assumption as SumF64Fast: every record must share record 0's
// bitmask layout.
//
// Parallelism wins when the file is large enough that the per-worker chunk
// doesn't fit in L1/L2 — below ~100k records the goroutine-launch overhead
// dominates and the serial SumF64Fast is faster.
func (r *Reader) SumF64FastPar(key string, workers int) float64 {
	slot, ok := r.keyIndex[key]
	if !ok {
		panic("nxs: key not in schema")
	}
	layout := r.computeFastLayout(slot)
	if !layout.present {
		return 0
	}
	n := int(r.recordCount)
	if workers <= 0 {
		workers = runtime.GOMAXPROCS(0)
	}
	if workers > n {
		workers = n
	}
	if workers <= 1 {
		return r.SumF64Fast(key)
	}

	data := r.data
	tail := r.tailStart
	offsetTablePos := 8 + layout.bitmaskLen + layout.tableIdx*2

	// Partition: worker w handles records [w*chunk, (w+1)*chunk).
	// Last worker gets the remainder.
	chunk := n / workers
	partials := make([]float64, workers)

	var wg sync.WaitGroup
	wg.Add(workers)
	for w := 0; w < workers; w++ {
		start := w * chunk
		end := start + chunk
		if w == workers-1 {
			end = n
		}
		go func(w, start, end int) {
			defer wg.Done()
			var s float64
			for i := start; i < end; i++ {
				abs := int(u64(data, tail+i*10+2))
				rel := int(u16(data, abs+offsetTablePos))
				s += f64(data, abs+rel)
			}
			partials[w] = s
		}(w, start, end)
	}
	wg.Wait()

	var total float64
	for _, s := range partials {
		total += s
	}
	return total
}

// SumI64FastPar — parallel variant of SumI64Fast.
func (r *Reader) SumI64FastPar(key string, workers int) int64 {
	slot, ok := r.keyIndex[key]
	if !ok {
		panic("nxs: key not in schema")
	}
	layout := r.computeFastLayout(slot)
	if !layout.present {
		return 0
	}
	n := int(r.recordCount)
	if workers <= 0 {
		workers = runtime.GOMAXPROCS(0)
	}
	if workers > n {
		workers = n
	}
	if workers <= 1 {
		return r.SumI64Fast(key)
	}

	data := r.data
	tail := r.tailStart
	offsetTablePos := 8 + layout.bitmaskLen + layout.tableIdx*2

	chunk := n / workers
	partials := make([]int64, workers)

	var wg sync.WaitGroup
	wg.Add(workers)
	for w := 0; w < workers; w++ {
		start := w * chunk
		end := start + chunk
		if w == workers-1 {
			end = n
		}
		go func(w, start, end int) {
			defer wg.Done()
			var s int64
			for i := start; i < end; i++ {
				abs := int(u64(data, tail+i*10+2))
				rel := int(u16(data, abs+offsetTablePos))
				s += int64(u64(data, abs+rel))
			}
			partials[w] = s
		}(w, start, end)
	}
	wg.Wait()

	var total int64
	for _, s := range partials {
		total += s
	}
	return total
}

// Unused import guard (math is used elsewhere in the package).
var _ = math.Float64frombits

// ── Pre-built field index ─────────────────────────────────────────────────────

// FieldIndex holds the absolute byte offsets of every occurrence of one field,
// pre-computed from the tail-index in a single forward pass.
// Once built it is a flat []uint32 — a single sequential read suffices to sum
// the field, with no pointer chasing through the object headers.
type FieldIndex struct {
	offsets []uint32 // absolute file offset of each value, in record order
	slot    int
}

// BuildFieldIndex walks every record once and records the absolute byte offset
// of `key`'s value. Cost: one full scan. Benefit: every subsequent reducer
// call on the same field is a simple sequential float64 read — no random access.
func (r *Reader) BuildFieldIndex(key string) (*FieldIndex, bool) {
	slot, ok := r.keyIndex[key]
	if !ok {
		return nil, false
	}
	layout := r.computeFastLayout(slot)
	if !layout.present {
		return nil, false
	}
	data := r.data
	tail := r.tailStart
	n := int(r.recordCount)
	offsetTablePos := 8 + layout.bitmaskLen + layout.tableIdx*2

	offsets := make([]uint32, n)
	for i := 0; i < n; i++ {
		abs := int(u64(data, tail+i*10+2))
		rel := int(u16(data, abs+offsetTablePos))
		offsets[i] = uint32(abs + rel)
	}
	return &FieldIndex{offsets: offsets, slot: slot}, true
}

// SumF64Indexed sums float64 values using a pre-built FieldIndex.
// The hot loop is a sequential read over offsets[] (likely cache-warm) plus
// one scattered data[] load per record. No bitmask walk, no offset-table read.
func (r *Reader) SumF64Indexed(idx *FieldIndex) float64 {
	data := r.data
	var sum float64
	for _, off := range idx.offsets {
		sum += f64(data, int(off))
	}
	return sum
}

// SumI64Indexed — same for i64 fields.
func (r *Reader) SumI64Indexed(idx *FieldIndex) int64 {
	data := r.data
	var sum int64
	for _, off := range idx.offsets {
		sum += int64(u64(data, int(off)))
	}
	return sum
}

// MinF64Indexed — minimum over a pre-built index.
func (r *Reader) MinF64Indexed(idx *FieldIndex) (float64, bool) {
	if len(idx.offsets) == 0 {
		return 0, false
	}
	data := r.data
	m := f64(data, int(idx.offsets[0]))
	for _, off := range idx.offsets[1:] {
		v := f64(data, int(off))
		if v < m {
			m = v
		}
	}
	return m, true
}

// MaxF64Indexed — maximum over a pre-built index.
func (r *Reader) MaxF64Indexed(idx *FieldIndex) (float64, bool) {
	if len(idx.offsets) == 0 {
		return 0, false
	}
	data := r.data
	m := f64(data, int(idx.offsets[0]))
	for _, off := range idx.offsets[1:] {
		v := f64(data, int(off))
		if v > m {
			m = v
		}
	}
	return m, true
}
