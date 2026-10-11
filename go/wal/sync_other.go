//go:build !darwin && !linux

package wal

import "os"

func dataSync(f *os.File) error { return f.Sync() }
func fullSync(f *os.File) error { return f.Sync() }
