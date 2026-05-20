package nxs

import (
	"bytes"
	"encoding/binary"
	"iter"
	"math"
	"strings"
)

// ── Predicate ────────────────────────────────────────────────────────────────

// Predicate tests a single record. Implementations must not allocate.
type Predicate interface {
	test(data []byte, r *Reader, objOffset int) bool
}

// Eq returns a predicate that matches records where key == value.
// Supported value types: string, int64, float64, bool.
// int and float32 literals are automatically widened to int64 and float64.
func Eq(key string, value any) Predicate {
	switch v := value.(type) {
	case int:
		value = int64(v)
	case float32:
		value = float64(v)
	}
	return &cmpPred{key: key, val: value, op: opEq}
}

// Gt returns a predicate that matches records where key > value (numeric keys only).
func Gt(key string, value float64) Predicate { return &cmpPred{key: key, val: value, op: opGt} }

// Lt returns a predicate that matches records where key < value (numeric keys only).
func Lt(key string, value float64) Predicate { return &cmpPred{key: key, val: value, op: opLt} }

// Gte returns a predicate that matches records where key >= value (numeric keys only).
func Gte(key string, value float64) Predicate { return &cmpPred{key: key, val: value, op: opGte} }

// Lte returns a predicate that matches records where key <= value (numeric keys only).
func Lte(key string, value float64) Predicate { return &cmpPred{key: key, val: value, op: opLte} }

// And returns a predicate that passes only when all of ps pass.
func And(ps ...Predicate) Predicate { return andPred(ps) }

// Or returns a predicate that passes when any of ps passes.
func Or(ps ...Predicate) Predicate { return orPred(ps) }

// Not returns a predicate that inverts p.
func Not(p Predicate) Predicate { return notPred{p} }

type cmpOp int

const (
	opEq cmpOp = iota
	opGt
	opLt
	opGte
	opLte
)

type cmpPred struct {
	key string
	val any
	op  cmpOp
}

func (p *cmpPred) test(data []byte, r *Reader, objOffset int) bool {
	slot, ok := r.keyIndex[p.key]
	if !ok {
		return false
	}
	off := resolveSlotDirect(data, objOffset, slot)
	if off < 0 {
		return false
	}
	switch v := p.val.(type) {
	case bool:
		if off >= len(data) {
			return false
		}
		return p.op == opEq && (data[off] != 0) == v
	case string:
		if off+4 > len(data) {
			return false
		}
		length := int(binary.LittleEndian.Uint32(data[off : off+4]))
		if off+4+length > len(data) {
			return false
		}
		// Compare bytes directly; bytes.Equal avoids a string allocation on the wire value.
		return p.op == opEq && bytes.Equal(data[off+4:off+4+length], []byte(v))
	case int64:
		if off+8 > len(data) {
			return false
		}
		got := int64(binary.LittleEndian.Uint64(data[off : off+8]))
		switch p.op {
		case opEq:
			return got == v
		case opGt:
			return got > v
		case opLt:
			return got < v
		case opGte:
			return got >= v
		case opLte:
			return got <= v
		}
	case float64:
		if off+8 > len(data) {
			return false
		}
		got := math.Float64frombits(binary.LittleEndian.Uint64(data[off : off+8]))
		switch p.op {
		case opEq:
			return got == v
		case opGt:
			return got > v
		case opLt:
			return got < v
		case opGte:
			return got >= v
		case opLte:
			return got <= v
		}
	}
	return false
}

type andPred []Predicate

func (ps andPred) test(data []byte, r *Reader, off int) bool {
	for _, p := range ps {
		if !p.test(data, r, off) {
			return false
		}
	}
	return true
}

type orPred []Predicate

func (ps orPred) test(data []byte, r *Reader, off int) bool {
	for _, p := range ps {
		if p.test(data, r, off) {
			return true
		}
	}
	return false
}

type notPred struct{ inner Predicate }

func (n notPred) test(data []byte, r *Reader, off int) bool {
	return !n.inner.test(data, r, off)
}

// ── Query ─────────────────────────────────────────────────────────────────────

// Query is a lazy view over a Reader filtered by a Predicate.
// It is created via Reader.Where and consumed via Records() or Count().
// A zero-value Query (created by Reader.All) iterates all records.
type Query struct {
	r    *Reader
	pred Predicate // nil means all records
}

// Where returns a Query that yields only records matching pred.
// Multiple predicates can be combined: r.Where(And(Gt("score", 80), Eq("active", true)))
func (r *Reader) Where(pred Predicate) Query {
	return Query{r: r, pred: pred}
}

// All returns a Query that yields every record in the file.
func (r *Reader) All() Query {
	return Query{r: r}
}

// Records returns an iterator over matching *Object values.
// It reads directly from the memory-mapped buffer with no heap allocation
// in the hot path. Use with range:
//
//	for obj := range r.Where(Eq("active", true)).Records() {
//	    fmt.Println(obj.GetStr("username"))
//	}
func (q Query) Records() iter.Seq[*Object] {
	return func(yield func(*Object) bool) {
		r := q.r
		data := r.data
		tail := r.tailStart
		n := int(r.recordCount)
		if tail+n*10 > len(data) {
			return
		}
		for i := 0; i < n; i++ {
			abs := int(binary.LittleEndian.Uint64(data[tail+i*10+2 : tail+i*10+10]))
			if q.pred != nil && !q.pred.test(data, r, abs) {
				continue
			}
			obj := &Object{reader: r, offset: abs}
			if !yield(obj) {
				return
			}
		}
	}
}

// Count returns the number of records matching the predicate.
// It does not allocate — equivalent to ranging Records() and counting.
func (q Query) Count() int {
	n := 0
	for range q.Records() {
		n++
	}
	return n
}

// First returns the first matching *Object, or nil if none match.
func (q Query) First() *Object {
	for obj := range q.Records() {
		return obj
	}
	return nil
}

// ── Nested path access ────────────────────────────────────────────────────────

// GetStrPath walks a dot-notated path through nested NYXO objects.
// Example: obj.GetStrPath("address.city")
// Returns ("", false) if any segment is absent or not a string/object.
func (o *Object) GetStrPath(dotPath string) (string, bool) {
	leafOff, ok := walkDotPath(o.reader.data, o.reader, o.offset, dotPath)
	if !ok {
		return "", false
	}
	data := o.reader.data
	if leafOff+4 > len(data) {
		return "", false
	}
	length := int(binary.LittleEndian.Uint32(data[leafOff : leafOff+4]))
	if leafOff+4+length > len(data) {
		return "", false
	}
	return string(data[leafOff+4 : leafOff+4+length]), true
}

// GetI64Path walks a dot-notated path and reads the leaf as int64.
func (o *Object) GetI64Path(dotPath string) (int64, bool) {
	off, data := o.walkPath(dotPath)
	if off < 0 || off+8 > len(data) {
		return 0, false
	}
	return int64(binary.LittleEndian.Uint64(data[off : off+8])), true
}

// GetF64Path walks a dot-notated path and reads the leaf as float64.
func (o *Object) GetF64Path(dotPath string) (float64, bool) {
	off, data := o.walkPath(dotPath)
	if off < 0 || off+8 > len(data) {
		return 0, false
	}
	return math.Float64frombits(binary.LittleEndian.Uint64(data[off : off+8])), true
}

// GetBoolPath walks a dot-notated path and reads the leaf as bool.
func (o *Object) GetBoolPath(dotPath string) (bool, bool) {
	off, data := o.walkPath(dotPath)
	if off < 0 || off >= len(data) {
		return false, false
	}
	return data[off] != 0, true
}

// walkPath returns the byte offset of the leaf value and the data slice.
// Returns (-1, nil) on any failure.
func (o *Object) walkPath(dotPath string) (int, []byte) {
	leafOff, ok := walkDotPath(o.reader.data, o.reader, o.offset, dotPath)
	if !ok {
		return -1, nil
	}
	return leafOff, o.reader.data
}

// walkDotPath iterates dot-separated segments of dotPath without allocating a
// slice. It resolves each intermediate segment as an NYXO object and returns
// the absolute byte offset of the leaf field. Returns (-1, false) on any failure.
func walkDotPath(data []byte, r *Reader, objOffset int, dotPath string) (leafOff int, ok bool) {
	off := objOffset
	remaining := dotPath
	for {
		dot := strings.IndexByte(remaining, '.')
		var segment string
		if dot < 0 {
			segment = remaining
		} else {
			segment = remaining[:dot]
		}
		slot, exists := r.keyIndex[segment]
		if !exists {
			return -1, false
		}
		fieldOff := resolveSlotDirect(data, off, slot)
		if fieldOff < 0 {
			return -1, false
		}
		if dot < 0 {
			// leaf reached
			return fieldOff, true
		}
		// intermediate segment: must be an NYXO object
		if fieldOff+4 > len(data) {
			return -1, false
		}
		if binary.LittleEndian.Uint32(data[fieldOff:fieldOff+4]) != magicObj {
			return -1, false
		}
		off = fieldOff
		remaining = remaining[dot+1:]
	}
}

// ── resolveSlotDirect ─────────────────────────────────────────────────────────

// resolveSlotDirect is a stateless version of Object.resolveSlot that operates
// directly on the raw buffer and object offset. Used by predicates and path
// walkers to avoid constructing an Object on the heap.
func resolveSlotDirect(data []byte, objOffset, slot int) int {
	p := objOffset + 8 // skip Magic (4) + Length (4)
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
		for bit := range 7 {
			if cur == slot {
				if (bits>>uint(bit))&1 == 0 {
					return -1
				}
				found = true
			} else if cur < slot && (bits>>uint(bit))&1 == 1 {
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
	// Skip remaining continuation bytes before the offset table
	for b&0x80 != 0 {
		if p >= len(data) {
			return -1
		}
		b = data[p]
		p++
	}
	if p+tableIdx*2+2 > len(data) {
		return -1
	}
	rel := binary.LittleEndian.Uint16(data[p+tableIdx*2:])
	return objOffset + int(rel)
}
