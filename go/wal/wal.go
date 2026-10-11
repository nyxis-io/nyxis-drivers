// Package wal is a durable, crash-safe append-only log of NYXO objects
// (.nxsw v2). See FORMAT.md for the on-disk layout and recovery rules.
package wal

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"time"

	nxs "github.com/nyxis-io/nyxis-drivers/go"
)

var (
	ErrRecordTooLarge = errors.New("wal: record exceeds MaxRecordBytes")
	ErrSchemaMismatch = errors.New("wal: schema is not an additive extension of the segment schema")
	ErrCorruptSegment = errors.New("wal: corrupt sealed segment")
	ErrWALFailed      = errors.New("wal: failed after write or fsync error; reopen required")
	ErrLocked         = errors.New("wal: directory locked by another process")
	ErrClosed         = errors.New("wal: closed")
)

const (
	defaultMaxRecordBytes = 1 << 20
	defaultSegmentBytes   = 128 << 20
	maxBatchFrames        = 4096
	maxBatchBytes         = 4 << 20
)

// Field is one schema slot.
type Field struct {
	Key   string
	Sigil byte // '=', '~', '?', '"', '<', '@', '^'
}

// Schema is the ordered field list records are encoded with.
type Schema struct{ Fields []Field }

// SyncPolicy controls when Append returns relative to fsync.
type SyncPolicy int

const (
	SyncGroup      SyncPolicy = iota // default: Append blocks until a shared sync covers its frame
	SyncEachAppend                   // a sync per Append
	SyncManual                       // caller invokes Sync; for bulk tools and tests
)

// Options tune durability and sizing. The zero value is valid.
type Options struct {
	Sync           SyncPolicy
	GroupWindow    time.Duration // extra wait to grow a SyncGroup batch; 0 batches what is already queued
	MaxRecordBytes int           // default 1 MiB
	SegmentBytes   int64         // rotate threshold; default 128 MiB

	fs fileSystem
}

func (o Options) withDefaults() Options {
	if o.MaxRecordBytes <= 0 {
		o.MaxRecordBytes = defaultMaxRecordBytes
	}
	if o.SegmentBytes <= 0 {
		o.SegmentBytes = defaultSegmentBytes
	}
	if o.fs == nil {
		o.fs = osFS{}
	}
	return o
}

// Offset addresses one frame. The zero Offset precedes every record.
type Offset struct {
	Segment uint64 // segment sequence number
	Pos     uint64 // byte offset of the frame header within the segment file
}

// Less orders offsets by segment, then position.
func (o Offset) Less(p Offset) bool {
	if o.Segment != p.Segment {
		return o.Segment < p.Segment
	}
	return o.Pos < p.Pos
}

// RecoveryReport describes what Open found and repaired.
type RecoveryReport struct {
	Segments       int
	Records        uint64 // valid records in the live segment
	TruncatedBytes int64
	SealedOnOpen   []uint64 // non-highest unsealed segments sealed during Open
	CreatedLive    bool     // highest segment was sealed; a new live segment was created
	SchemaRotated  bool     // schema was additive-different; a new segment started with it
	Last           Offset
}

type reqKind int

const (
	reqAppend reqKind = iota
	reqSync
	reqRotate
	reqClose
)

type request struct {
	kind  reqKind
	frame []byte
	reply chan reply
}

type reply struct {
	off Offset
	err error
}

// WAL is an open log directory. All file writes happen on one flusher
// goroutine; appenders hand it encoded frames and wait for the result.
type WAL struct {
	dir    string
	opts   Options
	fs     fileSystem
	lock   *os.File
	schema Schema
	encs   sync.Pool

	reqs chan request
	done chan struct{} // closed when the flusher exits after a successful Close

	// Flusher-owned.
	live     segmentFile
	liveSeg  uint64
	writeEnd int64
	lastOff  Offset // last written frame
	wbuf     []byte

	mu         sync.Mutex
	durableSeg uint64
	durableEnd int64
	last       Offset // last durable frame
	failed     error
	closed     bool
}

// Open creates dir if needed, takes its lock, and recovers per FORMAT.md.
func Open(dir string, schema Schema, opts Options) (*WAL, RecoveryReport, error) {
	if err := schema.validate(); err != nil {
		return nil, RecoveryReport{}, err
	}
	opts = opts.withDefaults()
	if err := opts.fs.MkdirAll(dir); err != nil {
		return nil, RecoveryReport{}, err
	}
	lock, err := lockDir(dir)
	if err != nil {
		return nil, RecoveryReport{}, err
	}
	keys, sigils := schema.keys(), schema.sigils()
	w := &WAL{
		dir:    dir,
		opts:   opts,
		fs:     opts.fs,
		lock:   lock,
		schema: schema,
		reqs:   make(chan request),
		done:   make(chan struct{}),
	}
	w.encs.New = func() any { return nxs.NewObjectEncoder(keys, sigils) }
	rep, err := w.recover()
	if err != nil {
		if w.live != nil {
			_ = w.live.Close()
		}
		_ = unlockDir(lock)
		return nil, RecoveryReport{}, err
	}
	w.durableSeg, w.durableEnd, w.last = w.liveSeg, w.writeEnd, w.lastOff
	rep.Last = w.last
	go w.run()
	return w, rep, nil
}

func corrupt(seg uint64, err error) error {
	return fmt.Errorf("%w: segment %d: %v", ErrCorruptSegment, seg, err)
}

func (w *WAL) path(seg uint64) string { return filepath.Join(w.dir, segmentName(seg)) }

func (w *WAL) recover() (RecoveryReport, error) {
	var rep RecoveryReport
	segs, tmps, err := listSegments(w.fs, w.dir)
	if err != nil {
		return rep, err
	}

	// Read-only checks first so a mismatch or corruption leaves files untouched.
	headers := make([]segHeader, len(segs))
	for i, n := range segs {
		f, err := w.fs.OpenFile(w.path(n), os.O_RDONLY)
		if err != nil {
			return rep, err
		}
		headers[i], err = readHeader(f)
		if err == nil && i < len(segs)-1 && !headers[i].sealed() {
			var size int64
			if size, err = f.Size(); err == nil {
				_, _, _, err = scanFrames(f, headers[i].length, size)
			}
		}
		_ = f.Close()
		if err != nil {
			return rep, corrupt(n, err)
		}
	}
	sameSchema := true
	if len(segs) > 0 {
		top := headers[len(segs)-1].schema
		sameSchema = w.schema.equal(top)
		if !sameSchema && !w.schema.IsAdditiveOf(top) {
			return rep, ErrSchemaMismatch
		}
	}

	for _, name := range tmps {
		if err := w.fs.Remove(filepath.Join(w.dir, name)); err != nil {
			return rep, err
		}
	}
	if len(tmps) > 0 {
		if err := w.fs.SyncDir(w.dir); err != nil {
			return rep, err
		}
	}

	if len(segs) == 0 {
		rep.Segments = 1
		return rep, w.startSegment(1, Offset{})
	}

	for i, n := range segs[:len(segs)-1] {
		if headers[i].sealed() {
			continue
		}
		if err := w.sealFile(n, headers[i].flags); err != nil {
			return rep, err
		}
		rep.SealedOnOpen = append(rep.SealedOnOpen, n)
	}

	topSeg := segs[len(segs)-1]
	rep.Segments = len(segs)
	if headers[len(segs)-1].sealed() {
		last, err := w.lastFrameBefore(topSeg + 1)
		if err != nil {
			return rep, err
		}
		rep.CreatedLive = true
		rep.SchemaRotated = !sameSchema
		rep.Segments++
		return rep, w.startSegment(topSeg+1, last)
	}

	f, err := w.fs.OpenFile(w.path(topSeg), os.O_RDWR)
	if err != nil {
		return rep, err
	}
	w.live, w.liveSeg = f, topSeg
	size, err := f.Size()
	if err != nil {
		return rep, err
	}
	end, lastPos, count, err := scanFrames(f, headers[len(segs)-1].length, size)
	if err != nil && !errors.Is(err, errInvalidFrame) {
		return rep, err
	}
	if end < size {
		if err := f.Truncate(end); err != nil {
			return rep, err
		}
		rep.TruncatedBytes = size - end
	}
	// Frames that survived in the page cache become durable before anything
	// is acknowledged after them.
	if err := f.Sync(); err != nil {
		return rep, err
	}
	rep.Records = count
	w.writeEnd = end
	if lastPos >= 0 {
		w.lastOff = Offset{Segment: topSeg, Pos: uint64(lastPos)}
	} else if w.lastOff, err = w.lastFrameBefore(topSeg); err != nil {
		return rep, err
	}

	if !sameSchema {
		rep.SchemaRotated = true
		rep.Segments++
		if err := w.sealLive(); err != nil {
			return rep, err
		}
		return rep, w.startSegment(topSeg+1, w.lastOff)
	}
	return rep, nil
}

// lastFrameBefore finds the last frame in sealed segments below seg.
func (w *WAL) lastFrameBefore(seg uint64) (Offset, error) {
	for n := seg - 1; n > 0; n-- {
		f, err := w.fs.OpenFile(w.path(n), os.O_RDONLY)
		if errors.Is(err, os.ErrNotExist) {
			return Offset{}, nil
		}
		if err != nil {
			return Offset{}, err
		}
		h, err := readHeader(f)
		var size, last int64
		if err == nil {
			size, err = f.Size()
		}
		if err == nil {
			_, last, _, err = scanFrames(f, h.length, size)
		}
		_ = f.Close()
		if err != nil {
			return Offset{}, corrupt(n, err)
		}
		if last >= 0 {
			return Offset{Segment: n, Pos: uint64(last)}, nil
		}
	}
	return Offset{}, nil
}

func (w *WAL) sealFile(seg uint64, flags uint16) error {
	f, err := w.fs.OpenFile(w.path(seg), os.O_RDWR)
	if err != nil {
		return err
	}
	err = writeFlags(f, flags|flagSealed)
	if err == nil {
		err = f.Sync()
	}
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	return err
}

// sealLive syncs the live segment and sets its SEALED flag (rotation steps 1–2).
func (w *WAL) sealLive() error {
	if err := w.live.DataSync(); err != nil {
		return err
	}
	if err := writeFlags(w.live, liveFlags|flagSealed); err != nil {
		return err
	}
	return w.live.Sync()
}

// startSegment creates segment seg and makes it live (rotation steps 3–4).
func (w *WAL) startSegment(seg uint64, last Offset) error {
	f, hdrLen, err := createSegment(w.fs, w.dir, seg, w.schema)
	if err != nil {
		return err
	}
	if w.live != nil {
		_ = w.live.Close()
	}
	w.live, w.liveSeg, w.writeEnd, w.lastOff = f, seg, hdrLen, last
	return nil
}

// ── Public API ───────────────────────────────────────────────────────────────

// Append encodes one record via fill and returns once it is durable per
// Options.Sync. Concurrency-safe. It returns ErrRecordTooLarge or an encoding
// error (the WAL stays usable), ErrWALFailed, ErrClosed, or ctx.Err(). Once a
// record is handed to the flusher, Append waits for its outcome.
func (w *WAL) Append(ctx context.Context, fill func(rec *nxs.ObjectEncoder)) (Offset, error) {
	if err := ctx.Err(); err != nil {
		return Offset{}, err
	}
	if err := w.failedErr(); err != nil {
		return Offset{}, ErrWALFailed
	}
	enc := w.encs.Get().(*nxs.ObjectEncoder)
	enc.Reset()
	fill(enc)
	obj, err := enc.Bytes()
	if err != nil {
		w.encs.Put(enc)
		return Offset{}, fmt.Errorf("wal: encode record: %w", err)
	}
	if len(obj) > w.opts.MaxRecordBytes {
		w.encs.Put(enc)
		return Offset{}, ErrRecordTooLarge
	}
	frame := appendFrame(make([]byte, 0, frameHeaderLen+len(obj)), obj)
	w.encs.Put(enc)
	return w.call(ctx, reqAppend, frame)
}

// Sync makes every written record durable. Under SyncGroup and
// SyncEachAppend acknowledged records already are.
func (w *WAL) Sync() error {
	_, err := w.call(context.Background(), reqSync, nil)
	return err
}

// Rotate seals the live segment and starts the next one. It returns the
// offset of the last record in the sealed segment (or earlier), or the zero
// Offset if the WAL holds no records.
func (w *WAL) Rotate() (sealed Offset, err error) {
	return w.call(context.Background(), reqRotate, nil)
}

// Last returns the offset of the last durable record.
func (w *WAL) Last() Offset {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.last
}

// Close syncs and closes. In the failed state it instead truncates the live
// segment to the durable end and syncs; on error it stays failed and Close
// may be retried. Idempotent once it succeeds.
func (w *WAL) Close() error {
	w.mu.Lock()
	closed := w.closed
	w.mu.Unlock()
	if closed {
		return nil
	}
	r := request{kind: reqClose, reply: make(chan reply, 1)}
	select {
	case w.reqs <- r:
	case <-w.done:
		return nil
	}
	return (<-r.reply).err
}

func (w *WAL) call(ctx context.Context, kind reqKind, frame []byte) (Offset, error) {
	r := request{kind: kind, frame: frame, reply: make(chan reply, 1)}
	select {
	case w.reqs <- r:
	case <-w.done:
		return Offset{}, ErrClosed
	case <-ctx.Done():
		return Offset{}, ctx.Err()
	}
	rep := <-r.reply
	return rep.off, rep.err
}

func (w *WAL) failedErr() error {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.failed
}

// fail moves the WAL to the failed state and returns the error for waiters.
func (w *WAL) fail(err error) error {
	w.mu.Lock()
	if w.failed == nil {
		w.failed = err
	}
	w.mu.Unlock()
	return fmt.Errorf("%w: %w", ErrWALFailed, err)
}

func (w *WAL) publishDurable() {
	w.mu.Lock()
	w.durableSeg, w.durableEnd, w.last = w.liveSeg, w.writeEnd, w.lastOff
	w.mu.Unlock()
}

func (w *WAL) durableState() (seg uint64, end int64) {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.durableSeg, w.durableEnd
}

// ── Flusher ──────────────────────────────────────────────────────────────────

func (w *WAL) run() {
	var batch []request
	for {
		batch = append(batch[:0], <-w.reqs)
		if batch[0].kind == reqAppend && w.opts.Sync != SyncEachAppend {
			batch = w.collect(batch)
		}
		if w.process(batch) {
			close(w.done)
			return
		}
	}
}

// collect adds requests that are already waiting (and, with GroupWindow, ones
// that arrive within it). A control request ends the batch.
func (w *WAL) collect(batch []request) []request {
	var window <-chan time.Time
	if w.opts.Sync == SyncGroup && w.opts.GroupWindow > 0 {
		t := time.NewTimer(w.opts.GroupWindow)
		defer t.Stop()
		window = t.C
	}
	bytes := len(batch[0].frame)
	for len(batch) < maxBatchFrames && bytes < maxBatchBytes {
		var r request
		select {
		case r = <-w.reqs:
		default:
			if window == nil {
				return batch
			}
			select {
			case r = <-w.reqs:
			case <-window:
				return batch
			}
		}
		batch = append(batch, r)
		if r.kind != reqAppend {
			return batch
		}
		bytes += len(r.frame)
	}
	return batch
}

// process handles a batch in order and reports whether the WAL closed.
func (w *WAL) process(batch []request) bool {
	for i := 0; i < len(batch); {
		if batch[i].kind == reqAppend {
			j := i
			for j < len(batch) && batch[j].kind == reqAppend {
				j++
			}
			w.appendFrames(batch[i:j])
			i = j
			continue
		}
		r := batch[i]
		i++
		switch r.kind {
		case reqSync:
			r.reply <- reply{err: w.doSync()}
		case reqRotate:
			off, err := w.doRotate()
			r.reply <- reply{off: off, err: err}
		case reqClose:
			err := w.doClose()
			r.reply <- reply{err: err}
			if err == nil {
				for _, rest := range batch[i:] {
					rest.reply <- reply{err: ErrClosed}
				}
				return true
			}
		}
	}
	return false
}

func failAll(reqs []request, err error) {
	for _, r := range reqs {
		r.reply <- reply{err: err}
	}
}

func (w *WAL) appendFrames(reqs []request) {
	if w.failedErr() != nil {
		failAll(reqs, ErrWALFailed)
		return
	}
	offs := make([]Offset, len(reqs))
	buf := w.wbuf[:0]
	pos := w.writeEnd
	for i, r := range reqs {
		offs[i] = Offset{Segment: w.liveSeg, Pos: uint64(pos)}
		buf = append(buf, r.frame...)
		pos += int64(len(r.frame))
	}
	w.wbuf = buf
	n, err := w.live.WriteAt(buf, w.writeEnd)
	if err == nil && n < len(buf) {
		err = errShortWrite
	}
	if err != nil {
		failAll(reqs, w.fail(err))
		return
	}
	w.writeEnd = pos
	w.lastOff = offs[len(offs)-1]
	if w.opts.Sync != SyncManual {
		if err := w.live.DataSync(); err != nil {
			failAll(reqs, w.fail(err))
			return
		}
		w.publishDurable()
	}
	for i, r := range reqs {
		r.reply <- reply{off: offs[i]}
	}
	if w.writeEnd >= w.opts.SegmentBytes {
		// A failure here leaves the WAL failed; the records above stay acknowledged.
		_, _ = w.doRotate()
	}
}

func (w *WAL) doSync() error {
	if w.failedErr() != nil {
		return ErrWALFailed
	}
	_, durableEnd := w.durableState()
	if w.writeEnd == durableEnd {
		return nil
	}
	if err := w.live.DataSync(); err != nil {
		return w.fail(err)
	}
	w.publishDurable()
	return nil
}

func (w *WAL) doRotate() (Offset, error) {
	if w.failedErr() != nil {
		return Offset{}, ErrWALFailed
	}
	if err := w.sealLive(); err != nil {
		return Offset{}, w.fail(err)
	}
	w.publishDurable()
	sealed := w.lastOff
	if err := w.startSegment(w.liveSeg+1, w.lastOff); err != nil {
		return Offset{}, w.fail(err)
	}
	w.publishDurable()
	return sealed, nil
}

func (w *WAL) doClose() error {
	if w.failedErr() != nil {
		_, durableEnd := w.durableState()
		if err := w.live.Truncate(durableEnd); err != nil {
			return fmt.Errorf("wal: truncate failed WAL: %w", err)
		}
		if err := w.live.Sync(); err != nil {
			return fmt.Errorf("wal: sync failed WAL: %w", err)
		}
		w.writeEnd = durableEnd
	} else if err := w.doSync(); err != nil {
		return err
	}
	_ = w.live.Close()
	w.mu.Lock()
	w.closed = true
	w.mu.Unlock()
	return unlockDir(w.lock)
}
