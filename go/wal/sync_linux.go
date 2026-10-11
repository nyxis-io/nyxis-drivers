package wal

import (
	"os"
	"syscall"
)

func dataSync(f *os.File) error { return syscall.Fdatasync(int(f.Fd())) }
func fullSync(f *os.File) error { return f.Sync() }
