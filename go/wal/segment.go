package wal

import (
	"encoding/binary"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
)

const (
	segMagic   uint32 = 0x5753584E // "NXSW"
	segVersion uint16 = 0x0200

	flagSchemaEmbedded uint16 = 0x0001
	flagCRCFramed      uint16 = 0x0002
	flagSealed         uint16 = 0x0004
	knownFlags                = flagSchemaEmbedded | flagCRCFramed | flagSealed
	liveFlags                 = flagSchemaEmbedded | flagCRCFramed

	fixedHeaderLen = 12
	flagsOffset    = 6
	maxSchemaBytes = 1 << 20

	segSuffix = ".nxsw"
	tmpSuffix = ".nxsw.tmp"
)

const validSigils = "=~?\"<@^"

func (s Schema) keys() []string {
	out := make([]string, len(s.Fields))
	for i, f := range s.Fields {
		out[i] = f.Key
	}
	return out
}

func (s Schema) sigils() []byte {
	out := make([]byte, len(s.Fields))
	for i, f := range s.Fields {
		out[i] = f.Sigil
	}
	return out
}

func (s Schema) validate() error {
	if len(s.Fields) == 0 || len(s.Fields) > 0xFFFF {
		return fmt.Errorf("wal: schema must have 1..65535 fields, got %d", len(s.Fields))
	}
	seen := make(map[string]bool, len(s.Fields))
	for i, f := range s.Fields {
		if f.Key == "" || strings.IndexByte(f.Key, 0) >= 0 {
			return fmt.Errorf("wal: field %d: key must be non-empty and contain no NUL", i)
		}
		if seen[f.Key] {
			return fmt.Errorf("wal: field %d: duplicate key %q", i, f.Key)
		}
		seen[f.Key] = true
		if strings.IndexByte(validSigils, f.Sigil) < 0 {
			return fmt.Errorf("wal: field %q: unsupported sigil %q", f.Key, f.Sigil)
		}
	}
	return nil
}

// IsAdditiveOf reports whether s equals prev plus zero or more trailing fields.
func (s Schema) IsAdditiveOf(prev Schema) bool {
	if len(s.Fields) < len(prev.Fields) {
		return false
	}
	for i, f := range prev.Fields {
		if s.Fields[i] != f {
			return false
		}
	}
	return true
}

func (s Schema) equal(o Schema) bool {
	return len(s.Fields) == len(o.Fields) && s.IsAdditiveOf(o)
}

func align8(n int) int { return (n + 7) &^ 7 }

// encodeSchema uses the .nxb schema header encoding: KeyCount u16, sigils,
// NUL-terminated keys, zero-padded to a multiple of 8.
func encodeSchema(s Schema) []byte {
	size := 2 + len(s.Fields)
	for _, f := range s.Fields {
		size += len(f.Key) + 1
	}
	b := make([]byte, align8(size))
	binary.LittleEndian.PutUint16(b, uint16(len(s.Fields)))
	p := 2
	for _, f := range s.Fields {
		b[p] = f.Sigil
		p++
	}
	for _, f := range s.Fields {
		p += copy(b[p:], f.Key) + 1
	}
	return b
}

func decodeSchema(b []byte) (Schema, error) {
	if len(b) < 2 {
		return Schema{}, errors.New("schema too short")
	}
	n := int(binary.LittleEndian.Uint16(b))
	if len(b) < 2+n {
		return Schema{}, errors.New("schema type manifest truncated")
	}
	sigils := b[2 : 2+n]
	p := 2 + n
	s := Schema{Fields: make([]Field, n)}
	for i := 0; i < n; i++ {
		end := -1
		for j := p; j < len(b); j++ {
			if b[j] == 0 {
				end = j
				break
			}
		}
		if end < 0 {
			return Schema{}, errors.New("schema key pool truncated")
		}
		s.Fields[i] = Field{Key: string(b[p:end]), Sigil: sigils[i]}
		p = end + 1
	}
	if err := s.validate(); err != nil {
		return Schema{}, err
	}
	return s, nil
}

func encodeHeader(s Schema, flags uint16) []byte {
	sb := encodeSchema(s)
	h := make([]byte, align8(fixedHeaderLen+len(sb)))
	binary.LittleEndian.PutUint32(h[0:], segMagic)
	binary.LittleEndian.PutUint16(h[4:], segVersion)
	binary.LittleEndian.PutUint16(h[flagsOffset:], flags)
	binary.LittleEndian.PutUint32(h[8:], uint32(len(sb)))
	copy(h[fixedHeaderLen:], sb)
	return h
}

type segHeader struct {
	flags  uint16
	schema Schema
	length int64 // frames start here
}

func (h segHeader) sealed() bool { return h.flags&flagSealed != 0 }

func readHeader(f segmentFile) (segHeader, error) {
	size, err := f.Size()
	if err != nil {
		return segHeader{}, err
	}
	var fixed [fixedHeaderLen]byte
	if size < fixedHeaderLen {
		return segHeader{}, errors.New("header truncated")
	}
	if _, err := f.ReadAt(fixed[:], 0); err != nil {
		return segHeader{}, err
	}
	if binary.LittleEndian.Uint32(fixed[0:]) != segMagic {
		return segHeader{}, errors.New("bad magic")
	}
	if v := binary.LittleEndian.Uint16(fixed[4:]); v != segVersion {
		return segHeader{}, fmt.Errorf("unsupported version %#04x", v)
	}
	flags := binary.LittleEndian.Uint16(fixed[flagsOffset:])
	if flags&^knownFlags != 0 || flags&liveFlags != liveFlags {
		return segHeader{}, fmt.Errorf("bad flags %#04x", flags)
	}
	schemaLen := int64(binary.LittleEndian.Uint32(fixed[8:]))
	length := int64(align8(fixedHeaderLen + int(min(schemaLen, maxSchemaBytes))))
	if schemaLen > maxSchemaBytes || length > size {
		return segHeader{}, fmt.Errorf("schema length %d exceeds file", schemaLen)
	}
	sb := make([]byte, schemaLen)
	if _, err := f.ReadAt(sb, fixedHeaderLen); err != nil {
		return segHeader{}, err
	}
	schema, err := decodeSchema(sb)
	if err != nil {
		return segHeader{}, err
	}
	return segHeader{flags: flags, schema: schema, length: length}, nil
}

func writeFlags(f segmentFile, flags uint16) error {
	var b [2]byte
	binary.LittleEndian.PutUint16(b[:], flags)
	n, err := f.WriteAt(b[:], flagsOffset)
	if err == nil && n < len(b) {
		err = errShortWrite
	}
	return err
}

func segmentName(n uint64) string { return fmt.Sprintf("%016x%s", n, segSuffix) }

func parseSegmentName(name string) (uint64, bool) {
	stem, ok := strings.CutSuffix(name, segSuffix)
	if !ok || len(stem) != 16 {
		return 0, false
	}
	n, err := strconv.ParseUint(stem, 16, 64)
	return n, err == nil && n > 0
}

// listSegments returns segment numbers in ascending order and leftover .tmp
// names. Segments must be contiguous.
func listSegments(fs fileSystem, dir string) ([]uint64, []string, error) {
	names, err := fs.ReadDir(dir)
	if err != nil {
		return nil, nil, err
	}
	var segs []uint64
	var tmps []string
	for _, name := range names {
		if n, ok := parseSegmentName(name); ok {
			segs = append(segs, n)
		} else if strings.HasSuffix(name, tmpSuffix) {
			tmps = append(tmps, name)
		}
	}
	slices.Sort(segs)
	for i := 1; i < len(segs); i++ {
		if segs[i] != segs[i-1]+1 {
			return nil, nil, fmt.Errorf("%w: segment %d missing", ErrCorruptSegment, segs[i-1]+1)
		}
	}
	return segs, tmps, nil
}

// createSegment writes a new segment header to <n>.nxsw.tmp, syncs it,
// renames it into place, syncs the directory, and reopens it read-write.
func createSegment(fs fileSystem, dir string, n uint64, schema Schema) (segmentFile, int64, error) {
	final := filepath.Join(dir, segmentName(n))
	tmp := final + ".tmp"
	hdr := encodeHeader(schema, liveFlags)
	f, err := fs.OpenFile(tmp, os.O_RDWR|os.O_CREATE|os.O_TRUNC)
	if err != nil {
		return nil, 0, err
	}
	written, err := f.WriteAt(hdr, 0)
	if err == nil && written < len(hdr) {
		err = errShortWrite
	}
	if err == nil {
		err = f.Sync()
	}
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		return nil, 0, err
	}
	if err := fs.Rename(tmp, final); err != nil {
		return nil, 0, err
	}
	if err := fs.SyncDir(dir); err != nil {
		return nil, 0, err
	}
	live, err := fs.OpenFile(final, os.O_RDWR)
	if err != nil {
		return nil, 0, err
	}
	return live, int64(len(hdr)), nil
}
