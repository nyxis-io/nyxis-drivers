# frozen_string_literal: true

require_relative 'pattern'

# NXS Reader — .nxb parser (Ruby 3.x, stdlib only).
#
# Implements Nyxis v1.1 binary wire format.
#
# Usage:
#   buf = File.binread("data.nxb")
#   reader = Nxs::Reader.new(buf)
#   reader.record_count          # => Integer
#   reader.keys                  # => Array<String>
#   obj = reader.record(42)      # => Nxs::Object
#   obj.get_str("username")      # => String | nil
#   obj.get_i64("id")            # => Integer | nil
#   obj.get_f64("score")         # => Float | nil
#   obj.get_bool("active")       # => true/false | nil
#   reader.sum_f64("score")      # => Float
#   reader.min_f64("score")      # => Float | nil
#   reader.max_f64("score")      # => Float | nil
#   reader.sum_i64("id")         # => Integer

module Nxs
  MAGIC_FILE   = 0x4E595842  # NYXB
  MAGIC_OBJ    = 0x4E59584F  # NYXO
  MAGIC_LIST   = 0x4E59584C  # NYXL
  MAGIC_PAGE   = 0x4E585350  # NYXP
  MAGIC_FOOTER = 0x2153584E  # NXS!
  FLAG_COLUMNAR = 0x0001
  FLAG_PAX      = 0x0004
  FLAG_SCHEMA   = 0x0002

  FOOTER_ROW_BYTES = 12
  FOOTER_COL_BYTES = 20
  FOOTER_PAX_BYTES = 28
  COL_TAIL_ENTRY_BYTES = 20
  PAX_TAIL_ENTRY_BYTES = 28

  # Adaptive prefetch (phase 1) — spec §6–§8.4
  DEFAULT_PAGE_SIZE = 65_536
  DEFAULT_MAX_PAGES = 64
  DEFAULT_COALESCE_GAP_PAGES = 1
  DEFAULT_PREFETCH_DEPTH = 4
  EAGER_THRESHOLD_MB = 10
  LAZY_THRESHOLD_MB = 50

  HINT_UNKNOWN = 0
  HINT_SEQUENTIAL = 1
  HINT_RANDOM = 2
  HINT_FULL = 3
  HINT_PARTIAL = 4

  HINT_SYMBOLS = {
    unknown: HINT_UNKNOWN,
    sequential: HINT_SEQUENTIAL,
    random: HINT_RANDOM,
    full: HINT_FULL,
    partial: HINT_PARTIAL
  }.freeze

  def self.normalize_hint(hint)
    return hint if hint.is_a?(Integer)

    HINT_SYMBOLS.fetch(hint) { HINT_UNKNOWN }
  end

  # Initial prefetch strategy from open hint and file size (spec §5.1).
  def self.initial_strategy(hint, file_size)
    hint = normalize_hint(hint)
    file_size_mb = file_size / (1024 * 1024)
    return 'eager' if hint == HINT_FULL && file_size_mb <= EAGER_THRESHOLD_MB
    return 'lazy' if file_size_mb > LAZY_THRESHOLD_MB

    'adaptive'
  end

  # Row-layout data sector byte range [start, length).
  def self.row_data_sector(tail_start, file_size)
    sector_start = 32
    if tail_start > sector_start && tail_start <= file_size
      [sector_start, tail_start - sector_start]
    else
      [sector_start, 0]
    end
  end

  # Merge sorted unique page indices when gap <= gap_pages (inclusive).
  def self.coalesce_page_indices(indices, gap_pages, page_size = DEFAULT_PAGE_SIZE)
    return [] if indices.empty?

    uniq = indices.uniq.sort
    spans = []
    start = uniq[0]
    end_ = uniq[0]
    uniq.each_cons(2) do |_a, b|
      if b - end_ <= gap_pages
        end_ = b
      else
        spans << [start, end_]
        start = end_ = b
      end
    end
    spans << [start, end_]
    spans.map do |a, b|
      { page_start: a, page_end: b, byte_start: a * page_size, byte_length: (b - a + 1) * page_size }
    end
  end

  def self.clamp_page_ranges(ranges, file_size)
    ranges.filter_map do |r|
      len = r[:byte_length]
      len = file_size - r[:byte_start] if r[:byte_start] + len > file_size
      next nil if len <= 0

      r.merge(byte_length: len)
    end
  end

  def self.page_indices_for_viewport(start_index, end_index, page_size, &record_offset)
    (start_index..end_index).map { |i| record_offset.call(i) / page_size }
  end

  # LRU page cache with optional pinning (spec §6).
  class PageCache
    attr_reader :max_pages, :page_size, :hits, :misses

    def initialize(max_pages = DEFAULT_MAX_PAGES, page_size = DEFAULT_PAGE_SIZE)
      @max_pages = max_pages
      @page_size = page_size
      @pages = {}
      @clock = 0
      @hits = 0
      @misses = 0
    end

    def has?(page_index)
      @pages.key?(page_index)
    end

    def get(page_index)
      entry = @pages[page_index]
      unless entry
        @misses += 1
        return nil
      end
      @clock += 1
      entry[:last_used] = @clock
      @hits += 1
      entry[:data]
    end

    def set(page_index, data, pinned: false)
      return if @max_pages <= 0

      while @pages.size >= @max_pages && !evict_one?; end
      @clock += 1
      @pages[page_index] = { data: data, last_used: @clock, pinned: pinned }
    end

    def pin_pages(page_indices)
      page_indices.each do |p|
        entry = @pages[p]
        entry[:pinned] = true if entry
      end
    end

    def unpin_all
      @pages.each_value { |entry| entry[:pinned] = false }
    end

    def stats
      bytes = @pages.values.sum { |e| e[:data].bytesize }
      {
        pages_cached: @pages.size,
        pages_max: @max_pages,
        memory_used_bytes: bytes,
        cache_hits: @hits,
        cache_misses: @misses
      }
    end

    private

    def evict_one?
      victim = nil
      oldest = nil
      @pages.each do |idx, entry|
        next if entry[:pinned]

        if oldest.nil? || entry[:last_used] < oldest
          oldest = entry[:last_used]
          victim = idx
        end
      end
      return false unless victim

      @pages.delete(victim)
      true
    end
  end

  # In-flight page fetch deduplication for concurrent prefetch_viewport calls.
  class InFlightMap
    Entry = Struct.new(:queue, :data, :error)

    def initialize
      @mu = Mutex.new
      @map = {}
    end

    def has?(page_index)
      @mu.synchronize { @map.key?(page_index) }
    end

    def wait(page_index)
      entry = @mu.synchronize { @map[page_index] }
      return nil unless entry

      entry.queue.pop
      raise entry.error if entry.error

      entry.data
    end

    def with(page_index)
      entry = nil
      leader = @mu.synchronize do
        existing = @map[page_index]
        if existing
          false
        else
          entry = Entry.new(Queue.new)
          @map[page_index] = entry
          true
        end
      end
      return wait(page_index) unless leader

      begin
        data = yield
        entry.data = data
        entry.queue << true
        data
      rescue StandardError => e
        entry.error = e
        entry.queue << true
        raise
      ensure
        @mu.synchronize { @map.delete(page_index) if @map[page_index] == entry }
      end
    end
  end

  class NxsError < StandardError
    attr_reader :code

    def initialize(code, msg)
      super("#{code}: #{msg}")
      @code = code
    end
  end

  # ── Reader ──────────────────────────────────────────────────────────────────

  class Reader
    attr_reader :keys, :record_count, :layout

    def initialize(bytes, **options)
      hint = options.fetch(:hint, HINT_UNKNOWN)
      max_pages = options.fetch(:max_pages, DEFAULT_MAX_PAGES)
      page_size = options.fetch(:page_size, DEFAULT_PAGE_SIZE)
      coalesce_gap_pages = options.fetch(:coalesce_gap_pages, DEFAULT_COALESCE_GAP_PAGES)
      prefetch_depth = options.fetch(:prefetch_depth, DEFAULT_PREFETCH_DEPTH)
      fetch_range = options.fetch(:fetch_range, nil)
      @data = bytes.b # force binary encoding
      sz = @data.bytesize
      raise NxsError.new('ERR_OUT_OF_BOUNDS', 'file too small') if sz < 32

      magic = @data.unpack1('L<')
      raise NxsError.new('ERR_BAD_MAGIC', "expected NYXB, got 0x#{magic.to_s(16)}") if magic != MAGIC_FILE

      footer = @data.unpack1("@#{sz - 4}L<")
      raise NxsError.new('ERR_BAD_MAGIC', 'footer magic mismatch') if footer != MAGIC_FOOTER

      # Preamble: Version(2) + Flags(2) + DictHash(8) + TailPtr(8) + Reserved(8)
      @flags         = @data.unpack1('@6 S<')
      preamble_tail  = @data.unpack1('@16 Q<')
      @tail_ptr       = preamble_tail
      layout_flags    = @flags & (FLAG_COLUMNAR | FLAG_PAX)
      if @tail_ptr.zero? && layout_flags.zero?
        raise NxsError.new('ERR_OUT_OF_BOUNDS', 'stream footer') if sz < 44

        @tail_ptr = @data.unpack1("@#{sz - FOOTER_ROW_BYTES}Q<")
      end

      @dict_hash = @data.unpack1('@8 Q<')

      # Schema (when Flags bit 1 set)
      @keys       = []
      @key_sigils = []
      @key_index  = {}
      if @flags & FLAG_SCHEMA != 0
        schema_end = read_schema(32)
        computed = murmur3_64(@data[32...schema_end].bytes)
        raise NxsError.new('ERR_DICT_MISMATCH', 'schema hash mismatch') if computed != @dict_hash
      end

      @col_buf_off = []
      @col_buf_len = []
      parse_layout_tail!(preamble_tail)
      init_column_prefetch!(fetch_range: fetch_range)
      init_prefetch!(
        hint: hint,
        max_pages: max_pages,
        page_size: page_size,
        coalesce_gap_pages: coalesce_gap_pages,
        prefetch_depth: prefetch_depth,
        fetch_range: fetch_range
      )
    end

    # O(1) record lookup — row tail-index or columnar/PAX record index.
    def record(i)
      unless i >= 0 && i < @record_count
        raise NxsError.new('ERR_OUT_OF_BOUNDS', "record #{i} out of [0, #{@record_count})")
      end

      return Object.new(self, i, i) if @layout != :row

      on_access(i)
      abs_offset = @data.unpack1("@#{@tail_start + i * 10 + 2}Q<")
      Object.new(self, abs_offset)
    end

    # Prefetch one column buffer (columnar layout only; §7.4).
    def prefetch_column(key)
      raise NxsError.new('ERR_LAYOUT', 'prefetch_column requires columnar layout') unless @layout == :columnar

      slot = @key_index[key]
      raise NxsError.new('ERR_KEY_NOT_FOUND', "key #{key.inspect} not in schema") unless slot

      off = nil
      length = nil
      fetch = nil
      @col_mu.synchronize do
        return if @col_warmed[slot]

        off = @col_buf_off[slot].to_i
        length = @col_buf_len[slot].to_i
        raise NxsError.new('ERR_OUT_OF_BOUNDS', 'column buffer') if off.negative? || length.negative?
        raise NxsError.new('ERR_OUT_OF_BOUNDS', 'column buffer') if !@col_remote_fetch && off + length > @data.bytesize

        fetch = @col_fetch_range
      end
      blob = fetch.call(off, length)
      @col_mu.synchronize do
        return if @col_warmed[slot]

        @col_overlay[slot] = blob if off + blob.bytesize > @data.bytesize
        @col_warmed[slot] = true
        @col_fetches += 1
      end
    end

    # Sum f64 column — columnar/PAX buffer path or row scan.
    def sum_f64(key)
      return col_sum_f64(key) if @layout != :row

      slot = @key_index[key]
      raise NxsError.new('ERR_OUT_OF_BOUNDS', "key '#{key}' not in schema") unless slot

      data = @data
      tail = @tail_start
      n    = @record_count
      sum  = 0.0
      i = 0
      while i < n
        abs = data.unpack1("@#{tail + i * 10 + 2}Q<")
        off = _scan_offset(data, abs, slot)
        sum += data.unpack1("@#{off}E") if off
        i += 1
      end
      sum
    end

    # Columnar/PAX f64 sum (row layout delegates to sum_f64).
    def col_sum_f64(key)
      slot = @key_index[key]
      raise NxsError.new('ERR_OUT_OF_BOUNDS', "key '#{key}' not in schema") unless slot

      return sum_f64(key) if @layout == :row
      return pax_sum_f64(slot) if @layout == :pax

      bm, vals = col_field_parts(slot)
      n = @record_count
      sum = 0.0
      i = 0
      while i < n
        if col_bit(bm, i)
          off = i * 8
          sum += vals.unpack1("@#{off}E") if off + 8 <= vals.bytesize
        end
        i += 1
      end
      sum
    end

    # Raw value bytes for a fixed-width column (columnar/PAX).
    def col_buffer(key)
      raise NxsError.new('ERR_LAYOUT', 'col_buffer requires columnar or PAX layout') if @layout == :row

      slot = @key_index[key]
      return nil unless slot
      return nil if var_sigil?(@key_sigils[slot])

      _bm, vals = col_field_parts(slot)
      vals
    rescue NxsError
      nil
    end

    # Null bitmap + u32 offsets + values for var-length columns (columnar only).
    def col_var_buffer(key)
      raise NxsError.new('ERR_LAYOUT', 'col_var_buffer is columnar-only') unless @layout == :columnar

      slot = @key_index[key]
      raise NxsError.new('ERR_OUT_OF_BOUNDS', "key '#{key}' not in schema") unless slot
      raise NxsError.new('ERR_UNSUPPORTED_FIELD_TYPE', key) unless var_sigil?(@key_sigils[slot])

      bm, offsets, values = col_var_parts(slot)
      { bitmap: bm, offsets: offsets, values: values, count: @record_count }
    end

    def col_get_str(key, record_index)
      slot = @key_index[key]
      return nil unless slot && record_index < @record_count && @layout != :row
      return nil unless @key_sigils[slot] == 0x22

      bm, offsets, values, ok = col_var_parts_at(record_index, slot)
      return nil unless ok

      bit_idx = @layout == :pax ? pax_find_page(record_index)&.[](:local) : record_index
      return nil if bit_idx.nil? || !col_bit(bm, bit_idx)

      var_str_at(offsets, values, bit_idx)
    end

    def min_f64(key)
      slot = @key_index[key]
      raise NxsError.new('ERR_OUT_OF_BOUNDS', "key '#{key}' not in schema") unless slot

      data = @data
      tail = @tail_start
      n    = @record_count
      min  = nil
      i = 0
      while i < n
        abs = data.unpack1("@#{tail + i * 10 + 2}Q<")
        off = _scan_offset(data, abs, slot)
        if off
          v   = data.unpack1("@#{off}E")
          min = v if min.nil? || v < min
        end
        i += 1
      end
      min
    end

    def max_f64(key)
      slot = @key_index[key]
      raise NxsError.new('ERR_OUT_OF_BOUNDS', "key '#{key}' not in schema") unless slot

      data = @data
      tail = @tail_start
      n    = @record_count
      max  = nil
      i = 0
      while i < n
        abs = data.unpack1("@#{tail + i * 10 + 2}Q<")
        off = _scan_offset(data, abs, slot)
        if off
          v   = data.unpack1("@#{off}E")
          max = v if max.nil? || v > max
        end
        i += 1
      end
      max
    end

    def sum_i64(key)
      slot = @key_index[key]
      raise NxsError.new('ERR_OUT_OF_BOUNDS', "key '#{key}' not in schema") unless slot

      data = @data
      tail = @tail_start
      n    = @record_count
      sum  = 0
      i = 0
      while i < n
        abs = data.unpack1("@#{tail + i * 10 + 2}Q<")
        off = _scan_offset(data, abs, slot)
        sum += data.unpack1("@#{off}q<") if off
        i += 1
      end
      sum
    end

    # Expose internals for Object
    attr_reader :data, :key_index

    # Walk the LEB128 bitmask from obj_offset+8, count set bits before `slot`,
    # and return the absolute byte offset of the field value (or nil if absent).
    # Used by both bulk reducers and NxsObject.
    def _scan_offset(data, obj_offset, slot)
      p = obj_offset + 8 # skip Magic(4) + Length(4)
      cur   = 0
      t_idx = 0

      loop do
        raise NxsError.new('ERR_OUT_OF_BOUNDS', 'bitmask overrun on corrupt input') if p >= data.bytesize

        b    = data.getbyte(p)
        p   += 1
        bits = b & 0x7F
        7.times do |i|
          if cur == slot
            # field absent if bit is 0
            return nil if ((bits >> i) & 1).zero?

            # p already past this bitmask byte; drain remaining continuation bytes
            while (b & 0x80) != 0
              b = data.getbyte(p)
              p += 1
            end
            # p now points to the offset table
            rel = data.unpack1("@#{p + t_idx * 2}S<")
            return obj_offset + rel
          end
          t_idx += 1 if (bits >> i) & 1 == 1
          cur += 1
        end
        # If all 7 bits processed and continuation bit clear, field is absent
        return nil if (b & 0x80).zero?
      end
    end

    # rubocop:disable Metrics/ParameterLists -- prefetch open options mirror Go OpenOptions
    def init_prefetch!(hint:, max_pages:, page_size:, coalesce_gap_pages:, prefetch_depth:, fetch_range:)
      @prefetch_mu = Mutex.new
      @cache_mu = Mutex.new
      @prefetch_hint = Nxs.normalize_hint(hint)
      @prefetch_page_size = page_size
      @prefetch_depth = prefetch_depth.positive? ? prefetch_depth : DEFAULT_PREFETCH_DEPTH
      @coalesce_gap_pages = coalesce_gap_pages
      @page_cache = PageCache.new(max_pages, page_size)
      @in_flight = InFlightMap.new
      @fetches_issued = 0
      @detector = AccessPatternDetector.new
      @prefetch_strategy = Nxs.initial_strategy(@prefetch_hint, @data.bytesize)
      @prefetch_pattern = PATTERN_UNKNOWN
      @eager_started = false
      @eager_complete = false
      @eager_cancel = false
      @eager_thread = nil
      @closed = false
      @prefetch_paused = false
      @fetch_range = fetch_range || lambda do |byte_start, byte_length|
        raise NxsError.new('ERR_OUT_OF_BOUNDS', 'fetch range out of bounds') if byte_start.negative?

        end_ = byte_start + byte_length
        raise NxsError.new('ERR_OUT_OF_BOUNDS', 'fetch range out of bounds') if end_ > @data.bytesize

        @data[byte_start, byte_length]
      end
      start_eager_background! if @layout == :row && @prefetch_strategy == 'eager'
    end
    # rubocop:enable Metrics/ParameterLists

    # Block until eager / background prefetch completes (spec §8).
    def warmup
      t = @prefetch_mu.synchronize { @eager_thread }
      t&.join
    end

    # Stop scheduling speculative and eager prefetch (§8.1).
    def pause_prefetch
      @prefetch_mu.synchronize { @prefetch_paused = true }
    end

    # Re-enable speculative prefetch after pause_prefetch.
    def resume_prefetch
      @prefetch_mu.synchronize { @prefetch_paused = false }
    end

    # Cancel in-flight eager prefetch and wait for the background thread.
    def close
      t = nil
      @prefetch_mu.synchronize do
        @closed = true
        @eager_cancel = true
        t = @eager_thread
      end
      t&.join
    end

    def on_access(index)
      return unless @layout == :row
      return if @record_count.zero?

      adaptive_seq = false
      skip_spec = false
      start_eager = false
      @prefetch_mu.synchronize do
        return if @closed || @prefetch_paused

        @detector.observe(index)
        @prefetch_pattern = @detector.pattern
        start_eager = maybe_upgrade_to_eager!
        if eager_complete? || @prefetch_strategy == 'eager'
          skip_spec = true
          next
        end
        page_index = record_byte_offset(index) / @prefetch_page_size
        @cache_mu.synchronize { @page_cache.get(page_index) }
        adaptive_seq = @prefetch_strategy == 'adaptive' && @detector.pattern == PATTERN_SEQUENTIAL
      end
      start_eager_background! if start_eager
      return if skip_spec

      speculative_prefetch! if adaptive_seq
    end

    def record_byte_offset(i)
      @data.unpack1("@#{@tail_start + i * 10 + 2}Q<")
    end

    # Prefetch pages for records [start_index, end_index] (row layout only).
    def prefetch_viewport(start_index, end_index)
      return self if @layout != :row

      n = @record_count
      unless start_index.between?(0, end_index) && end_index < n
        raise NxsError.new(
          'ERR_OUT_OF_BOUNDS',
          "prefetch_viewport [#{start_index}, #{end_index}] out of [0, #{n})"
        )
      end

      @cache_mu.synchronize do
        page_size = @prefetch_page_size
        indices = Nxs.page_indices_for_viewport(start_index, end_index, page_size) do |i|
          record_byte_offset(i)
        end
        missing = indices.uniq.select { |p| !@page_cache.has?(p) && !@in_flight.has?(p) }
        if missing.empty?
          @page_cache.pin_pages(indices)
          @page_cache.unpin_all
          return self
        end

        ranges = Nxs.clamp_page_ranges(
          Nxs.coalesce_page_indices(missing, @coalesce_gap_pages, page_size),
          @data.bytesize
        )
        ranges.each { |r| fetch_coalesced_range_unlocked!(r) }
        @page_cache.pin_pages(indices)
        @page_cache.unpin_all
      end
      self
    end

    def cache_stats
      stats = @page_cache.stats
      col_fetches = @col_mu.synchronize { @col_fetches }
      strategy, pattern = @prefetch_mu.synchronize do
        [@prefetch_strategy, @detector.pattern]
      end
      stats.merge(
        fetches_issued: @fetches_issued,
        column_fetches_issued: col_fetches,
        strategy: strategy,
        pattern: pattern
      )
    end

    private

    def eager_complete?
      @prefetch_strategy == 'eager' && @eager_complete
    end

    def maybe_upgrade_to_eager!
      return if @prefetch_paused
      return unless @prefetch_strategy == 'adaptive'
      return unless @detector.pattern == PATTERN_SEQUENTIAL
      return if @detector.sequential_runs < UPGRADE_SEQUENTIAL_THRESHOLD
      return if @data.bytesize / (1024 * 1024) > EAGER_THRESHOLD_MB

      @prefetch_strategy = 'eager'
      true
    end

    def speculative_prefetch!
      return if @prefetch_mu.synchronize { @prefetch_paused }

      predicted = @prefetch_mu.synchronize { @detector.predict_next(@prefetch_depth, @record_count) }
      return if predicted.empty?

      page_size = @prefetch_page_size
      missing = @cache_mu.synchronize do
        predicted.filter_map do |idx|
          off = record_byte_offset(idx)
          p = off / page_size
          p unless @page_cache.has?(p) || @in_flight.has?(p)
        end.uniq
      end
      return if missing.empty?

      ranges = Nxs.clamp_page_ranges(
        Nxs.coalesce_page_indices(missing, @coalesce_gap_pages, page_size),
        @data.bytesize
      )
      ranges.each { |r| fetch_coalesced_range!(r) }
    end

    def start_eager_background!
      return unless @prefetch_strategy == 'eager'

      @prefetch_mu.synchronize do
        return if @prefetch_paused || @eager_started

        @eager_started = true
        sector_start, sector_len = Nxs.row_data_sector(@tail_start, @data.bytesize)
        if sector_len.zero?
          @eager_complete = true
          next
        end
        @eager_thread = Thread.new { run_eager_background(sector_start, sector_len) }
      end
    end

    def run_eager_background(sector_start, sector_len)
      end_byte = [sector_start + sector_len, @data.bytesize].min
      return if sector_start >= end_byte

      page_size = @prefetch_page_size
      first_page = sector_start / page_size
      last_page = (end_byte - 1) / page_size
      indices = (first_page..last_page).to_a
      eager_cancelled = @prefetch_mu.synchronize { @eager_cancel }
      return if eager_cancelled

      missing = @cache_mu.synchronize do
        indices.select { |p| !@page_cache.has?(p) && !@in_flight.has?(p) }
      end
      if missing.empty?
        @prefetch_mu.synchronize { @eager_complete = true unless @eager_cancel }
        return
      end

      ranges = Nxs.clamp_page_ranges(
        Nxs.coalesce_page_indices(missing, @coalesce_gap_pages, page_size),
        @data.bytesize
      )
      ranges.each do |r|
        break if @prefetch_mu.synchronize { @eager_cancel }

        fetch_coalesced_range!(r)
      end
      @prefetch_mu.synchronize { @eager_complete = true unless @eager_cancel }
    end

    def fetch_coalesced_range!(page_range)
      @cache_mu.synchronize { fetch_coalesced_range_unlocked!(page_range) }
    end

    def fetch_coalesced_range_unlocked!(page_range)
      blob = fetch_range_bytes!(page_range[:byte_start], page_range[:byte_length])
      page_size = @prefetch_page_size
      (page_range[:page_start]..page_range[:page_end]).each do |p|
        next if @page_cache.has?(p)

        page_off = p * page_size - page_range[:byte_start]
        page_len = [page_size, blob.bytesize - page_off].min
        next if page_len <= 0

        @page_cache.set(p, blob[page_off, page_len])
      end
    end

    def fetch_range_bytes!(byte_start, byte_length)
      @fetches_issued += 1
      @fetch_range.call(byte_start, byte_length)
    end

    def parse_layout_tail!(preamble_tail)
      if (@flags & FLAG_COLUMNAR != 0) && (@flags & FLAG_PAX != 0)
        raise NxsError.new('ERR_INVALID_FLAGS', 'columnar and PAX both set')
      end
      if (@flags & FLAG_COLUMNAR != 0) && preamble_tail.zero?
        raise NxsError.new('ERR_INCOMPATIBLE_FLAGS', 'columnar with TailPtr=0')
      end

      if (@flags & FLAG_COLUMNAR) != 0
        @layout = :columnar
        parse_columnar_footer!
        return
      end
      if (@flags & FLAG_PAX) != 0
        @layout = :pax
        parse_pax_footer!
        return
      end

      @layout = :row
      if preamble_tail.zero?
        raise NxsError.new('ERR_OUT_OF_BOUNDS', 'streamable footer') if @data.bytesize < 44

        @tail_ptr = @data.unpack1("@#{@data.bytesize - FOOTER_ROW_BYTES}Q<")
      end
      raise NxsError.new('ERR_OUT_OF_BOUNDS', 'tail index') if @tail_ptr + 4 > @data.bytesize

      @record_count = @data.unpack1("@#{@tail_ptr}L<")
      @tail_start   = @tail_ptr + 4
    end

    def parse_columnar_footer!
      sz = @data.bytesize
      raise NxsError.new('ERR_OUT_OF_BOUNDS', 'columnar footer') if sz < FOOTER_COL_BYTES

      fo = sz - FOOTER_COL_BYTES
      @tail_ptr      = @data.unpack1("@#{fo}Q<")
      @record_count  = @data.unpack1("@#{fo + 8}Q<")
      @tail_start    = @tail_ptr
      kc = @keys.length
      @col_buf_off = Array.new(kc)
      @col_buf_len = Array.new(kc)
      kc.times do |i|
        e = @tail_start + i * COL_TAIL_ENTRY_BYTES
        raise NxsError.new('ERR_OUT_OF_BOUNDS', 'columnar tail entry') if e + COL_TAIL_ENTRY_BYTES > sz

        fid = @data.unpack1("@#{e}S<")
        raise NxsError.new('ERR_OUT_OF_BOUNDS', "invalid field ID #{fid}") if fid >= kc

        @col_buf_off[fid] = @data.unpack1("@#{e + 4}Q<")
        @col_buf_len[fid] = @data.unpack1("@#{e + 12}Q<")
      end
    end

    def parse_pax_footer!
      sz = @data.bytesize
      raise NxsError.new('ERR_OUT_OF_BOUNDS', 'PAX footer') if sz < FOOTER_PAX_BYTES

      fo = sz - FOOTER_PAX_BYTES
      @tail_ptr       = @data.unpack1("@#{fo}Q<")
      @record_count   = @data.unpack1("@#{fo + 8}Q<")
      @page_count     = @data.unpack1("@#{fo + 16}L<")
      @page_size_hint = @data.unpack1("@#{fo + 20}L<")
      @tail_start     = @tail_ptr
      @page_index     = []
      @page_rec_start = []
      @page_rec_count = []
      @page_offset    = []
      @page_length    = []

      @page_count.times do |i|
        e = @tail_start + i * PAX_TAIL_ENTRY_BYTES
        raise NxsError.new('ERR_OUT_OF_BOUNDS', 'PAX tail entry') if e + PAX_TAIL_ENTRY_BYTES > sz

        @page_index << @data.unpack1("@#{e}L<")
        @page_rec_start << @data.unpack1("@#{e + 4}Q<")
        @page_rec_count << @data.unpack1("@#{e + 12}L<")
        @page_offset << @data.unpack1("@#{e + 16}Q<")
        @page_length << @data.unpack1("@#{e + 24}L<")
      end

      @page_count.times do |i|
        poff = @page_offset[i]
        if poff > sz || poff + 4 > sz || @data.unpack1("@#{poff}L<") != MAGIC_PAGE
          raise NxsError.new('ERR_INVALID_PAGE_MAGIC', 'PAX page magic mismatch')
        end
      end
    end

    def null_bitmap_bytes(n)
      raw = (n + 7) / 8
      (raw + 7) & ~7
    end

    # rubocop:disable Naming/PredicateMethod -- mirrors C col_bit naming
    def col_bit(bm, rec)
      ((bm.getbyte(rec / 8) >> (rec % 8)) & 1) == 1
    end
    # rubocop:enable Naming/PredicateMethod

    def var_sigil?(sig)
      [0x22, 0x3C].include?(sig)
    end

    def var_off_bytes_len(rc)
      off = (rc + 1) * 4
      raise NxsError.new('ERR_OUT_OF_BOUNDS', 'var offsets overflow') if off > @data.bytesize

      off
    end

    def field_sector_len(sector_off, rc, sigil)
      bm_len = null_bitmap_bytes(rc)
      return bm_len + rc * 8 unless var_sigil?(sigil)

      off_bytes = var_off_bytes_len(rc)
      raise NxsError.new('ERR_OUT_OF_BOUNDS', 'var offsets') if sector_off + bm_len + off_bytes > @data.bytesize

      end_off = @data.unpack1("@#{sector_off + bm_len + rc * 4}L<")
      total = bm_len + off_bytes + end_off
      raise NxsError.new('ERR_OUT_OF_BOUNDS', 'var values') if sector_off + total > @data.bytesize

      total
    end

    def var_str_at(offsets, values, record_index)
      need = (record_index + 2) * 4
      return nil if offsets.bytesize < need

      off = record_index * 4
      start = offsets.unpack1("@#{off}L<")
      end_  = offsets.unpack1("@#{off + 4}L<")
      return nil if end_ < start || end_ > values.bytesize

      values[start...end_].force_encoding('UTF-8')
    end

    def col_field_parts(slot)
      sector = column_sector(slot)
      bm_len = null_bitmap_bytes(@record_count)
      raise NxsError.new('ERR_OUT_OF_BOUNDS', 'null bitmap') if sector.bytesize < bm_len

      [sector[0, bm_len], sector[bm_len..]]
    end

    def init_column_prefetch!(fetch_range: nil)
      return unless @layout == :columnar

      @col_mu = Mutex.new
      @col_warmed = {}
      @col_overlay = {}
      @col_fetches = 0
      @col_remote_fetch = !fetch_range.nil?
      data = @data
      @col_fetch_range = fetch_range || ->(off, len) { data[off, len] }
    end

    def column_sector(slot)
      raise NxsError.new('ERR_OUT_OF_BOUNDS', "key slot #{slot}") if slot.negative? || slot >= @col_buf_off.length

      off = @col_buf_off[slot].to_i
      length = @col_buf_len[slot].to_i
      if @col_warmed
        @col_mu.synchronize do
          overlay = @col_overlay[slot]
          return overlay[0, length] if @col_warmed[slot] && overlay && !overlay.empty?
        end
      end
      raise NxsError.new('ERR_OUT_OF_BOUNDS', 'column buffer') if off + length > @data.bytesize

      @data[off, length]
    end

    def col_var_parts(slot)
      bm, tail = col_field_parts(slot)
      off_bytes = var_off_bytes_len(@record_count)
      raise NxsError.new('ERR_OUT_OF_BOUNDS', 'var offsets') if tail.bytesize < off_bytes

      [bm, tail[0, off_bytes], tail[off_bytes..]]
    end

    def col_var_parts_at(rec, slot)
      return [nil, nil, nil, false] if slot.negative? || slot >= @key_sigils.length || !var_sigil?(@key_sigils[slot])

      if @layout == :columnar
        bm, offsets, values = col_var_parts(slot)
        return [bm, offsets, values, true]
      end
      if @layout == :pax
        loc = pax_find_page(rec)
        return [nil, nil, nil, false] unless loc

        bm, tail = page_field_parts(loc[:page], slot)
        return [nil, nil, nil, false] unless bm

        rc = @page_rec_count[loc[:page]]
        off_bytes = var_off_bytes_len(rc)
        return [nil, nil, nil, false] if tail.bytesize < off_bytes

        return [bm, tail[0, off_bytes], tail[off_bytes..], true]
      end
      [nil, nil, nil, false]
    end

    def col_numeric_bytes(rec, slot)
      return nil if slot >= 0 && slot < @key_sigils.length && var_sigil?(@key_sigils[slot])

      if @layout == :columnar
        bm, vals = col_field_parts(slot)
        return nil if rec >= @record_count || !col_bit(bm, rec)

        off = rec * 8
        return nil if off + 8 > vals.bytesize

        return vals[off, 8]
      end
      if @layout == :pax
        loc = pax_find_page(rec)
        return nil unless loc

        bm, vals = page_field_parts(loc[:page], slot)
        return nil unless bm && col_bit(bm, loc[:local])

        off = loc[:local] * 8
        return nil if off + 8 > vals.bytesize

        return vals[off, 8]
      end
      nil
    end

    def pax_find_page(rec)
      return nil if @page_count.zero?

      lo = 0
      hi = @page_count - 1
      while lo <= hi
        mid = lo + (hi - lo) / 2
        start = @page_rec_start[mid]
        count = @page_rec_count[mid]
        if rec < start
          hi = mid - 1
        elsif rec >= start + count
          lo = mid + 1
        else
          return { page: mid, local: rec - start }
        end
      end
      nil
    end

    def page_field_sector(pi, slot)
      poff = @page_offset[pi].to_i
      return nil if poff + 24 > @data.bytesize || @data.unpack1("@#{poff}L<") != MAGIC_PAGE

      fc = @data.unpack1("@#{poff + 20}S<")
      return nil if slot.negative? || slot >= fc || fc > @key_sigils.length

      rc = @page_rec_count[pi]
      body = poff + 24
      slot.times do |fi|
        sig = fi < @key_sigils.length ? @key_sigils[fi] : 0x3D
        flen = field_sector_len(body, rc, sig)
        body += flen
      end
      sig = slot < @key_sigils.length ? @key_sigils[slot] : 0x3D
      flen = field_sector_len(body, rc, sig)
      return nil if body + flen > @data.bytesize

      @data[body, flen]
    end

    def page_field_parts(pi, slot)
      sector = page_field_sector(pi, slot)
      return [nil, nil] unless sector

      bm_len = null_bitmap_bytes(@page_rec_count[pi])
      return [nil, nil] if sector.bytesize < bm_len

      [sector[0, bm_len], sector[bm_len..]]
    end

    def pax_sum_f64(slot)
      sum = 0.0
      @page_count.times do |pi|
        bm, vals = page_field_parts(pi, slot)
        next unless bm

        rc = @page_rec_count[pi]
        i = 0
        while i < rc
          if col_bit(bm, i)
            off = i * 8
            sum += vals.unpack1("@#{off}E") if off + 8 <= vals.bytesize
          end
          i += 1
        end
      end
      sum
    end

    def read_schema(offset)
      key_count = @data.unpack1("@#{offset}S<")
      offset += 2

      @key_sigils = @data[offset, key_count].bytes
      offset += key_count

      # Null-terminated UTF-8 strings in StringPool
      pool = @data[offset..]
      pos  = 0
      key_count.times do |i|
        term = pool.index("\x00", pos)
        @keys << pool[pos...term].force_encoding('UTF-8')
        @key_index[@keys.last] = i
        pos = term + 1
      end
      offset += pos

      # Pad to 8-byte boundary
      rem = offset % 8
      offset += (8 - rem) % 8
      offset
    end

    MURMUR_C1 = 0xFF51AFD7ED558CCD
    MURMUR_C2 = 0xC4CEB9FE1A85EC53
    MURMUR_MASK = 0xFFFFFFFFFFFFFFFF

    def murmur3_64(bytes)
      h = 0x93681D6255313A99
      i = 0
      len = bytes.length
      while i < len
        chunk = bytes[i, 8]
        k = 0
        chunk.each_with_index { |b, j| k |= b << (j * 8) }
        k = (k * MURMUR_C1) & MURMUR_MASK
        k ^= k >> 33
        h ^= k
        h = (h * MURMUR_C2) & MURMUR_MASK
        h ^= h >> 33
        i += 8
      end
      h ^= len
      h ^= h >> 33
      h = (h * MURMUR_C1) & MURMUR_MASK
      h ^= h >> 33
      h
    end
  end

  # ── Query engine ─────────────────────────────────────────────────────────────

  # Base predicate — supports & | ~ operator overloading.
  class Predicate
    def &(other)  = And.new(self, other)
    def |(other)  = Or.new(self, other)
    def ~@        = Not.new(self)
    def call(_record) = raise NotImplementedError, "#{self.class}#call not implemented"
  end

  # Eq(key, value) — equality for String, Integer, Float, or boolean.
  class Eq < Predicate
    def initialize(key, value)
      super()
      @key   = key
      @value = value
    end

    def call(record) = record[@key] == @value
  end

  # Gt(key, number) — numeric greater-than.
  class Gt < Predicate
    def initialize(key, value)
      super()
      @key   = key
      @value = value
    end

    def call(record)
      v = record[@key]
      v.is_a?(Numeric) && v > @value
    end
  end

  # Lt(key, number) — numeric less-than.
  class Lt < Predicate
    def initialize(key, value)
      super()
      @key   = key
      @value = value
    end

    def call(record)
      v = record[@key]
      v.is_a?(Numeric) && v < @value
    end
  end

  # And(p1, p2) — conjunction.
  class And < Predicate
    def initialize(a, b)
      super()
      @a = a
      @b = b
    end

    def call(record) = @a.call(record) && @b.call(record)
  end

  # Or(p1, p2) — disjunction.
  class Or < Predicate
    def initialize(a, b)
      super()
      @a = a
      @b = b
    end

    def call(record) = @a.call(record) || @b.call(record)
  end

  # Not(p) — negation.
  class Not < Predicate
    def initialize(inner)
      super()
      @inner = inner
    end

    def call(record) = !@inner.call(record)
  end

  # ── Record proxy ─────────────────────────────────────────────────────────────

  # Thin hash-like wrapper around Nxs::Object so predicates can use record[key].
  # Values are fetched lazily and memoised per field access.
  class RecordProxy
    def initialize(obj, reader)
      @obj    = obj
      @reader = reader
      @cache  = {}
    end

    # Reset to a new underlying object, clearing the field cache.
    # Used by Query#each to reuse a single RecordProxy instance across iterations.
    def reset(obj)
      @obj   = obj
      @cache = {}
    end

    def [](key)
      return @cache[key] if @cache.key?(key)

      slot = @reader.key_index[key]
      unless slot
        @cache[key] = nil
        return nil
      end

      sigil = @reader.key_sigils[slot]
      val = case sigil
            when 0x22 then @obj.get_str(key)   # '"' string
            when 0x3D then @obj.get_i64(key)   # '=' i64
            when 0x7E then @obj.get_f64(key)   # '~' f64
            when 0x3F then @obj.get_bool(key)  # '?' bool
            end
      @cache[key] = val
    end
  end

  # ── Query ─────────────────────────────────────────────────────────────────────

  # Lazy filtered view over a Reader.  Created via reader.where(pred) or reader.all.
  #
  # Includes Enumerable, so map/select/min/max etc. all work automatically.
  # count and first are overridden for clarity (Enumerable would work too).
  class Query
    include Enumerable

    def initialize(reader, pred = nil)
      @reader = reader
      @pred   = pred
    end

    # Yield each matching Nxs::Object to the block.
    def each
      n    = @reader.record_count
      pred = @pred
      proxy = RecordProxy.new(nil, @reader)
      i = 0
      while i < n
        obj = @reader.record(i)
        if pred.nil?
          yield obj
        else
          proxy.reset(obj)
          yield obj if pred.call(proxy)
        end
        i += 1
      end
    end

    # Number of matching records (no block form; delegates to Enumerable when block given).
    def count(&blk)
      return super if blk

      n = 0
      each { n += 1 }
      n
    end

    # First matching record, or nil.
    def first
      find { true }
    end
  end

  # ── Reader extensions ────────────────────────────────────────────────────────

  class Reader
    # Returns a Query filtered by pred.
    def where(pred) = Query.new(self, pred)

    # Returns a Query over all records.
    def all = Query.new(self)

    # Expose key_sigils for RecordProxy
    attr_reader :key_sigils
  end

  # ── Object ───────────────────────────────────────────────────────────────────

  class Object
    def initialize(reader, offset, record_index = nil)
      @reader       = reader
      @offset       = offset
      @record_index = record_index
      @parsed       = false
    end

    def get_str(key)
      slot = @reader.key_index[key]
      return nil unless slot

      return @reader.col_get_str(key, record_index) if uses_columnar_field_access?

      off = field_offset(key)
      return nil unless off

      len = @reader.data.unpack1("@#{off}L<")
      @reader.data[off + 4, len].force_encoding('UTF-8')
    end

    def get_i64(key)
      slot = @reader.key_index[key]
      return nil unless slot

      if uses_columnar_field_access?
        cell = @reader.send(:col_numeric_bytes, record_index, slot)
        return nil unless cell

        return cell.unpack1('q<')
      end

      off = field_offset(key)
      return nil unless off

      @reader.data.unpack1("@#{off}q<")
    end

    def get_f64(key)
      slot = @reader.key_index[key]
      return nil unless slot

      if uses_columnar_field_access?
        cell = @reader.send(:col_numeric_bytes, record_index, slot)
        return nil unless cell

        return cell.unpack1('E')
      end

      off = field_offset(key)
      return nil unless off

      @reader.data.unpack1("@#{off}E")
    end

    def get_bool(key)
      slot = @reader.key_index[key]
      return nil unless slot

      if uses_columnar_field_access?
        cell = @reader.send(:col_numeric_bytes, record_index, slot)
        return nil unless cell

        return cell.getbyte(0) != 0
      end

      off = field_offset(key)
      return nil unless off

      @reader.data.getbyte(off) != 0
    end

    private

    def record_index
      @record_index.nil? ? @offset : @record_index
    end

    def obj_at_nyxo?
      return false if @offset + 4 > @reader.data.bytesize

      @reader.data.unpack1("@#{@offset}L<") == MAGIC_OBJ
    end

    # Columnar/PAX top-level records use record index; nested NYXO blobs use row paths.
    def uses_columnar_field_access?
      @reader.layout != :row && !obj_at_nyxo?
    end

    # Parse the object header (lazy — only on first field access).
    def parse_header
      return if @parsed

      p = @offset

      magic = @reader.data.unpack1("@#{p}L<")
      raise NxsError.new('ERR_BAD_MAGIC', "expected NYXO at #{p}") if magic != MAGIC_OBJ

      p += 8 # skip Magic(4) + Length(4)

      bitmask = []
      loop do
        b = @reader.data.getbyte(p)
        p += 1
        bitmask << (b & 0x7F)
        break if (b & 0x80).zero?
      end

      @bitmask          = bitmask
      @offset_tbl_start = p
      @parsed           = true
    end

    # Return the absolute byte offset of the field for `key`, or nil.
    def field_offset(key)
      slot = @reader.key_index[key]
      return nil unless slot

      # Delegate to Reader's scan logic (same implementation, avoids duplication)
      @reader._scan_offset(@reader.data, @offset, slot)
    end
  end
end
