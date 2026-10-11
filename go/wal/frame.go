package wal

import (
	"bufio"
	"encoding/binary"
	"errors"
	"hash/crc32"
	"io"
)

const frameHeaderLen = 8

var (
	castagnoli = crc32.MakeTable(crc32.Castagnoli)

	errShortWrite   = errors.New("wal: short write")
	errInvalidFrame = errors.New("wal: invalid frame")
)

// appendFrame appends [payload_len u32][crc32c u32][payload] to dst.
func appendFrame(dst, payload []byte) []byte {
	dst = binary.LittleEndian.AppendUint32(dst, uint32(len(payload)))
	dst = binary.LittleEndian.AppendUint32(dst, crc32.Checksum(payload, castagnoli))
	return append(dst, payload...)
}

// frameIter reads consecutive frames of one segment up to limit.
type frameIter struct {
	f     segmentFile
	pos   int64 // offset of the next frame header
	limit int64 // exclusive end of readable bytes
	r     *bufio.Reader
	hdr   [frameHeaderLen]byte
	buf   []byte
}

func newFrameIter(f segmentFile, pos, limit int64) *frameIter {
	it := &frameIter{f: f, pos: pos}
	it.setLimit(limit)
	return it
}

// setLimit moves the readable end, for live segments whose durable end grew.
func (it *frameIter) setLimit(limit int64) {
	it.limit = limit
	if limit <= it.pos {
		it.r = nil
		return
	}
	sec := io.NewSectionReader(it.f, it.pos, limit-it.pos)
	if it.r == nil {
		it.r = bufio.NewReaderSize(sec, 256<<10)
	} else {
		it.r.Reset(sec)
	}
}

// next returns the payload and position of the next frame. It returns io.EOF
// when pos reaches limit and errInvalidFrame for a torn or corrupt frame. The
// payload is valid until the following call.
func (it *frameIter) next() ([]byte, int64, error) {
	if it.r == nil || it.pos >= it.limit {
		return nil, 0, io.EOF
	}
	if it.limit-it.pos < frameHeaderLen {
		return nil, 0, errInvalidFrame
	}
	if _, err := io.ReadFull(it.r, it.hdr[:]); err != nil {
		return nil, 0, readErr(err)
	}
	n := int64(binary.LittleEndian.Uint32(it.hdr[0:]))
	sum := binary.LittleEndian.Uint32(it.hdr[4:])
	if n < 8 || n%8 != 0 || n > it.limit-it.pos-frameHeaderLen {
		return nil, 0, errInvalidFrame
	}
	if int64(cap(it.buf)) < n {
		it.buf = make([]byte, n)
	}
	payload := it.buf[:n]
	if _, err := io.ReadFull(it.r, payload); err != nil {
		return nil, 0, readErr(err)
	}
	if crc32.Checksum(payload, castagnoli) != sum {
		return nil, 0, errInvalidFrame
	}
	pos := it.pos
	it.pos += frameHeaderLen + n
	return payload, pos, nil
}

// readErr maps a file shorter than the expected limit to an invalid frame.
func readErr(err error) error {
	if errors.Is(err, io.EOF) || errors.Is(err, io.ErrUnexpectedEOF) {
		return errInvalidFrame
	}
	return err
}

// scanFrames walks every frame from pos to limit. It returns the end of the
// last valid frame, the offset of that frame (-1 if none), and the count. err
// is errInvalidFrame when a frame before limit is invalid.
func scanFrames(f segmentFile, pos, limit int64) (end, last int64, count uint64, err error) {
	it := newFrameIter(f, pos, limit)
	end, last = pos, -1
	for {
		_, at, err := it.next()
		if errors.Is(err, io.EOF) {
			return end, last, count, nil
		}
		if err != nil {
			return end, last, count, err
		}
		last = at
		end = it.pos
		count++
	}
}
