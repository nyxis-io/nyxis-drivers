package nxs

import "fmt"

// PrefetchColumn issues a single range fetch for a columnar column buffer (§7.4).
// Row page cache is not used. Idempotent per column slot.
func (r *Reader) PrefetchColumn(key string) error {
	if r.layout != LayoutColumnar {
		return fmt.Errorf("ERR_LAYOUT: prefetch_column requires columnar layout")
	}
	slot, ok := r.keyIndex[key]
	if !ok {
		return fmt.Errorf("ERR_KEY_NOT_FOUND: %q", key)
	}
	r.colMu.Lock()
	defer r.colMu.Unlock()
	if r.colWarmed[slot] {
		return nil
	}
	off := int64(r.colBufOff[slot])
	ln := int64(r.colBufLen[slot])
	blob, err := r.colFetchRange(off, ln)
	if err != nil {
		return err
	}
	if int(off)+len(blob) > len(r.data) {
		r.colOverlay[slot] = blob
	}
	r.colWarmed[slot] = true
	r.colFetches++
	return nil
}

func (r *Reader) initColumnPrefetch(cfg readerConfig) {
	data := r.data
	fetchRange := cfg.fetchRange
	if fetchRange == nil {
		fetchRange = func(off, length int64) ([]byte, error) {
			return sliceInt64(data, off, length)
		}
	}
	r.colFetchRange = fetchRange
	r.colWarmed = make(map[int]bool)
	r.colOverlay = make(map[int][]byte)
}

func (r *Reader) columnSector(slot int) ([]byte, error) {
	if slot < 0 || slot >= len(r.colBufOff) {
		return nil, fmt.Errorf("ERR_KEY_NOT_FOUND")
	}
	off := int(r.colBufOff[slot])
	length := int(r.colBufLen[slot])
	r.colMu.Lock()
	overlay, warmed := r.colOverlay[slot], r.colWarmed[slot]
	r.colMu.Unlock()
	if warmed && len(overlay) > 0 {
		if len(overlay) < length {
			return nil, fmt.Errorf("ERR_OUT_OF_BOUNDS: column overlay")
		}
		return overlay[:length], nil
	}
	if off+length > len(r.data) {
		return nil, fmt.Errorf("ERR_OUT_OF_BOUNDS: column buffer")
	}
	return r.data[off : off+length], nil
}
