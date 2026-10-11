//go:build !darwin && !linux

package wal

import (
	"errors"
	"os"
)

func lockDir(dir string) (*os.File, error) {
	return nil, errors.New("wal: directory locking is not supported on this platform")
}

func unlockDir(f *os.File) error { return f.Close() }
