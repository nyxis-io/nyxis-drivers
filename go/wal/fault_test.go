package wal

import (
	"errors"
	"fmt"
	"path/filepath"
	"strings"
	"sync"
)

var errInjected = errors.New("injected I/O error")

type opKind string

const (
	opWrite    opKind = "write"
	opDataSync opKind = "datasync"
	opSync     opKind = "sync"
	opTruncate opKind = "truncate"
	opRename   opKind = "rename"
	opRemove   opKind = "remove"
	opSyncDir  opKind = "syncdir"
	opNote     opKind = "note"
)

// fsOp is one recorded filesystem call. name is a base name ("old->new" for
// renames). For syncs, extent is how many bytes had been written to the file.
type fsOp struct {
	kind   opKind
	name   string
	off    int64
	n      int
	extent int64
	note   string
}

func (o fsOp) String() string {
	switch o.kind {
	case opWrite:
		return fmt.Sprintf("write %s @%d+%d", o.name, o.off, o.n)
	case opNote:
		return "note " + o.note
	default:
		return fmt.Sprintf("%s %s", o.kind, o.name)
	}
}

// faultFS wraps osFS, records every mutating call, and lets a test inject
// failures. fault runs before the call; a non-nil error fails it, and for
// writes the returned count is how many bytes reach the file first.
type faultFS struct {
	inner  fileSystem
	mu     sync.Mutex
	ops    []fsOp
	extent map[string]int64
	fault  func(op fsOp) (int, error)
}

func newFaultFS() *faultFS {
	return &faultFS{inner: osFS{}, extent: map[string]int64{}}
}

func (f *faultFS) setFault(fn func(op fsOp) (int, error)) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.fault = fn
}

func (f *faultFS) record(op fsOp) (int, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if op.kind == opDataSync || op.kind == opSync {
		op.extent = f.extent[op.name]
	}
	f.ops = append(f.ops, op)
	if f.fault != nil {
		return f.fault(op)
	}
	return 0, nil
}

func (f *faultFS) note(s string) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.ops = append(f.ops, fsOp{kind: opNote, note: s})
}

func (f *faultFS) snapshot() []fsOp {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]fsOp(nil), f.ops...)
}

func (f *faultFS) resetOps() {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.ops = nil
}

func (f *faultFS) grow(name string, end int64) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if end > f.extent[name] {
		f.extent[name] = end
	}
}

func (f *faultFS) setExtent(name string, size int64) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.extent[name] = size
}

func (f *faultFS) MkdirAll(dir string) error            { return f.inner.MkdirAll(dir) }
func (f *faultFS) ReadDir(dir string) ([]string, error) { return f.inner.ReadDir(dir) }

func (f *faultFS) OpenFile(name string, flag int) (segmentFile, error) {
	inner, err := f.inner.OpenFile(name, flag)
	if err != nil {
		return nil, err
	}
	base := filepath.Base(name)
	if size, err := inner.Size(); err == nil {
		f.grow(base, size)
	}
	return &faultFile{fs: f, inner: inner, name: base}, nil
}

func (f *faultFS) Rename(oldpath, newpath string) error {
	from, to := filepath.Base(oldpath), filepath.Base(newpath)
	if _, err := f.record(fsOp{kind: opRename, name: from + "->" + to}); err != nil {
		return err
	}
	if err := f.inner.Rename(oldpath, newpath); err != nil {
		return err
	}
	f.mu.Lock()
	f.extent[to] = f.extent[from]
	delete(f.extent, from)
	f.mu.Unlock()
	return nil
}

func (f *faultFS) Remove(name string) error {
	if _, err := f.record(fsOp{kind: opRemove, name: filepath.Base(name)}); err != nil {
		return err
	}
	return f.inner.Remove(name)
}

func (f *faultFS) SyncDir(dir string) error {
	if _, err := f.record(fsOp{kind: opSyncDir, name: filepath.Base(dir)}); err != nil {
		return err
	}
	return f.inner.SyncDir(dir)
}

type faultFile struct {
	fs    *faultFS
	inner segmentFile
	name  string
}

func (f *faultFile) ReadAt(p []byte, off int64) (int, error) { return f.inner.ReadAt(p, off) }
func (f *faultFile) Size() (int64, error)                    { return f.inner.Size() }
func (f *faultFile) Close() error                            { return f.inner.Close() }

func (f *faultFile) WriteAt(p []byte, off int64) (int, error) {
	allow, err := f.fs.record(fsOp{kind: opWrite, name: f.name, off: off, n: len(p)})
	if err != nil {
		if allow > 0 {
			n, _ := f.inner.WriteAt(p[:allow], off)
			f.fs.grow(f.name, off+int64(n))
			return n, err
		}
		return 0, err
	}
	n, err := f.inner.WriteAt(p, off)
	f.fs.grow(f.name, off+int64(n))
	return n, err
}

func (f *faultFile) DataSync() error {
	if _, err := f.fs.record(fsOp{kind: opDataSync, name: f.name}); err != nil {
		return err
	}
	return f.inner.DataSync()
}

func (f *faultFile) Sync() error {
	if _, err := f.fs.record(fsOp{kind: opSync, name: f.name}); err != nil {
		return err
	}
	return f.inner.Sync()
}

func (f *faultFile) Truncate(size int64) error {
	if _, err := f.fs.record(fsOp{kind: opTruncate, name: f.name, off: size}); err != nil {
		return err
	}
	if err := f.inner.Truncate(size); err != nil {
		return err
	}
	f.fs.setExtent(f.name, size)
	return nil
}

func isSegmentName(name string) bool {
	return strings.HasSuffix(name, ".nxsw")
}
