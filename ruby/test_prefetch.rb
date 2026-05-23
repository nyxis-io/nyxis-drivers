# frozen_string_literal: true

# Adaptive prefetch unit tests — run: ruby test_prefetch.rb

require_relative 'nxs'
require_relative 'nxs_writer'

PASS = "\e[32mPASS\e[0m"
FAIL = "\e[31mFAIL\e[0m"

passes = 0
fails  = 0

def check(label, &blk)
  if blk.call
    puts "  #{PASS}  #{label}"
    true
  else
    puts "  #{FAIL}  #{label}"
    false
  end
rescue StandardError => e
  puts "  #{FAIL}  #{label} — exception: #{e}"
  false
end

def build_records(n)
  schema = Nxs::Schema.new(%w[id username score active])
  w = Nxs::Writer.new(schema)
  n.times do |i|
    w.begin_object
    w.write_i64(0, i)
    w.write_str(1, "user_#{i}")
    w.write_f64(2, i * 0.25)
    w.write_bool(3, i.even?)
    w.end_object
  end
  w.finish
end

puts
puts 'NXS Ruby Prefetch — Tests'
puts '━' * 60
puts

[
  check('coalesce_page_indices [3,4,6,7,12] gap=1 → 3 ranges') do
    r = Nxs.coalesce_page_indices([3, 4, 6, 7, 12], 1, Nxs::DEFAULT_PAGE_SIZE)
    r.length == 3 &&
      r[0][:page_start] == 3 && r[0][:page_end] == 4 &&
      r[1][:page_start] == 6 && r[1][:page_end] == 7 &&
      r[2][:page_start] == 12 && r[2][:page_end] == 12
  end,

  check('PageCache LRU evicts at max_pages') do
    c = Nxs::PageCache.new(2, 64)
    c.set(0, "\x00".b * 64)
    c.set(1, "\x00".b * 64)
    c.get(0)
    c.set(2, "\x00".b * 64)
    !c.has?(1) && c.has?(0) && c.has?(2)
  end,

  check('InFlightMap dedupes concurrent page loads') do
    m = Nxs::InFlightMap.new
    fetches = 0
    mu = Mutex.new
    t1 = Thread.new do
      m.with(3) do
        sleep 0.01
        mu.synchronize { fetches += 1 }
        'page'
      end
    end
    sleep 0.001
    t2 = Thread.new { m.with(3) { 'unused' } }
    [t1, t2].each(&:join)
    fetches == 1
  end,

  check('prefetch_viewport uses ≤3 coalesced fetch_range calls for 50 records') do
    buf = build_records(60)
    ranges = []
    reader = Nxs::Reader.new(
      buf,
      max_pages: 64,
      coalesce_gap_pages: 1,
      fetch_range: lambda do |start, len|
        ranges << { start: start, len: len }
        buf[start, len]
      end
    )
    reader.prefetch_viewport(0, 49)
    ranges.length <= 3 &&
      reader.cache_stats[:fetches_issued] == ranges.length
  end,

  check('prefetch_viewport_basic — records readable after prefetch') do
    buf = build_records(55)
    reader = Nxs::Reader.new(buf)
    reader.prefetch_viewport(0, 49)
    reader.record(49).get_i64('id') == 49
  end,

  check('prefetch_memory_eviction') do
    buf = build_records(20)
    reader = Nxs::Reader.new(buf, max_pages: 2, page_size: 256, coalesce_gap_pages: 0)
    reader.prefetch_viewport(0, 0)
    reader.prefetch_viewport(19, 19)
    reader.cache_stats[:pages_cached] <= 2
  end,

  check('prefetch_deduplication — parallel viewport same page') do
    buf = build_records(10)
    calls = 0
    mu = Mutex.new
    reader = Nxs::Reader.new(
      buf,
      max_pages: 8,
      fetch_range: lambda do |start, len|
        mu.synchronize { calls += 1 }
        sleep 0.005
        buf[start, len]
      end
    )
    t1 = Thread.new { reader.prefetch_viewport(0, 4) }
    t2 = Thread.new { reader.prefetch_viewport(0, 4) }
    [t1, t2].each(&:join)
    calls <= 3
  end,

  check('cache_stats returns expected keys') do
    buf = build_records(5)
    reader = Nxs::Reader.new(buf)
    reader.prefetch_viewport(0, 4)
    stats = reader.cache_stats
    %i[pages_cached pages_max memory_used_bytes cache_hits cache_misses
       fetches_issued strategy pattern].all? { |k| stats.key?(k) } &&
      stats[:strategy] == 'lazy' && stats[:pattern] == 'unknown'
  end,

  check('prefetch_viewport out-of-bounds raises') do
    buf = build_records(5)
    reader = Nxs::Reader.new(buf)
    begin
      reader.prefetch_viewport(0, 10)
      false
    rescue Nxs::NxsError => e
      e.code == 'ERR_OUT_OF_BOUNDS'
    end
  end
].each { |r| r ? (passes += 1) : (fails += 1) }

puts
puts '━' * 60
puts "  Results: #{passes} passed, #{fails} failed"
puts

exit(fails.zero? ? 0 : 1)
