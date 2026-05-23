package nxs

// Access pattern detector (Adaptive-prefetch-spec §4).

const (
	SequentialThreshold        = 10
	RandomThreshold            = 100
	HistorySize                = 32
	MinObservations            = 8
	UpgradeSequentialThreshold = 100
)

// AccessPattern classifies observed record access.
type AccessPattern int

const (
	PatternUnknown AccessPattern = iota
	PatternSequential
	PatternRandom
	PatternMixed
)

func (p AccessPattern) String() string {
	switch p {
	case PatternSequential:
		return "sequential"
	case PatternRandom:
		return "random"
	case PatternMixed:
		return "mixed"
	default:
		return "unknown"
	}
}

// AccessPatternDetector observes record/seek indices and classifies access patterns.
type AccessPatternDetector struct {
	accesses       [HistorySize]int64
	writePos       int
	filled         int
	sequentialRuns uint32
	randomJumps    uint32
	lastIndex      int64
}

// NewAccessPatternDetector returns an empty detector.
func NewAccessPatternDetector() *AccessPatternDetector {
	d := &AccessPatternDetector{lastIndex: -1}
	for i := range d.accesses {
		d.accesses[i] = -1
	}
	return d
}

// SequentialRuns returns the count of sequential step observations.
func (d *AccessPatternDetector) SequentialRuns() uint32 {
	return d.sequentialRuns
}

// LastIndex returns the most recently observed record index, or -1 if none.
func (d *AccessPatternDetector) LastIndex() int64 {
	return d.lastIndex
}

// Observe records an access at index.
func (d *AccessPatternDetector) Observe(index int) {
	idx := int64(index)
	if d.lastIndex >= 0 {
		var delta uint64
		if idx >= d.lastIndex {
			delta = uint64(idx - d.lastIndex)
		} else {
			delta = uint64(d.lastIndex - idx)
		}
		if delta <= SequentialThreshold {
			if d.sequentialRuns < ^uint32(0) {
				d.sequentialRuns++
			}
		} else if delta > RandomThreshold {
			if d.randomJumps < ^uint32(0) {
				d.randomJumps++
			}
		}
	}
	d.accesses[d.writePos] = idx
	d.writePos = (d.writePos + 1) % HistorySize
	if d.filled < HistorySize {
		d.filled++
	}
	d.lastIndex = idx
}

// Pattern returns the current classification.
func (d *AccessPatternDetector) Pattern() AccessPattern {
	total := int(d.sequentialRuns) + int(d.randomJumps)
	if total < MinObservations {
		return PatternUnknown
	}
	if d.sequentialRuns > d.randomJumps*3 {
		return PatternSequential
	}
	if d.randomJumps > d.sequentialRuns {
		return PatternRandom
	}
	return PatternMixed
}

// PredictNext returns predicted next record indices when pattern is sequential (§4.4).
func (d *AccessPatternDetector) PredictNext(depth, recordCount int) []int {
	if d.Pattern() != PatternSequential || d.lastIndex < 0 {
		return nil
	}
	start := int(d.lastIndex) + 1
	out := make([]int, 0, depth)
	for i := 0; i < depth; i++ {
		idx := start + i
		if idx < recordCount {
			out = append(out, idx)
		}
	}
	return out
}
