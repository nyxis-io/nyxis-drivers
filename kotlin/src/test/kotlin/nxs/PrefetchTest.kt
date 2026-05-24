package nxs

import kotlin.concurrent.thread
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotEquals
import kotlin.test.assertTrue

class PrefetchTest {
    private fun buildRecords(n: Int): ByteArray {
        val schema = NxsSchema(listOf("id", "username", "score", "active"))
        val w = NxsWriter(schema)
        for (i in 0 until n) {
            w.beginObject()
            w.writeI64(0, i.toLong())
            w.writeStr(1, "user_$i")
            w.writeF64(2, i * 0.25)
            w.writeBool(3, i % 2 == 0)
            w.endObject()
        }
        return w.finish()
    }

    private fun buildCompactRecords(n: Int): ByteArray {
        val schema = NxsSchema(listOf("id", "tag"))
        val w = NxsWriter(schema)
        for (i in 0 until n) {
            w.beginObject()
            w.writeI64(0, i.toLong())
            w.writeStr(1, "r$i")
            w.endObject()
        }
        return w.finish()
    }

    @Test
    fun patternUnknownUntilMinObservations() {
        val d = AccessPatternDetector()
        for (i in 0 until 8) d.observe(i)
        assertEquals(AccessPattern.UNKNOWN, d.pattern())
        d.observe(8)
        assertNotEquals(AccessPattern.UNKNOWN, d.pattern())
    }

    @Test
    fun patternSequential_smallDeltas() {
        val d = AccessPatternDetector()
        for (i in 0 until 20) d.observe(i)
        assertEquals(AccessPattern.SEQUENTIAL, d.pattern())
    }

    @Test
    fun patternRandom_largeJumps() {
        val d = AccessPatternDetector()
        for (i in 0 until 8) d.observe(i)
        for (k in 0 until 12) d.observe(k * 200)
        assertEquals(AccessPattern.RANDOM, d.pattern())
    }

    @Test
    fun predictNext_sequential() {
        val d = AccessPatternDetector()
        for (i in 0 until 10) d.observe(i)
        assertTrue(d.predictNext(4, 100).contentEquals(intArrayOf(10, 11, 12, 13)))
    }

    @Test
    fun coalescePageIndices_mergesAdjacentWithGap() {
        val ranges = coalescePageIndices(intArrayOf(3, 4, 6, 7, 12), 1, DEFAULT_PAGE_SIZE)
        assertEquals(3, ranges.size)
        assertEquals(3, ranges[0].pageStart)
        assertEquals(4, ranges[0].pageEnd)
        assertEquals(6, ranges[1].pageStart)
        assertEquals(7, ranges[1].pageEnd)
        assertEquals(12, ranges[2].pageStart)
        assertEquals(12, ranges[2].pageEnd)
        assertEquals(2L * DEFAULT_PAGE_SIZE, ranges[0].byteLength)
    }

    @Test
    fun coalescePageIndices_deduplicates() {
        val ranges = coalescePageIndices(intArrayOf(3, 3, 4), 1, DEFAULT_PAGE_SIZE)
        assertEquals(1, ranges.size)
        assertEquals(3, ranges[0].pageStart)
        assertEquals(4, ranges[0].pageEnd)
    }

    @Test
    fun pageCache_lruEvictsAtMaxPages() {
        val c = PageCache(2, 64)
        c.set(0, ByteArray(64))
        c.set(1, ByteArray(64))
        c.get(0)
        c.set(2, ByteArray(64))
        assertFalse(c.has(1), "page 1 should be evicted")
        assertTrue(c.has(0))
        assertTrue(c.has(2))
    }

    @Test
    fun prefetchViewport_coalescesFetches() {
        val buf = buildRecords(60)
        val ranges = mutableListOf<Pair<Long, Long>>()
        val reader =
            NxsReader(
                buf,
                OpenOptions(
                    maxPages = 64,
                    coalesceGapPages = 1,
                    fetchRange = { off, len ->
                        ranges.add(off to len)
                        buf.copyOfRange(off.toInt(), (off + len).toInt())
                    },
                ),
            )
        reader.prefetchViewport(0, 49)
        assertTrue(ranges.size <= 3, "expected ≤3 fetches, got ${ranges.size}: $ranges")
        assertEquals(ranges.size, reader.cacheStats().fetchesIssued)
    }

    @Test
    fun prefetchViewport_basicRecordsReadable() {
        val buf = buildRecords(55)
        val reader =
            NxsReader(
                buf,
                OpenOptions(fetchRange = { off, len -> buf.copyOfRange(off.toInt(), (off + len).toInt()) }),
            )
        reader.prefetchViewport(0, 49)
        assertEquals(49L, reader.record(49).getI64("id"))
    }

    @Test
    fun prefetchMemoryEviction() {
        val buf = buildRecords(20)
        val reader =
            NxsReader(
                buf,
                OpenOptions(maxPages = 2, pageSize = 256, coalesceGapPages = 0),
            )
        reader.prefetchViewport(0, 0)
        reader.prefetchViewport(19, 19)
        assertTrue(reader.cacheStats().pagesCached <= 2)
    }

    @Test
    fun prefetchDeduplication_concurrentViewport() {
        val buf = buildRecords(10)
        var calls = 0
        val reader =
            NxsReader(
                buf,
                OpenOptions(
                    maxPages = 8,
                    fetchRange = { off, len ->
                        synchronized(this) { calls++ }
                        Thread.sleep(5)
                        buf.copyOfRange(off.toInt(), (off + len).toInt())
                    },
                ),
            )
        val t1 = thread { reader.prefetchViewport(0, 4) }
        val t2 = thread { reader.prefetchViewport(0, 4) }
        t1.join()
        t2.join()
        assertTrue(calls <= 3, "too many fetches: $calls")
    }

    @Test
    fun openOptions_hintStored() {
        val buf = buildRecords(5)
        val reader = NxsReader(buf, OpenOptions(hint = AccessHint.SEQUENTIAL))
        assertEquals(AccessHint.SEQUENTIAL, reader.accessHint())
    }

    @Test
    fun cacheStats_defaults() {
        val buf = buildRecords(5)
        val reader = NxsReader(buf)
        val stats = reader.cacheStats()
        assertEquals(DEFAULT_MAX_PAGES, stats.pagesMax)
        assertEquals("adaptive", stats.strategy)
        assertEquals("unknown", stats.pattern)
    }

    @Test
    fun sequentialUpgrade_toEagerAfter150Records() {
        val buf = buildCompactRecords(200)
        val reader = NxsReader(buf)
        try {
            for (i in 0 until 150) reader.record(i)
            reader.warmup()
            assertEquals("eager", reader.cacheStats().strategy)
            assertEquals("sequential", reader.cacheStats().pattern)
        } finally {
            reader.close()
        }
    }

    @Test
    fun pauseStopsSpeculative() {
        val buf = buildCompactRecords(200)
        val reader = NxsReader(buf)
        try {
            for (i in 0 until 25) reader.record(i)
            assertEquals("sequential", reader.cacheStats().pattern)
            val before = reader.cacheStats().fetchesIssued
            reader.pausePrefetch()
            reader.record(26)
            assertEquals(before, reader.cacheStats().fetchesIssued)
            reader.resumePrefetch()
            reader.record(27)
            assertTrue(reader.cacheStats().fetchesIssued >= before)
        } finally {
            reader.close()
        }
    }

    @Test
    fun hintFull_eagerAtOpen() {
        val buf = buildCompactRecords(200)
        val reader = NxsReader(buf, OpenOptions(hint = AccessHint.FULL))
        try {
            reader.warmup()
            assertEquals("eager", reader.cacheStats().strategy)
        } finally {
            reader.close()
        }
    }

    @Test
    fun prefetchColumn_singleFetchBeforeColSum() {
        val candidates =
            listOf(
                java.nio.file.Paths.get("../../nyxis/conformance/columnar_flat8_dense_100.nxb"),
                java.nio.file.Paths.get("../conformance/columnar_flat8_dense_100.nxb"),
            )
        val path = candidates.firstOrNull { java.nio.file.Files.isRegularFile(it) } ?: return
        val data = java.nio.file.Files.readAllBytes(path)
        var fetches = 0
        val reader =
            NxsReader(
                data,
                OpenOptions(fetchRange = { off, len ->
                    fetches++
                    data.copyOfRange(off.toInt(), (off + len).toInt())
                }),
            )
        try {
            reader.prefetchColumn("score")
            assertEquals(1, fetches)
            val sum = reader.sumF64("score")
            reader.prefetchColumn("score")
            assertEquals(1, fetches)
            assertEquals(2475.0, sum, 1e-6)
            assertEquals(1, reader.cacheStats().columnFetchesIssued)
        } finally {
            reader.close()
        }
    }
}
