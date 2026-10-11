// Package wal is a durable, crash-safe append-only log of NYXO objects
// (.nxsw v2). See FORMAT.md for the on-disk layout and recovery rules.
package wal

import (
	"context"
	"errors"
	"time"

	nxs "github.com/nyxis-io/nyxis-drivers/go"
)

var errNotImplemented = errors.New("wal: not implemented")

var (
	ErrRecordTooLarge = errors.New("wal: record exceeds MaxRecordBytes")
	ErrSchemaMismatch = errors.New("wal: schema is not an additive extension of the segment schema")
	ErrCorruptSegment = errors.New("wal: corrupt sealed segment")
	ErrWALFailed      = errors.New("wal: failed after write or fsync error; reopen required")
	ErrLocked         = errors.New("wal: directory locked by another process")
	ErrClosed         = errors.New("wal: closed")
)

// Field is one schema slot.
type Field struct {
	Key   string
	Sigil byte // '=', '~', '?', '"', '<', '@', '^'
}

// Schema is the ordered field list records are encoded with.
type Schema struct{ Fields []Field }

// IsAdditiveOf reports whether s equals prev plus zero or more trailing fields.
func (s Schema) IsAdditiveOf(prev Schema) bool { return false }

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

// Offset addresses one frame.
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
	SealedOnOpen   []uint64
	CreatedLive    bool
	SchemaRotated  bool
	Last           Offset
}

// WAL is an open log directory.
type WAL struct{}

// Open creates dir if needed, takes its lock, and recovers per FORMAT.md.
func Open(dir string, schema Schema, opts Options) (*WAL, RecoveryReport, error) {
	return nil, RecoveryReport{}, errNotImplemented
}

// Append encodes one record via fill and returns once it is durable per
// Options.Sync.
func (w *WAL) Append(ctx context.Context, fill func(rec *nxs.ObjectEncoder)) (Offset, error) {
	return Offset{}, errNotImplemented
}

func (w *WAL) Sync() error                        { return errNotImplemented }
func (w *WAL) Rotate() (sealed Offset, err error) { return Offset{}, errNotImplemented }
func (w *WAL) Last() Offset                       { return Offset{} }
func (w *WAL) Close() error                       { return errNotImplemented }

// Scanner reads frames in order across segments.
type Scanner struct{}

func OpenScanner(dir string, after Offset) (*Scanner, error) { return nil, errNotImplemented }

func (w *WAL) NewLiveScanner(after Offset) (*Scanner, error) { return nil, errNotImplemented }

func (s *Scanner) Next() bool          { return false }
func (s *Scanner) Record() *nxs.Object { return nil }
func (s *Scanner) Schema() Schema      { return Schema{} }
func (s *Scanner) Offset() Offset      { return Offset{} }
func (s *Scanner) Err() error          { return errNotImplemented }
func (s *Scanner) Close() error        { return nil }
