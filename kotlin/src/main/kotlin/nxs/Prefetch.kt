package nxs

import kotlin.concurrent.thread

// Adaptive prefetch — page cache, range coalescing (spec §6–§8.4).

const val DEFAULT_PAGE_SIZE = 65536
const val DEFAULT_MAX_PAGES = 128
const val DEFAULT_COALESCE_GAP_PAGES = 1
const val DEFAULT_PREFETCH_DEPTH = 4
const val EAGER_THRESHOLD_MB = 10
const val LAZY_THRESHOLD_MB = 50

enum class PrefetchStrategy {
    LAZY,
    ADAPTIVE,
    EAGER,
    ;

    fun asString(): String =
        when (this) {
            ADAPTIVE -> "adaptive"
            EAGER -> "eager"
            LAZY -> "lazy"
        }
}

enum class AccessHint {
    UNKNOWN,
    SEQUENTIAL,
    RANDOM,
    FULL,
    PARTIAL,
}

data class OpenOptions(
    val hint: AccessHint = AccessHint.UNKNOWN,
    val maxPages: Int = DEFAULT_MAX_PAGES,
    val pageSize: Int = DEFAULT_PAGE_SIZE,
    val coalesceGapPages: Int = DEFAULT_COALESCE_GAP_PAGES,
    val prefetchDepth: Int = DEFAULT_PREFETCH_DEPTH,
    /** Injectable byte-range fetcher for tests or remote I/O; defaults to slicing [data]. */
    val fetchRange: ((byteStart: Long, byteLength: Long) -> ByteArray)? = null,
)

fun initialStrategy(
    hint: AccessHint,
    fileSize: Int,
): PrefetchStrategy {
    val fileSizeMb = fileSize / (1024 * 1024)
    if (hint == AccessHint.FULL && fileSizeMb <= EAGER_THRESHOLD_MB) return PrefetchStrategy.EAGER
    if (fileSizeMb > LAZY_THRESHOLD_MB) return PrefetchStrategy.LAZY
    return PrefetchStrategy.ADAPTIVE
}

fun rowDataSector(
    tailStart: Int,
    fileSize: Int,
): Pair<Int, Int> {
    val sectorStart = 32
    return if (tailStart > sectorStart && tailStart <= fileSize) {
        sectorStart to (tailStart - sectorStart)
    } else {
        sectorStart to 0
    }
}

data class CacheStats(
    val pagesCached: Int,
    val pagesMax: Int,
    val memoryUsedBytes: Int,
    val cacheHits: Int,
    val cacheMisses: Int,
    val fetchesIssued: Int,
    val strategy: String,
    val pattern: String,
)

data class PageRange(
    val pageStart: Int,
    val pageEnd: Int,
    val byteStart: Long,
    val byteLength: Long,
)

/** Merge page indices when the gap between consecutive indices is at most [gapPages]. */
fun coalescePageIndices(
    indices: IntArray,
    gapPages: Int,
    pageSize: Int = DEFAULT_PAGE_SIZE,
): List<PageRange> {
    if (indices.isEmpty()) return emptyList()
    val uniq = indices.toSet().sorted()
    val spans = mutableListOf<Pair<Int, Int>>()
    var start = uniq[0]
    var end = uniq[0]
    for (i in 1 until uniq.size) {
        if (uniq[i] - end <= gapPages) {
            end = uniq[i]
        } else {
            spans.add(start to end)
            start = uniq[i]
            end = uniq[i]
        }
    }
    spans.add(start to end)
    return spans.map { (a, b) ->
        PageRange(
            pageStart = a,
            pageEnd = b,
            byteStart = a.toLong() * pageSize,
            byteLength = (b - a + 1).toLong() * pageSize,
        )
    }
}

fun clampPageRanges(
    ranges: List<PageRange>,
    fileSize: Long,
): List<PageRange> =
    ranges.mapNotNull { r ->
        var len = r.byteLength
        if (r.byteStart + len > fileSize) len = fileSize - r.byteStart
        if (len <= 0) null else r.copy(byteLength = len)
    }

fun pageIndicesForViewport(
    startIndex: Int,
    endIndex: Int,
    pageSize: Int,
    recordOffset: (Int) -> Long,
): IntArray {
    val out = IntArray(endIndex - startIndex + 1)
    for (i in startIndex..endIndex) {
        out[i - startIndex] = (recordOffset(i) / pageSize).toInt()
    }
    return out
}

private data class PageEntry(
    val data: ByteArray,
    var lastUsed: Int,
    var pinned: Boolean,
)

internal class PageCache(
    val maxPages: Int,
    val pageSize: Int,
) {
    private val pages = mutableMapOf<Int, PageEntry>()
    private var clock = 0
    var hits = 0
        private set
    var misses = 0
        private set

    fun has(pageIndex: Int): Boolean = pages.containsKey(pageIndex)

    fun get(pageIndex: Int): ByteArray? {
        val e =
            pages[pageIndex] ?: run {
                misses++
                return null
            }
        e.lastUsed = ++clock
        hits++
        return e.data
    }

    fun set(
        pageIndex: Int,
        data: ByteArray,
        pinned: Boolean = false,
    ) {
        if (maxPages <= 0) return
        while (pages.size >= maxPages) {
            if (!evictOne()) break
        }
        pages[pageIndex] = PageEntry(data, ++clock, pinned)
    }

    private fun evictOne(): Boolean {
        var oldest = Int.MAX_VALUE
        var victim = -1
        for ((idx, e) in pages) {
            if (e.pinned) continue
            if (e.lastUsed < oldest) {
                oldest = e.lastUsed
                victim = idx
            }
        }
        if (victim < 0) return false
        pages.remove(victim)
        return true
    }

    fun pinPages(pageIndices: IntArray) {
        for (p in pageIndices) {
            pages[p]?.pinned = true
        }
    }

    fun unpinAll() {
        for (e in pages.values) {
            e.pinned = false
        }
    }

    fun stats(): Pair<Int, Int> {
        var bytes = 0
        for (e in pages.values) bytes += e.data.size
        return pages.size to bytes
    }
}

internal class PrefetchEngine(
    private val options: OpenOptions,
    private val fileSize: Int,
    private val tailStart: Int,
    private val recordCount: Int,
    private val data: ByteArray,
    private val recordOffset: (Int) -> Long,
    private val fetchRange: (Long, Long) -> ByteArray,
) {
    private val lock = Any()
    private val cacheLock = Any()

    val pageSize: Int =
        if (options.pageSize > 0) options.pageSize else DEFAULT_PAGE_SIZE
    private val coalesceGapPages: Int =
        if (options.coalesceGapPages >= 0) options.coalesceGapPages else DEFAULT_COALESCE_GAP_PAGES
    private val prefetchDepth: Int =
        if (options.prefetchDepth > 0) options.prefetchDepth else DEFAULT_PREFETCH_DEPTH

    private val maxPages = if (options.maxPages > 0) options.maxPages else DEFAULT_MAX_PAGES
    val cache = PageCache(maxPages, pageSize)
    private val detector = AccessPatternDetector()

    @Volatile private var strategy: PrefetchStrategy = initialStrategy(options.hint, fileSize)

    @Volatile private var closed = false

    @Volatile private var eagerCancel = false

    @Volatile private var eagerStarted = false

    @Volatile private var eagerComplete = false
    private var eagerThread: Thread? = null
    private var fetchesIssued = 0

    init {
        if (strategy == PrefetchStrategy.EAGER) startEagerBackground()
    }

    fun cacheStats(): CacheStats {
        val (strat, pat) =
            synchronized(lock) {
                strategy.asString() to detector.pattern().asString()
            }
        val (pagesCached, memoryUsed) =
            synchronized(cacheLock) {
                cache.stats()
            }
        val fetches =
            synchronized(cacheLock) {
                fetchesIssued
            }
        return CacheStats(
            pagesCached = pagesCached,
            pagesMax = cache.maxPages,
            memoryUsedBytes = memoryUsed,
            cacheHits = cache.hits,
            cacheMisses = cache.misses,
            fetchesIssued = fetches,
            strategy = strat,
            pattern = pat,
        )
    }

    fun close() {
        val thread: Thread?
        synchronized(lock) {
            if (closed) return
            closed = true
            eagerCancel = true
            thread = eagerThread
        }
        thread?.join()
    }

    fun warmup() {
        eagerThread?.join()
    }

    fun onAccess(index: Int) {
        if (recordCount == 0) return
        val speculative: Boolean
        synchronized(lock) {
            if (closed) return
            detector.observe(index)
            maybeUpgradeToEager()
            if (isEagerActive()) return
            speculative = strategy == PrefetchStrategy.ADAPTIVE && detector.pattern() == AccessPattern.SEQUENTIAL
        }
        val off = recordOffset(index)
        if (off >= 0) {
            synchronized(cacheLock) {
                cache.get((off / pageSize).toInt())
            }
        }
        if (speculative) speculativePrefetch()
    }

    fun prefetchViewport(
        startIndex: Int,
        endIndex: Int,
    ) {
        if (recordCount == 0 || data.isEmpty()) return
        synchronized(lock) {
            if (closed) return
        }
        val indices = pageIndicesForViewport(startIndex, endIndex, pageSize, recordOffset)
        synchronized(cacheLock) {
            val missing = indices.filter { p -> !cache.has(p) }.distinct()
            if (missing.isNotEmpty()) {
                val ranges =
                    clampPageRanges(
                        coalescePageIndices(missing.toIntArray(), coalesceGapPages, pageSize),
                        fileSize.toLong(),
                    )
                for (pr in ranges) fetchCoalescedRangeLocked(pr)
            }
            cache.pinPages(indices)
            cache.unpinAll()
        }
    }

    private fun isEagerActive(): Boolean = strategy == PrefetchStrategy.EAGER && !eagerComplete

    private fun maybeUpgradeToEager() {
        if (strategy != PrefetchStrategy.ADAPTIVE) return
        if (detector.pattern() != AccessPattern.SEQUENTIAL) return
        if (detector.sequentialRuns() < UPGRADE_SEQUENTIAL_THRESHOLD) return
        if (fileSize / (1024 * 1024) > EAGER_THRESHOLD_MB) return
        strategy = PrefetchStrategy.EAGER
        startEagerBackground()
    }

    private fun startEagerBackground() {
        if (strategy != PrefetchStrategy.EAGER || eagerStarted) return
        eagerStarted = true
        val (sectorStart, sectorLen) = rowDataSector(tailStart, fileSize)
        if (sectorLen == 0) {
            eagerComplete = true
            return
        }
        eagerThread =
            thread(name = "nxs-eager-prefetch", isDaemon = true) {
                val end = minOf(sectorStart + sectorLen, data.size)
                if (sectorStart >= end) {
                    if (!eagerCancel) eagerComplete = true
                    return@thread
                }
                val ranges =
                    clampPageRanges(
                        coalescePageIndices(
                            (sectorStart / pageSize..(end - 1) / pageSize).toList().toIntArray(),
                            coalesceGapPages,
                            pageSize,
                        ),
                        fileSize.toLong(),
                    )
                for (pr in ranges) {
                    if (eagerCancel) return@thread
                    synchronized(cacheLock) {
                        fetchCoalescedRangeLocked(pr)
                    }
                }
                if (!eagerCancel) eagerComplete = true
            }
    }

    private fun speculativePrefetch() {
        val predicted: IntArray
        synchronized(lock) {
            predicted = detector.predictNext(prefetchDepth, recordCount)
        }
        if (predicted.isEmpty()) return
        synchronized(cacheLock) {
            val seen = mutableSetOf<Int>()
            val pageIndices = mutableListOf<Int>()
            for (idx in predicted) {
                val off = recordOffset(idx)
                if (off < 0) continue
                val p = (off / pageSize).toInt()
                if (!seen.add(p)) continue
                if (!cache.has(p)) pageIndices.add(p)
            }
            if (pageIndices.isEmpty()) return
            val ranges =
                clampPageRanges(
                    coalescePageIndices(pageIndices.toIntArray(), coalesceGapPages, pageSize),
                    fileSize.toLong(),
                )
            for (pr in ranges) {
                if (eagerCancel || closed) return
                fetchCoalescedRangeLocked(pr)
            }
        }
    }

    private fun fetchCoalescedRangeLocked(pr: PageRange) {
        val blob = fetchRangeBytes(pr.byteStart, pr.byteLength)
        val ps = pageSize.toLong()
        for (p in pr.pageStart..pr.pageEnd) {
            if (cache.has(p)) continue
            val pageOff = p * ps - pr.byteStart
            var pageLen = ps
            if (pageOff + pageLen > blob.size.toLong()) pageLen = blob.size.toLong() - pageOff
            if (pageLen <= 0) continue
            cache.set(p, blob.copyOfRange(pageOff.toInt(), (pageOff + pageLen).toInt()))
        }
    }

    private fun fetchRangeBytes(
        byteStart: Long,
        byteLength: Long,
    ): ByteArray {
        fetchesIssued++
        return fetchRange(byteStart, byteLength)
    }
}
