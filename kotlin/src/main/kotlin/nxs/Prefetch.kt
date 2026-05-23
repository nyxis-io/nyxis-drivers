package nxs

// Adaptive prefetch — page cache, range coalescing (spec §6–§8.4).

const val DEFAULT_PAGE_SIZE = 65536
const val DEFAULT_MAX_PAGES = 128
const val DEFAULT_COALESCE_GAP_PAGES = 1

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
    /** Injectable byte-range fetcher for tests or remote I/O; defaults to slicing [data]. */
    val fetchRange: ((byteStart: Long, byteLength: Long) -> ByteArray)? = null,
)

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
