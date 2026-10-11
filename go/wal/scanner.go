package wal

import (
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"

	nxs "github.com/nyxis-io/nyxis-drivers/go"
)

// Scanner reads frames in order across segments (and schema boundaries),
// starting strictly after a given Offset.
type Scanner struct {
	dir   string
	fs    fileSystem
	w     *WAL   // live scanner when non-nil
	top   uint64 // offline: highest segment at open
	after Offset

	seg       uint64
	f         segmentFile
	it        *frameIter
	schema    Schema
	dec       *nxs.ObjectDecoder
	skipFirst bool // the next frame is `after` itself

	rec    *nxs.Object
	off    Offset
	err    error
	closed bool
}

// OpenScanner reads a WAL directory without locking it. At the end of the
// highest segment a partial or invalid frame ends the scan without error; in
// any lower segment it is ErrCorruptSegment.
func OpenScanner(dir string, after Offset) (*Scanner, error) {
	fs := osFS{}
	segs, _, err := listSegments(fs, dir)
	if err != nil {
		return nil, err
	}
	s := &Scanner{dir: dir, fs: fs, after: after}
	if len(segs) == 0 {
		s.closed = true
		return s, nil
	}
	s.top = segs[len(segs)-1]
	s.seg = max(after.Segment, segs[0])
	return s, nil
}

// NewLiveScanner reads up to the WAL's durable end as of each Next call; Next
// returns false there and true again once more records are durable.
func (w *WAL) NewLiveScanner(after Offset) (*Scanner, error) {
	segs, _, err := listSegments(w.fs, w.dir)
	if err != nil {
		return nil, err
	}
	s := &Scanner{dir: w.dir, fs: w.fs, w: w, after: after}
	s.seg = after.Segment
	if len(segs) > 0 {
		s.seg = max(s.seg, segs[0])
	}
	return s, nil
}

// bounds returns the readable end of the current segment (-1: the file size,
// once opened), whether the segment is complete, and whether it exists yet.
func (s *Scanner) bounds() (limit int64, complete, exists bool, err error) {
	if s.w != nil {
		durSeg, durEnd := s.w.durableState()
		switch {
		case s.seg > durSeg:
			return 0, false, false, nil
		case s.seg == durSeg:
			return durEnd, false, true, nil
		}
	} else if s.seg > s.top {
		return 0, false, false, nil
	}
	complete = s.w != nil || s.seg < s.top
	if s.f == nil {
		return -1, complete, true, nil
	}
	size, err := s.f.Size()
	return size, complete, true, err
}

func (s *Scanner) openSegment(limit int64) error {
	f, err := s.fs.OpenFile(filepath.Join(s.dir, segmentName(s.seg)), os.O_RDONLY)
	if err != nil {
		return err
	}
	h, err := readHeader(f)
	if err != nil {
		_ = f.Close()
		return corrupt(s.seg, err)
	}
	dec, err := nxs.NewObjectDecoder(h.schema.keys(), h.schema.sigils())
	if err != nil {
		_ = f.Close()
		return corrupt(s.seg, err)
	}
	if limit < 0 {
		if limit, err = f.Size(); err != nil {
			_ = f.Close()
			return err
		}
	}
	start := h.length
	if s.seg == s.after.Segment && int64(s.after.Pos) >= h.length {
		start = int64(s.after.Pos)
		s.skipFirst = true
	}
	s.f, s.schema, s.dec = f, h.schema, dec
	s.it = newFrameIter(f, start, limit)
	return nil
}

func (s *Scanner) closeSegment() {
	if s.f != nil {
		_ = s.f.Close()
	}
	s.f, s.it = nil, nil
	s.seg++
}

// Next advances to the next record.
func (s *Scanner) Next() bool {
	if s.err != nil || s.closed {
		return false
	}
	for {
		limit, complete, exists, err := s.bounds()
		if err != nil {
			s.err = err
			return false
		}
		if !exists {
			return false
		}
		if s.f == nil {
			if err := s.openSegment(limit); err != nil {
				s.err = err
				return false
			}
		} else if limit != s.it.limit {
			s.it.setLimit(limit)
		}

		payload, pos, err := s.it.next()
		switch {
		case err == nil:
			if s.skipFirst {
				s.skipFirst = false
				if uint64(pos) != s.after.Pos {
					s.err = fmt.Errorf("wal: offset %+v is not a frame boundary", s.after)
					return false
				}
				continue
			}
			obj, derr := s.dec.Decode(payload)
			if derr != nil {
				s.err = corrupt(s.seg, fmt.Errorf("frame at %d: %v", pos, derr))
				return false
			}
			s.rec, s.off = obj, Offset{Segment: s.seg, Pos: uint64(pos)}
			return true
		case errors.Is(err, io.EOF):
			if !complete {
				return false
			}
			s.closeSegment()
		case errors.Is(err, errInvalidFrame):
			if complete {
				s.err = corrupt(s.seg, fmt.Errorf("invalid frame at %d", s.it.pos))
			}
			return false
		default:
			s.err = err
			return false
		}
	}
}

// Record returns the current record, decoded with its segment's schema. It is
// valid until the next call to Next.
func (s *Scanner) Record() *nxs.Object { return s.rec }

// Schema returns the schema of the segment holding the current record.
func (s *Scanner) Schema() Schema { return s.schema }

// Offset returns the current record's offset.
func (s *Scanner) Offset() Offset { return s.off }

// Err returns the error that stopped the scan, if any.
func (s *Scanner) Err() error { return s.err }

// Close releases the open segment file.
func (s *Scanner) Close() error {
	s.closed = true
	if s.f != nil {
		err := s.f.Close()
		s.f, s.it = nil, nil
		return err
	}
	return nil
}
