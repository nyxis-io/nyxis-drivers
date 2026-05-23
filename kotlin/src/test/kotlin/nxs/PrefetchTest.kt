package nxs

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue
import kotlin.concurrent.thread

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
        assertEquals("lazy", stats.strategy)
        assertEquals("unknown", stats.pattern)
    }
}
