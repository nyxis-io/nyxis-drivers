package nxs

/** Columnar column-buffer warmup (Adaptive-prefetch-spec §7.4). */
internal class ColumnWarmState(
    private val data: ByteArray,
    fetchRange: ((Long, Long) -> ByteArray)?,
) {
    private val warmed = mutableSetOf<Int>()
    private val overlay = mutableMapOf<Int, ByteArray>()

    @Volatile var fetches: Int = 0
    private val lock = Any()
    private val customFetch = fetchRange != null
    private val fetch: (Long, Long) -> ByteArray =
        fetchRange ?: { off, len ->
            val end = off + len
            if (off < 0 || end > data.size.toLong()) {
                throw NxsError("ERR_OUT_OF_BOUNDS", "column fetch [$off, $end)")
            }
            data.copyOfRange(off.toInt(), end.toInt())
        }

    fun prefetchColumn(
        reader: NxsReader,
        key: String,
    ) {
        if (reader.layout != Layout.COLUMNAR) {
            throw NxsError("ERR_LAYOUT", "prefetch_column requires columnar layout")
        }
        val slot = reader.keyIndex[key] ?: throw NxsError("ERR_KEY_NOT_FOUND", key)
        val off: Long
        val len: Long
        synchronized(lock) {
            if (slot in warmed) return
            off = reader.colBufOff[slot]
            len = reader.colBufLen[slot]
            if (off < 0 || len < 0) throw NxsError("ERR_OUT_OF_BOUNDS", "column buffer")
            if (!customFetch && off + len > data.size.toLong()) {
                throw NxsError("ERR_OUT_OF_BOUNDS", "column buffer")
            }
        }
        val blob = fetch(off, len)
        synchronized(lock) {
            if (slot in warmed) return
            if (off + blob.size > data.size) overlay[slot] = blob
            warmed.add(slot)
            fetches++
        }
    }

    fun sector(
        reader: NxsReader,
        slot: Int,
    ): ByteArray {
        val len = reader.colBufLen[slot].toInt()
        synchronized(lock) {
            overlay[slot]?.let { o ->
                if (o.size >= len) return o.copyOfRange(0, len)
            }
        }
        val off = reader.colBufOff[slot].toInt()
        if (off >= 0 && off + len <= data.size) {
            return data.copyOfRange(off, off + len)
        }
        throw NxsError("ERR_OUT_OF_BOUNDS", "column buffer")
    }
}
