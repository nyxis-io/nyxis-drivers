# frozen_string_literal: true

# NXS Writer — direct-to-buffer .nxb emitter for Ruby 3.x.
#
# Mirrors the Rust NxsWriter API:
#   NxsSchema — precompile keys once; share across NxsWriter instances.
#   NxsWriter — slot-based hot path; no per-key hash lookups during write.
#
# Usage:
#   require_relative "nxs_writer"
#
#   schema = Nxs::Schema.new(["id", "username", "score", "active"])
#   w = Nxs::Writer.new(schema)
#   w.begin_object
#   w.write_i64(0, 42)
#   w.write_str(1, "alice")
#   w.write_f64(2, 9.5)
#   w.write_bool(3, true)
#   w.end_object
#   bytes = w.finish   # => binary String (encoding: ASCII-8BIT)

module Nxs
  MAGIC_FILE           = 0x4E595842  # NYXB
  MAGIC_OBJ            = 0x4E59584F  # NYXO
  MAGIC_LIST           = 0x4E59584C  # NYXL
  MAGIC_FOOTER         = 0x2153584E  # NXS!
  VERSION              = 0x0101
  FLAG_SCHEMA_EMBEDDED = 0x0002

  # ── MurmurHash3-64 ────────────────────────────────────────────────────────────

  MASK64 = 0xFFFFFFFFFFFFFFFF
  C1_MM3 = 0xFF51AFD7ED558CCD
  C2_MM3 = 0xC4CEB9FE1A85EC53

  def self.murmur3_64(data)
    h = 0x93681D6255313A99
    length = data.bytesize
    i = 0
    while i < length
      k = 0
      8.times { |b| k |= data.getbyte(i + b) << (b * 8) if i + b < length }
      k = (k * C1_MM3) & MASK64
      k ^= k >> 33
      h ^= k
      h = (h * C2_MM3) & MASK64
      h ^= h >> 33
      i += 8
    end
    h ^= length
    h ^= h >> 33
    h = (h * C1_MM3) & MASK64
    h ^= h >> 33
    h
  end

  # ── Schema ────────────────────────────────────────────────────────────────────

  class Schema
    attr_reader :keys, :bitmask_bytes

    def initialize(keys)
      @keys = keys.map(&:freeze).freeze
      @bitmask_bytes = [((keys.length + 6) / 7), 1].max
    end

    def length = @keys.length
  end

  # ── Frame (per open object) ───────────────────────────────────────────────────

  Frame = Struct.new(:start, :bitmask, :offset_table, :slot_offsets,
                     :last_slot, :needs_sort)

  # ── Writer ────────────────────────────────────────────────────────────────────

  class Writer
    def initialize(schema)
      @schema         = schema
      @buf            = String.new(encoding: "BINARY")
      @frames         = []
      @record_offsets = []
    end

    def begin_object
      @record_offsets << @buf.bytesize if @frames.empty?
      start = @buf.bytesize

      bm = "\x00".b * @schema.bitmask_bytes
      (@schema.bitmask_bytes - 1).times { |i| bm.setbyte(i, 0x80) }

      @frames << Frame.new(start, bm.dup, [], [], -1, false)

      _write_u32(MAGIC_OBJ)
      _write_u32(0)                              # length placeholder
      @buf << bm                                 # bitmask placeholder
      @buf << ("\x00".b * (@schema.length * 2)) # offset table placeholder

      @buf << "\x00".b while (@buf.bytesize - start) % 8 != 0
    end

    def end_object
      raise "end_object without begin_object" if @frames.empty?
      frame = @frames.pop

      total_len = @buf.bytesize - frame.start
      @buf[frame.start + 4, 4] = [total_len].pack("V")

      bm_off = frame.start + 8
      frame.bitmask.bytesize.times { |i| @buf.setbyte(bm_off + i, frame.bitmask.getbyte(i)) }

      ot_start = bm_off + @schema.bitmask_bytes
      present  = frame.offset_table.length

      if !frame.needs_sort
        frame.offset_table.each_with_index do |rel, i|
          @buf[ot_start + i * 2, 2] = [rel].pack("v")
        end
      else
        frame.slot_offsets.sort_by { |s, _| s }.each_with_index do |(_, buf_off), i|
          @buf[ot_start + i * 2, 2] = [buf_off - frame.start].pack("v")
        end
      end

      # Zero unused slots
      (present * 2...@schema.length * 2).each { |i| @buf.setbyte(ot_start + i, 0) }
    end

    def finish
      raise "unclosed objects" unless @frames.empty?

      schema_bytes   = _build_schema
      dict_hash      = Nxs.murmur3_64(schema_bytes)
      data_start_abs = 32 + schema_bytes.bytesize

      data_sector = @buf.dup
      tail_ptr    = data_start_abs + data_sector.bytesize
      tail        = _build_tail_index(data_start_abs, tail_ptr)

      out = String.new(encoding: "BINARY")
      out << [MAGIC_FILE].pack("V")
      out << [VERSION, FLAG_SCHEMA_EMBEDDED].pack("vv")
      out << [dict_hash & 0xFFFFFFFF, dict_hash >> 32].pack("VV")
      out << [0, 0].pack("VV")
      out << "\x00".b * 8  # reserved

      out << schema_bytes
      out << data_sector
      out << tail
      out
    end

    # ── Typed write methods ──────────────────────────────────────────────────────

    def write_i64(slot, v)
      _mark_slot(slot)
      @buf << [v].pack("q<")
    end

    def write_f64(slot, v)
      _mark_slot(slot)
      @buf << [v].pack("E")
    end

    def write_bool(slot, v)
      _mark_slot(slot)
      @buf << (v ? "\x01" : "\x00").b
      @buf << "\x00".b * 7
    end

    def write_time(slot, unix_ns)
      write_i64(slot, unix_ns)
    end

    def write_null(slot)
      _mark_slot(slot)
      @buf << "\x00".b * 8
    end

    def write_str(slot, v)
      _mark_slot(slot)
      b = v.encode("UTF-8").b
      @buf << [b.bytesize].pack("V")
      @buf << b
      used = (4 + b.bytesize) % 8
      @buf << "\x00".b * (8 - used) if used != 0
    end

    def write_bytes(slot, data)
      _mark_slot(slot)
      @buf << [data.bytesize].pack("V")
      @buf << data.b
      used = (4 + data.bytesize) % 8
      @buf << "\x00".b * (8 - used) if used != 0
    end

    def write_list_i64(slot, values)
      _mark_slot(slot)
      total = 16 + values.length * 8
      @buf << [MAGIC_LIST, total, 0x3D, values.length].pack("VVCVx3")
      @buf << values.pack("q<*")
    end

    def write_list_f64(slot, values)
      _mark_slot(slot)
      total = 16 + values.length * 8
      @buf << [MAGIC_LIST, total, 0x7E, values.length].pack("VVCVx3")
      @buf << values.pack("E*")
    end

    # Convenience: write records from an array of hashes.
    def self.from_records(keys, records)
      schema = Schema.new(keys)
      w = new(schema)
      records.each do |rec|
        w.begin_object
        keys.each_with_index do |key, i|
          next unless rec.key?(key)
          val = rec[key]
          case val
          when NilClass  then w.write_null(i)
          when TrueClass, FalseClass then w.write_bool(i, val)
          when Integer   then w.write_i64(i, val)
          when Float     then w.write_f64(i, val)
          when String    then w.write_str(i, val)
          end
        end
        w.end_object
      end
      w.finish
    end

    private

    def _mark_slot(slot)
      raise "no active object" if @frames.empty?
      frame = @frames.last

      byte_idx = slot / 7
      bit_idx  = slot % 7
      frame.bitmask.setbyte(byte_idx, frame.bitmask.getbyte(byte_idx) | (1 << bit_idx))

      rel = @buf.bytesize - frame.start
      frame.needs_sort = true if slot < frame.last_slot
      frame.last_slot = slot

      frame.offset_table << rel
      frame.slot_offsets  << [slot, @buf.bytesize]
    end

    def _write_u32(v)
      @buf << [v].pack("V")
    end

    def _build_schema
      keys    = @schema.keys
      encoded = keys.map { |k| k.encode("UTF-8").b }
      size    = 2 + keys.length + encoded.sum(&:bytesize) + encoded.length
      padded  = size + ((-size) % 8)

      buf = String.new("\x00".b * padded, encoding: "BINARY")
      p = 0
      buf[p, 2] = [keys.length].pack("v"); p += 2
      keys.length.times { buf.setbyte(p, 0x22); p += 1 }  # '"' sigil
      encoded.each do |e|
        buf[p, e.bytesize] = e; p += e.bytesize
        buf.setbyte(p, 0x00);   p += 1
      end
      buf
    end

    def _build_tail_index(data_start, tail_ptr)
      n   = @record_offsets.length
      buf = String.new(encoding: "BINARY")
      buf << [n].pack("V")
      @record_offsets.each_with_index do |rel, i|
        abs = data_start + rel
        buf << [i].pack("v")
        buf << [abs & 0xFFFFFFFF, abs >> 32].pack("VV")
      end
      buf << [tail_ptr & 0xFFFFFFFF, tail_ptr >> 32].pack("VV")
      buf << [MAGIC_FOOTER].pack("V")
      buf
    end
  end
end
