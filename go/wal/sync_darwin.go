package wal

import "os"

// os.File.Sync issues fcntl(F_FULLFSYNC) on darwin; plain fsync leaves data in
// the drive cache.
func dataSync(f *os.File) error { return f.Sync() }
func fullSync(f *os.File) error { return f.Sync() }
