# frozen_string_literal: true

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

    def initialize(bytes)
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
    end

    # O(1) record lookup — row tail-index or columnar/PAX record index.
    def record(i)
      unless i >= 0 && i < @record_count
        raise NxsError.new('ERR_OUT_OF_BOUNDS', "record #{i} out of [0, #{@record_count})")
      end

      return Object.new(self, i, i) if @layout != :row

      abs_offset = @data.unpack1("@#{@tail_start + i * 10 + 2}Q<")
      Object.new(self, abs_offset)
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

    private

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
      raise NxsError.new('ERR_OUT_OF_BOUNDS', "key slot #{slot}") if slot.negative? || slot >= @col_buf_off.length

      off = @col_buf_off[slot].to_i
      length = @col_buf_len[slot].to_i
      raise NxsError.new('ERR_OUT_OF_BOUNDS', 'column buffer') if off + length > @data.bytesize

      bm_len = null_bitmap_bytes(@record_count)
      raise NxsError.new('ERR_OUT_OF_BOUNDS', 'null bitmap') if length < bm_len

      sector = @data[off, length]
      [sector[0, bm_len], sector[bm_len..]]
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

    def uses_columnar_field_access?
      return false if @reader.layout == :row
      return false if @offset + 4 > @reader.data.bytesize

      @reader.data.unpack1("@#{@offset}L<") != MAGIC_OBJ
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
