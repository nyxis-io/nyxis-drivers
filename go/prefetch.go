package nxs

import (
	"context"
	"fmt"
	"sort"
	"sync"
	"sync/atomic"
)

// Adaptive prefetch constants (spec §6–§8.4).
const (
	DefaultPageSize         = 65536
	DefaultMaxPages         = 256
	DefaultCoalesceGapPages = 1
	DefaultPrefetchDepth    = 4
	EagerThresholdMB        = 10
	LazyThresholdMB         = 50
)

// PrefetchStrategy is the active prefetch mode (spec §5).
type PrefetchStrategy int

const (
	StrategyLazy PrefetchStrategy = iota
	StrategyAdaptive
	StrategyEager
)

func (s PrefetchStrategy) String() string {
	switch s {
	case StrategyAdaptive:
		return "adaptive"
	case StrategyEager:
		return "eager"
	default:
		return "lazy"
	}
}

// OpenOptions configures prefetch at open time.
type OpenOptions struct {
	Hint             AccessHint
	MaxPages         int
	PageSize         int
	CoalesceGapPages int
	PrefetchDepth    int
}

// DefaultOpenOptions returns spec defaults.
func DefaultOpenOptions() OpenOptions {
	return OpenOptions{
		Hint:             HintUnknown,
		MaxPages:         DefaultMaxPages,
		PageSize:         DefaultPageSize,
		CoalesceGapPages: DefaultCoalesceGapPages,
		PrefetchDepth:    DefaultPrefetchDepth,
	}
}

// Validate checks open-time options.
func (o OpenOptions) Validate() error {
	if o.PageSize <= 0 {
		return fmt.Errorf("ERR_PARSE: prefetch page_size must be greater than 0")
	}
	return nil
}

// InitialStrategy selects lazy/adaptive/eager from hint and file size (§5.1).
func InitialStrategy(hint AccessHint, fileSize int) PrefetchStrategy {
	fileSizeMB := fileSize / (1024 * 1024)
	if hint == HintFull && fileSizeMB <= EagerThresholdMB {
		return StrategyEager
	}
	if fileSizeMB > LazyThresholdMB {
		return StrategyLazy
	}
	return StrategyAdaptive
}

// RowDataSector returns row-layout data sector byte range [start, length).
func RowDataSector(tailStart, fileSize int) (start, length int) {
	const sectorStart = 32
	if tailStart > sectorStart && tailStart <= fileSize {
		return sectorStart, tailStart - sectorStart
	}
	return sectorStart, 0
}

// AccessHint is advisory for prefetch strategy selection.
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

// prefetchEngine drives page cache, pattern detection, and strategies (spec §4–§8.4).
type prefetchEngine struct {
	mu      sync.Mutex
	cacheMu sync.Mutex

	cache         *PageCache
	inFlight      *inFlightMap
	fetchesIssued int
	options       OpenOptions
	strategy      PrefetchStrategy
	detector      *AccessPatternDetector
	fileSize      int
	tailStart     int
	recordCount   int
	data          []byte
	recordOffset  func(int) int64
	fetchRange    func(off, length int64) ([]byte, error)

	eagerCancel   context.CancelFunc
	eagerDone     chan struct{}
	eagerStarted  bool
	eagerComplete atomic.Bool
	closed        bool
}

func newPrefetchEngine(opts OpenOptions, fileSize, tailStart, recordCount int, data []byte, recordOffset func(int) int64, fetchRange func(off, length int64) ([]byte, error)) *prefetchEngine {
	if opts.MaxPages <= 0 {
		opts.MaxPages = DefaultMaxPages
	}
	if opts.PageSize <= 0 {
		opts.PageSize = DefaultPageSize
	}
	if opts.CoalesceGapPages < 0 {
		opts.CoalesceGapPages = DefaultCoalesceGapPages
	}
	if opts.PrefetchDepth <= 0 {
		opts.PrefetchDepth = DefaultPrefetchDepth
	}
	e := &prefetchEngine{
		cache:        newPageCache(opts.MaxPages, opts.PageSize),
		inFlight:     newInFlightMap(),
		options:      opts,
		strategy:     InitialStrategy(opts.Hint, fileSize),
		detector:     NewAccessPatternDetector(),
		fileSize:     fileSize,
		tailStart:    tailStart,
		recordCount:  recordCount,
		data:         data,
		recordOffset: recordOffset,
		fetchRange:   fetchRange,
	}
	if e.strategy == StrategyEager {
		e.startEagerBackground()
	}
	return e
}

func (e *prefetchEngine) cacheStats() CacheStats {
	e.mu.Lock()
	strategy := e.strategy.String()
	pattern := e.detector.Pattern().String()
	e.mu.Unlock()
	e.cacheMu.Lock()
	defer e.cacheMu.Unlock()
	pagesCached, memoryUsed := e.cache.stats()
	return CacheStats{
		PagesCached:     pagesCached,
		PagesMax:        e.cache.maxPages,
		MemoryUsedBytes: memoryUsed,
		CacheHits:       e.cache.hits,
		CacheMisses:     e.cache.misses,
		FetchesIssued:   e.fetchesIssued,
		Strategy:        strategy,
		Pattern:         pattern,
	}
}

func (e *prefetchEngine) isEagerComplete() bool {
	return e.strategy == StrategyEager && e.eagerComplete.Load()
}

func (e *prefetchEngine) close() {
	e.mu.Lock()
	if e.closed {
		e.mu.Unlock()
		return
	}
	e.closed = true
	cancel := e.eagerCancel
	done := e.eagerDone
	e.mu.Unlock()
	if cancel != nil {
		cancel()
	}
	if done != nil {
		<-done
	}
}

func (e *prefetchEngine) warmup() {
	e.mu.Lock()
	done := e.eagerDone
	e.mu.Unlock()
	if done != nil {
		<-done
	}
}

func (e *prefetchEngine) onAccess(index int) {
	if e.recordCount == 0 {
		return
	}
	e.mu.Lock()
	if e.closed {
		e.mu.Unlock()
		return
	}
	e.detector.Observe(index)
	e.maybeUpgradeToEager()
	eager := e.isEagerComplete() || e.strategy == StrategyEager
	adaptiveSeq := e.strategy == StrategyAdaptive && e.detector.Pattern() == PatternSequential
	e.mu.Unlock()
	if eager {
		return
	}
	if off := e.recordOffset(index); off >= 0 {
		pageIndex := int(off / int64(e.options.PageSize))
		e.cacheMu.Lock()
		_ = e.cache.get(pageIndex)
		e.cacheMu.Unlock()
	}
	if adaptiveSeq {
		e.speculativePrefetch()
	}
}

func (e *prefetchEngine) maybeUpgradeToEager() {
	if e.strategy != StrategyAdaptive {
		return
	}
	if e.detector.Pattern() != PatternSequential {
		return
	}
	if e.detector.SequentialRuns() < UpgradeSequentialThreshold {
		return
	}
	if e.fileSize/(1024*1024) > EagerThresholdMB {
		return
	}
	e.strategy = StrategyEager
	e.startEagerBackground()
}

func (e *prefetchEngine) startEagerBackground() {
	if e.strategy != StrategyEager || e.eagerStarted {
		return
	}
	e.eagerStarted = true
	sectorStart, sectorLen := RowDataSector(e.tailStart, len(e.data))
	if sectorLen == 0 {
		e.eagerComplete.Store(true)
		return
	}
	ctx, cancel := context.WithCancel(context.Background())
	e.eagerCancel = cancel
	done := make(chan struct{})
	e.eagerDone = done

	go func() {
		defer close(done)
		end := sectorStart + sectorLen
		if end > len(e.data) {
			end = len(e.data)
		}
		if sectorStart >= end {
			if ctx.Err() == nil {
				e.eagerComplete.Store(true)
			}
			return
		}
		pageSize := e.options.PageSize
		firstPage := sectorStart / pageSize
		lastPage := (end - 1) / pageSize
		indices := make([]int, 0, lastPage-firstPage+1)
		for p := firstPage; p <= lastPage; p++ {
			indices = append(indices, p)
		}
		ranges := clampPageRanges(
			CoalescePageIndices(indices, e.options.CoalesceGapPages, pageSize),
			int64(len(e.data)),
		)
		for _, pr := range ranges {
			if ctx.Err() != nil {
				return
			}
			if err := e.fetchCoalescedRange(ctx, pr); err != nil {
				return
			}
		}
		if ctx.Err() == nil {
			e.eagerComplete.Store(true)
		}
	}()
}

func (e *prefetchEngine) speculativePrefetch() {
	depth := e.options.PrefetchDepth
	e.mu.Lock()
	predicted := e.detector.PredictNext(depth, e.recordCount)
	e.mu.Unlock()
	if len(predicted) == 0 {
		return
	}
	pageSize := e.options.PageSize
	seen := make(map[int]struct{})
	var pageIndices []int
	e.cacheMu.Lock()
	for _, idx := range predicted {
		off := e.recordOffset(idx)
		if off < 0 {
			continue
		}
		p := int(off / int64(pageSize))
		if _, ok := seen[p]; ok {
			continue
		}
		seen[p] = struct{}{}
		if !e.cache.has(p) && !e.inFlight.has(p) {
			pageIndices = append(pageIndices, p)
		}
	}
	e.cacheMu.Unlock()
	if len(pageIndices) == 0 {
		return
	}
	sort.Ints(pageIndices)
	ranges := clampPageRanges(
		CoalescePageIndices(pageIndices, e.options.CoalesceGapPages, pageSize),
		int64(len(e.data)),
	)
	for _, pr := range ranges {
		_ = e.fetchCoalescedRange(context.Background(), pr)
	}
}

func (e *prefetchEngine) prefetchViewport(ctx context.Context, startIndex, endIndex int) error {
	if e.recordCount == 0 || len(e.data) == 0 {
		return nil
	}
	e.mu.Lock()
	closed := e.closed
	e.mu.Unlock()
	if closed {
		return ctx.Err()
	}
	pageSize := e.options.PageSize
	indices := pageIndicesForViewport(startIndex, endIndex, pageSize, e.recordOffset)
	e.cacheMu.Lock()
	defer e.cacheMu.Unlock()
	missingSet := make(map[int]struct{})
	for _, p := range indices {
		if !e.cache.has(p) && !e.inFlight.has(p) {
			missingSet[p] = struct{}{}
		}
	}
	if len(missingSet) > 0 {
		missing := make([]int, 0, len(missingSet))
		for p := range missingSet {
			missing = append(missing, p)
		}
		ranges := clampPageRanges(
			CoalescePageIndices(missing, e.options.CoalesceGapPages, pageSize),
			int64(len(e.data)),
		)
		for _, pr := range ranges {
			if err := ctx.Err(); err != nil {
				return err
			}
			if err := e.fetchCoalescedRangeLocked(ctx, pr); err != nil {
				return err
			}
		}
	}
	e.cache.pinPages(indices)
	e.cache.unpinAll()
	return nil
}

func (e *prefetchEngine) fetchCoalescedRange(ctx context.Context, pr PageRange) error {
	e.cacheMu.Lock()
	defer e.cacheMu.Unlock()
	return e.fetchCoalescedRangeLocked(ctx, pr)
}

func (e *prefetchEngine) fetchCoalescedRangeLocked(ctx context.Context, pr PageRange) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	e.fetchesIssued++
	blob, err := e.fetchRange(pr.ByteStart, pr.ByteLength)
	if err != nil {
		return err
	}
	pageSize := int64(e.options.PageSize)
	for p := pr.PageStart; p <= pr.PageEnd; p++ {
		if e.cache.has(p) {
			continue
		}
		pageOff := int64(p)*pageSize - pr.ByteStart
		pageLen := pageSize
		if pageOff+pageLen > int64(len(blob)) {
			pageLen = int64(len(blob)) - pageOff
		}
		if pageLen <= 0 {
			continue
		}
		n, err := intFromInt64(pageLen)
		if err != nil {
			return err
		}
		pageData := make([]byte, n)
		copy(pageData, blob[int(pageOff):int(pageOff+pageLen)])
		e.cache.set(p, pageData, false)
	}
	return nil
}
