# frozen_string_literal: true

# NXS vs JSON vs CSV benchmark — Ruby (stdlib only).
#
# 6 scenarios across 4 dataset sizes (1k, 10k, 100k, 1M records).
#
# Usage: ruby ruby/bench.rb [fixtures_dir]

require 'json'
require 'csv'
require_relative 'nxs'

# ── Helpers ──────────────────────────────────────────────────────────────────

# Returns average wall-clock seconds per iteration.
def bench(iters, &block)
  3.times(&block) # warmup
  t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  iters.times(&block)
  (Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0) / iters
end

def fmt_time(s)
  return "#{(s * 1e9).round}  ns" if s < 1e-6
  return "#{format('%.1f', s * 1e6)} µs" if s < 1e-3
  return "#{format('%.2f', s * 1e3)} ms" if s < 1

  "#{'%.2f' % s}  s"
end

def fmt_bytes(n)
  return "#{n} B" if n < 1024
  return "#{format('%.1f', n / 1024.0)} KB" if n < 1024 * 1024

  "#{format('%.2f', n / 1024.0 / 1024.0)} MB"
end

def row(label, avg, baseline)
  ratio = if avg == baseline
            'baseline'
          elsif avg < baseline
            "#{format('%.1f', baseline / avg)}x faster"
          else
            "#{format('%.1f', avg / baseline)}x slower"
          end
  printf "  │  %-42s %12s   %s\n", label, fmt_time(avg), ratio
end

def section(title)
  bar = '─' * [0, 76 - title.length].max
  puts "\n  ┌─ #{title} #{bar}┐"
end

def endsection
  puts "  └#{'─' * 79}┘"
end

# ── Per-size benchmark ───────────────────────────────────────────────────────

def run_scale(fixture_dir, n)
  nxb_path  = File.join(fixture_dir, "records_#{n}.nxb")
  json_path = File.join(fixture_dir, "records_#{n}.json")
  csv_path  = File.join(fixture_dir, "records_#{n}.csv")

  unless File.exist?(nxb_path) && File.exist?(json_path)
    puts "\n  ⚠  skipping n=#{n}: fixtures missing"
    return
  end

  nxb_buf  = File.binread(nxb_path)
  json_str = File.read(json_path, encoding: 'UTF-8')
  csv_str  = File.exist?(csv_path) ? File.read(csv_path, encoding: 'UTF-8') : nil

  puts "\n#{'━' * 40}  n = #{n.to_s.reverse.gsub(/(\d{3})(?=\d)/, '\\1,').reverse}  #{'━' * 38}"
  printf "  .nxb  size:  %10s\n", fmt_bytes(nxb_buf.bytesize)
  printf "  .json size:  %10s  (%.2fx NXS)\n", fmt_bytes(json_str.bytesize), json_str.bytesize.to_f / nxb_buf.bytesize

  # Scale iteration counts to keep wall-clock reasonable
  if n >= 1_000_000
    parse_iters = 3
    rand_iters = 5_000
    iter_iters = 2
    cold_iters = 3
  elsif n >= 100_000
    parse_iters = 20
    rand_iters = 20_000
    iter_iters = 5
    cold_iters = 15
  elsif n >= 10_000
    parse_iters = 200
    rand_iters = 50_000
    iter_iters = 50
    cold_iters = 100
  else
    parse_iters = 2_000
    rand_iters = 100_000
    iter_iters = 500
    cold_iters = 500
  end

  rng     = Random.new(0)
  indices = Array.new(rand_iters) { rng.rand(n) }

  # ── 1. Open file ────────────────────────────────────────────────────────────
  section('Open file (parse / index full structure)')
  t_json_open = bench(parse_iters) { JSON.parse(json_str) }
  row('JSON.parse(entire document)', t_json_open, t_json_open)

  if csv_str
    t_csv_open = bench(parse_iters) { CSV.parse(csv_str, headers: true) }
    row('CSV.parse(entire document)', t_csv_open, t_json_open)
  end

  t_nxs_open = bench(parse_iters) { Nxs::Reader.new(nxb_buf) }
  row('Nxs::Reader.new(buffer)', t_nxs_open, t_json_open)
  endsection

  # Pre-build warm objects for remaining benchmarks
  parsed  = JSON.parse(json_str)
  reader  = Nxs::Reader.new(nxb_buf)
  csv_tab = csv_str ? CSV.parse(csv_str, headers: true) : nil

  # ── 2. Warm random access ───────────────────────────────────────────────────
  section('Warm random-access read (1 field from 1 record)')
  ri = 0
  t_json_rand = bench(rand_iters) { parsed[indices[ri = (ri + 1) % rand_iters]]['username'] }
  row("parsed_json[k]['username']", t_json_rand, t_json_rand)

  if csv_tab
    ri2 = 0
    t_csv_rand = bench(rand_iters) { csv_tab[indices[ri2 = (ri2 + 1) % rand_iters]]['username'] }
    row("csv_table[k]['username']", t_csv_rand, t_json_rand)
  end

  ri3 = 0
  t_nxs_rand = bench(rand_iters) { reader.record(indices[ri3 = (ri3 + 1) % rand_iters]).get_str('username') }
  row("reader.record(k).get_str('username')", t_nxs_rand, t_json_rand)
  endsection

  # ── 3. Cold start ───────────────────────────────────────────────────────────
  section('Cold start — open file + read 1 field')
  cold_k = n / 2
  t_json_cold = bench(cold_iters) { JSON.parse(json_str)[cold_k]['username'] }
  row("JSON.parse + arr[k]['username']", t_json_cold, t_json_cold)

  if csv_str
    t_csv_cold = bench(cold_iters) { CSV.parse(csv_str, headers: true)[cold_k]['username'] }
    row("CSV.parse + table[k]['username']", t_csv_cold, t_json_cold)
  end

  t_nxs_cold = bench(cold_iters) { Nxs::Reader.new(nxb_buf).record(cold_k).get_str('username') }
  row('Nxs::Reader.new + record(k).get_str', t_nxs_cold, t_json_cold)
  endsection

  # ── 4. Full scan per-record ──────────────────────────────────────────────────
  section('Full scan per-record — sum score via record(i).get_f64')
  t_json_scan = bench(iter_iters) do
    s = 0.0
    parsed.each { |r| s += r['score'].to_f }
    s
  end
  row("arr.each { |r| sum += r['score'] }", t_json_scan, t_json_scan)

  if csv_tab
    t_csv_scan = bench(iter_iters) do
      s = 0.0
      csv_tab.each { |r| s += r['score'].to_f }
      s
    end
    row("csv.each { |r| sum += r['score'] }", t_csv_scan, t_json_scan)
  end

  t_nxs_scan = bench(iter_iters) do
    s = 0.0
    n.times do |i|
      v = reader.record(i).get_f64('score')
      s += v if v
    end
    s
  end
  row('n.times { reader.record(i).get_f64 }', t_nxs_scan, t_json_scan)
  endsection

  # ── 5. Reducer ──────────────────────────────────────────────────────────────
  section("Reducer — reader.sum_f64('score') tight loop")
  t_json_red = bench(iter_iters) do
    s = 0.0
    parsed.each { |r| s += r['score'].to_f }
    s
  end
  row('json: arr.each sum (baseline)', t_json_red, t_json_red)

  if csv_tab
    t_csv_red = bench(iter_iters) do
      s = 0.0
      csv_tab.each { |r| s += r['score'].to_f }
      s
    end
    row('csv: table.each sum', t_csv_red, t_json_red)
  end

  t_nxs_red = bench(iter_iters) { reader.sum_f64('score') }
  row("reader.sum_f64('score')", t_nxs_red, t_json_red)
  endsection

  # ── 6. Cold pipeline ────────────────────────────────────────────────────────
  section('Cold pipeline — read file from disk + sum scores')
  t_json_pipe = bench(cold_iters) do
    s = 0.0
    JSON.parse(json_str).each { |r| s += r['score'].to_f }
    s
  end
  row('JSON.parse + sum scores', t_json_pipe, t_json_pipe)

  t_nxs_pipe = bench(cold_iters) { Nxs::Reader.new(nxb_buf).sum_f64('score') }
  row('Nxs::Reader.new + sum_f64', t_nxs_pipe, t_json_pipe)
  endsection
end

# ── Main ─────────────────────────────────────────────────────────────────────

fixture_dir = ARGV[0] || '../js/fixtures'

puts
puts '╔════════════════════════════════════════════════════════════════════════════════╗'
puts '║         NXS vs JSON vs CSV  —  Ruby Benchmark                                 ║'
puts '╚════════════════════════════════════════════════════════════════════════════════╝'
printf "\n  Ruby:     %s\n", RUBY_DESCRIPTION
printf "  Platform: %s-%s\n", RUBY_PLATFORM, RbConfig::CONFIG['host_cpu']
printf "  Fixtures: %s\n", fixture_dir

[1_000, 10_000, 100_000, 1_000_000].each do |n|
  run_scale(fixture_dir, n)
rescue StandardError => e
  puts "\n  ⚠  n=#{n} failed: #{e.message}"
  puts(e.backtrace.first(3).map { |l| "       #{l}" })
end

puts
puts '═' * 80
puts '  Notes:'
puts '    • JSON.parse materialises the entire document eagerly (Ruby stdlib C extension).'
puts '    • Nxs::Reader reads only preamble + schema + tail-index on open (~microseconds).'
puts '    • .record(k) is O(1) — one 10-byte tail-index read.'
puts '    • sum_f64 is an allocation-free tight loop over the bitmask + 8-byte value.'
puts '    • CSV is included as a second baseline; all times are per-iteration averages.'
puts
