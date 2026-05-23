package nxs

import (
	"fmt"
	"sort"
	"sync"
)

// Adaptive prefetch constants (spec §6–§8.4).
const (
	DefaultPageSize         = 65536
	DefaultMaxPages         = 256
	DefaultCoalesceGapPages = 1
)

// AccessHint is advisory only in phase 1 (stored, not acted on).
type AccessHint uint8

const (
	HintUnknown AccessHint = iota
	HintSequential
	HintRandom
	HintFull
	HintPartial
)

// CacheStats reports page-cache and prefetch counters.
type CacheStats struct {
	PagesCached     int
	PagesMax        int
	MemoryUsedBytes int
	CacheHits       int
	CacheMisses     int
	FetchesIssued   int
	Strategy        string
	Pattern         string
}

// PageRange is an inclusive page index span with byte offsets.
type PageRange struct {
	PageStart  int
	PageEnd    int
	ByteStart  int64
	ByteLength int64
}

// sliceInt64 returns data[off:off+length] with bounds checks (Go slice indices are int).
func sliceInt64(data []byte, off, length int64) ([]byte, error) {
	if off < 0 || length < 0 || off > int64(len(data)) {
		return nil, fmt.Errorf("ERR_OUT_OF_BOUNDS: fetch range [%d, %d)", off, off+length)
	}
	end := off + length
	if end > int64(len(data)) {
		end = int64(len(data))
	}
	if end <= off {
		return nil, fmt.Errorf("ERR_OUT_OF_BOUNDS: empty fetch range")
	}
	return data[int(off):int(end)], nil
}

func intFromInt64(n int64) (int, error) {
	if n < 0 || n > int64(int(^uint(0)>>1)) {
		return 0, fmt.Errorf("ERR_OUT_OF_BOUNDS: length %d", n)
	}
	return int(n), nil
}

// CoalescePageIndices merges page indices into byte ranges where the gap between
// consecutive indices is at most gapPages (Adaptive-prefetch-spec §7.2).
func CoalescePageIndices(indices []int, gapPages, pageSize int) []PageRange {
	if len(indices) == 0 {
		return nil
	}
	seen := make(map[int]struct{}, len(indices))
	uniq := make([]int, 0, len(indices))
	for _, p := range indices {
		if _, ok := seen[p]; ok {
			continue
		}
		seen[p] = struct{}{}
		uniq = append(uniq, p)
	}
	sort.Ints(uniq)

	var spans [][2]int
	start := uniq[0]
	end := uniq[0]
	for i := 1; i < len(uniq); i++ {
		if uniq[i]-end <= gapPages {
			end = uniq[i]
			continue
		}
		spans = append(spans, [2]int{start, end})
		start = uniq[i]
		end = uniq[i]
	}
	spans = append(spans, [2]int{start, end})

	out := make([]PageRange, 0, len(spans))
	for _, s := range spans {
		out = append(out, PageRange{
			PageStart:  s[0],
			PageEnd:    s[1],
			ByteStart:  int64(s[0]) * int64(pageSize),
			ByteLength: int64(s[1]-s[0]+1) * int64(pageSize),
		})
	}
	return out
}

func clampPageRanges(ranges []PageRange, fileSize int64) []PageRange {
	out := make([]PageRange, 0, len(ranges))
	for _, r := range ranges {
		length := r.ByteLength
		if r.ByteStart+length > fileSize {
			length = fileSize - r.ByteStart
		}
		if length <= 0 {
			continue
		}
		r.ByteLength = length
		out = append(out, r)
	}
	return out
}

func pageIndicesForViewport(startIndex, endIndex, pageSize int, recordOffset func(int) int64) []int {
	out := make([]int, 0, endIndex-startIndex+1)
	for i := startIndex; i <= endIndex; i++ {
		off := recordOffset(i)
		out = append(out, int(off/int64(pageSize)))
	}
	return out
}

type pageEntry struct {
	data     []byte
	lastUsed int
	pinned   bool
}

// PageCache is an LRU page cache with optional pinning.
type PageCache struct {
	maxPages int
	pageSize int
	pages    map[int]*pageEntry
	clock    int
	hits     int
	misses   int
}

func newPageCache(maxPages, pageSize int) *PageCache {
	return &PageCache{
		maxPages: maxPages,
		pageSize: pageSize,
		pages:    make(map[int]*pageEntry),
	}
}

func (c *PageCache) has(pageIndex int) bool {
	_, ok := c.pages[pageIndex]
	return ok
}

func (c *PageCache) get(pageIndex int) []byte {
	e, ok := c.pages[pageIndex]
	if !ok {
		c.misses++
		return nil
	}
	c.clock++
	e.lastUsed = c.clock
	c.hits++
	return e.data
}

func (c *PageCache) set(pageIndex int, data []byte, pinned bool) {
	if c.maxPages <= 0 {
		return
	}
	for len(c.pages) >= c.maxPages {
		if !c.evictOne() {
			break
		}
	}
	c.clock++
	c.pages[pageIndex] = &pageEntry{
		data:     data,
		lastUsed: c.clock,
		pinned:   pinned,
	}
}

func (c *PageCache) evictOne() bool {
	oldest := int(^uint(0) >> 1)
	victim := -1
	for idx, e := range c.pages {
		if e.pinned {
			continue
		}
		if e.lastUsed < oldest {
			oldest = e.lastUsed
			victim = idx
		}
	}
	if victim < 0 {
		return false
	}
	delete(c.pages, victim)
	return true
}

func (c *PageCache) pinPages(pageIndices []int) {
	for _, p := range pageIndices {
		if e, ok := c.pages[p]; ok {
			e.pinned = true
		}
	}
}

func (c *PageCache) unpinAll() {
	for _, e := range c.pages {
		e.pinned = false
	}
}

func (c *PageCache) stats() (pagesCached, memoryUsed int) {
	for _, e := range c.pages {
		memoryUsed += len(e.data)
	}
	return len(c.pages), memoryUsed
}

// inFlightMap deduplicates concurrent page fetches.
type inFlightMap struct {
	mu sync.Mutex
	m  map[int]*inFlightEntry
}

type inFlightEntry struct {
	wg   sync.WaitGroup
	data []byte
	err  error
}

func newInFlightMap() *inFlightMap {
	return &inFlightMap{m: make(map[int]*inFlightEntry)}
}

func (f *inFlightMap) has(pageIndex int) bool {
	f.mu.Lock()
	defer f.mu.Unlock()
	_, ok := f.m[pageIndex]
	return ok
}

func (f *inFlightMap) wait(pageIndex int) ([]byte, error) {
	f.mu.Lock()
	entry, ok := f.m[pageIndex]
	f.mu.Unlock()
	if !ok {
		return nil, nil
	}
	entry.wg.Wait()
	return entry.data, entry.err
}

func (f *inFlightMap) begin(pageIndex int) (*inFlightEntry, bool) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if entry, ok := f.m[pageIndex]; ok {
		return entry, false
	}
	entry := &inFlightEntry{}
	entry.wg.Add(1)
	f.m[pageIndex] = entry
	return entry, true
}

func (f *inFlightMap) finish(pageIndex int, entry *inFlightEntry) {
	entry.wg.Done()
	f.mu.Lock()
	if f.m[pageIndex] == entry {
		delete(f.m, pageIndex)
	}
	f.mu.Unlock()
}
