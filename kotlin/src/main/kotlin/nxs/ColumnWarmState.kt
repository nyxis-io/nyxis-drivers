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
            if (off + len > data.size.toLong()) throw NxsError("ERR_OUT_OF_BOUNDS", "column buffer")
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
        synchronized(lock) {
            overlay[slot]?.let { o ->
                val len = reader.colBufLen[slot].toInt()
                if (o.size >= len) return o.copyOfRange(0, len)
            }
        }
        val off = reader.colBufOff[slot].toInt()
        val len = reader.colBufLen[slot].toInt()
        if (off + len > data.size) throw NxsError("ERR_OUT_OF_BOUNDS", "column buffer")
        return data.copyOfRange(off, off + len)
    }
}
