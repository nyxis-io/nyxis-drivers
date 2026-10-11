package wal

import (
	"os"
)

// fileSystem is the I/O seam under the WAL. Production uses osFS; tests wrap
// it to inject short writes and sync, truncate, or rename errors and to
// record the order of durable operations.
type fileSystem interface {
	MkdirAll(dir string) error
	ReadDir(dir string) ([]string, error)
	OpenFile(name string, flag int) (segmentFile, error)
	Rename(oldpath, newpath string) error
	Remove(name string) error
	SyncDir(dir string) error
}

type segmentFile interface {
	ReadAt(p []byte, off int64) (int, error)
	WriteAt(p []byte, off int64) (int, error)
	Truncate(size int64) error
	Size() (int64, error)
	DataSync() error // frames
	Sync() error     // headers and other metadata changes
	Close() error
}

type osFS struct{}

func (osFS) MkdirAll(dir string) error { return os.MkdirAll(dir, 0o755) }

func (osFS) ReadDir(dir string) ([]string, error) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil, err
	}
	names := make([]string, len(entries))
	for i, e := range entries {
		names[i] = e.Name()
	}
	return names, nil
}

func (osFS) OpenFile(name string, flag int) (segmentFile, error) {
	f, err := os.OpenFile(name, flag, 0o644)
	if err != nil {
		return nil, err
	}
	return osFile{f}, nil
}

func (osFS) Rename(oldpath, newpath string) error { return os.Rename(oldpath, newpath) }
func (osFS) Remove(name string) error             { return os.Remove(name) }

func (osFS) SyncDir(dir string) error {
	d, err := os.Open(dir)
	if err != nil {
		return err
	}
	if err := d.Sync(); err != nil {
		_ = d.Close()
		return err
	}
	return d.Close()
}

type osFile struct{ *os.File }

func (f osFile) Size() (int64, error) {
	st, err := f.Stat()
	if err != nil {
		return 0, err
	}
	return st.Size(), nil
}

func (f osFile) DataSync() error { return dataSync(f.File) }
func (f osFile) Sync() error     { return fullSync(f.File) }
