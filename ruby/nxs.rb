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
  MAGIC_FOOTER = 0x2153584E  # NXS!
  FLAG_SCHEMA  = 0x0002

  class NxsError < StandardError
    attr_reader :code

    def initialize(code, msg)
      super("#{code}: #{msg}")
      @code = code
    end
  end

  # ── Reader ──────────────────────────────────────────────────────────────────

  class Reader
    attr_reader :keys, :record_count

    def initialize(bytes)
      @data = bytes.b # force binary encoding
      sz = @data.bytesize
      raise NxsError.new('ERR_OUT_OF_BOUNDS', 'file too small') if sz < 32

      magic = @data.unpack1('L<')
      raise NxsError.new('ERR_BAD_MAGIC', "expected NYXB, got 0x#{magic.to_s(16)}") if magic != MAGIC_FILE

      footer = @data.unpack1("@#{sz - 4}L<")
      raise NxsError.new('ERR_BAD_MAGIC', 'footer magic mismatch') if footer != MAGIC_FOOTER

      # Preamble: Version(2) + Flags(2) + DictHash(8) + TailPtr(8) + Reserved(8)
      @flags    = @data.unpack1('@6 S<')
      @tail_ptr = @data.unpack1('@16 Q<')
      if @tail_ptr.zero?
        raise NxsError.new('ERR_OUT_OF_BOUNDS', 'stream footer') if sz < 44

        @tail_ptr = @data.unpack1("@#{sz - 12}Q<")
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

      # Tail-index: u32 EntryCount followed by records
      @record_count = @data.unpack1("@#{@tail_ptr}L<")
      @tail_start   = @tail_ptr + 4
    end

    # O(1) record lookup — reads one 10-byte tail-index entry.
    def record(i)
      unless i >= 0 && i < @record_count
        raise NxsError.new('ERR_OUT_OF_BOUNDS', "record #{i} out of [0, #{@record_count})")
      end

      # Each tail-index entry: u16 KeyID + u64 AbsoluteOffset = 10 bytes
      abs_offset = @data.unpack1("@#{@tail_start + i * 10 + 2}Q<")
      Object.new(self, abs_offset)
    end

    # Tight allocation-free sum loop.
    def sum_f64(key)
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
    def initialize(reader, offset)
      @reader = reader
      @offset = offset
      @parsed = false
    end

    def get_str(key)
      off = field_offset(key)
      return nil unless off

      len = @reader.data.unpack1("@#{off}L<")
      @reader.data[off + 4, len].force_encoding('UTF-8')
    end

    def get_i64(key)
      off = field_offset(key)
      return nil unless off

      @reader.data.unpack1("@#{off}q<")
    end

    def get_f64(key)
      off = field_offset(key)
      return nil unless off

      @reader.data.unpack1("@#{off}E")
    end

    def get_bool(key)
      off = field_offset(key)
      return nil unless off

      @reader.data.getbyte(off) != 0
    end

    private

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
