package nxs

// Access pattern detector (Adaptive-prefetch-spec §4).

const val SEQUENTIAL_THRESHOLD = 10
const val RANDOM_THRESHOLD = 100
const val HISTORY_SIZE = 32
const val MIN_OBSERVATIONS = 8
const val UPGRADE_SEQUENTIAL_THRESHOLD = 100

enum class AccessPattern {
    UNKNOWN,
    SEQUENTIAL,
    RANDOM,
    MIXED,
    ;

    fun asString(): String =
        when (this) {
            SEQUENTIAL -> "sequential"
            RANDOM -> "random"
            MIXED -> "mixed"
            UNKNOWN -> "unknown"
        }
}

/** Observes record(index) / seek calls and classifies access patterns. */
class AccessPatternDetector {
    private val accesses = LongArray(HISTORY_SIZE) { -1 }
    private var writePos = 0
    private var filled = 0
    private var sequentialRuns = 0
    private var randomJumps = 0
    var lastIndex: Long = -1
        private set

    fun sequentialRuns(): Int = sequentialRuns

    fun observe(index: Int) {
        val idx = index.toLong()
        if (lastIndex >= 0) {
            val delta =
                if (idx >= lastIndex) {
                    idx - lastIndex
                } else {
                    lastIndex - idx
                }
            when {
                delta <= SEQUENTIAL_THRESHOLD -> {
                    if (sequentialRuns < Int.MAX_VALUE) sequentialRuns++
                }
                delta > RANDOM_THRESHOLD -> {
                    if (randomJumps < Int.MAX_VALUE) randomJumps++
                }
            }
        }
        accesses[writePos] = idx
        writePos = (writePos + 1) % HISTORY_SIZE
        if (filled < HISTORY_SIZE) filled++
        lastIndex = idx
    }

    fun pattern(): AccessPattern {
        val total = sequentialRuns + randomJumps
        if (total < MIN_OBSERVATIONS) return AccessPattern.UNKNOWN
        if (sequentialRuns > randomJumps * 3) return AccessPattern.SEQUENTIAL
        if (randomJumps > sequentialRuns) return AccessPattern.RANDOM
        return AccessPattern.MIXED
    }

    fun predictNext(
        depth: Int,
        recordCount: Int,
    ): IntArray {
        if (pattern() != AccessPattern.SEQUENTIAL || lastIndex < 0) return intArrayOf()
        val start = (lastIndex + 1).toInt()
        val out = mutableListOf<Int>()
        for (i in 0 until depth) {
            val idx = start + i
            if (idx < recordCount) out.add(idx)
        }
        return out.toIntArray()
    }
}
