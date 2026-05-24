// Access pattern detector (Adaptive-prefetch-spec §4).

import Foundation

public enum AccessPattern: String {
    case unknown
    case sequential
    case random
    case mixed
}

public enum PatternConstants {
    public static let sequentialThreshold = 10
    public static let randomThreshold = 100
    public static let historySize = 32
    public static let minObservations = 8
    public static let upgradeSequentialThreshold = 100
}

public final class AccessPatternDetector {
    private var accesses: [Int64]
    private var writePos = 0
    private var filled = 0
    private(set) var sequentialRuns: UInt32 = 0
    private var randomJumps: UInt32 = 0
    private(set) var lastIndex: Int64 = -1

    public init() {
        accesses = Array(repeating: -1, count: PatternConstants.historySize)
    }

    public func observe(_ index: Int) {
        let idx = Int64(index)
        if lastIndex >= 0 {
            let delta: UInt64 = idx >= lastIndex
                ? UInt64(idx - lastIndex)
                : UInt64(lastIndex - idx)
            if delta <= UInt64(PatternConstants.sequentialThreshold) {
                if sequentialRuns < UInt32.max { sequentialRuns += 1 }
            } else if delta > UInt64(PatternConstants.randomThreshold) {
                if randomJumps < UInt32.max { randomJumps += 1 }
            }
        }
        accesses[writePos] = idx
        writePos = (writePos + 1) % PatternConstants.historySize
        if filled < PatternConstants.historySize { filled += 1 }
        lastIndex = idx
    }

    public func pattern() -> AccessPattern {
        let total = Int(sequentialRuns) + Int(randomJumps)
        if total < PatternConstants.minObservations { return .unknown }
        if sequentialRuns > randomJumps * 3 { return .sequential }
        if randomJumps > sequentialRuns { return .random }
        return .mixed
    }

    public func predictNext(depth: Int, recordCount: Int) -> [Int] {
        guard pattern() == .sequential, lastIndex >= 0 else { return [] }
        let start = Int(lastIndex) + 1
        var out: [Int] = []
        out.reserveCapacity(depth)
        for i in 0..<depth {
            let idx = start + i
            if idx < recordCount { out.append(idx) }
        }
        return out
    }
}
