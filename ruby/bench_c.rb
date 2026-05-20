# frozen_string_literal: true
# NXS C vs Pure-Ruby vs JSON benchmark.
#
# Compares Nxs::CReader (C extension) vs Nxs::Reader (pure Ruby) vs JSON.parse
# across 6 scenarios at n = 1000 / 10000 / 100000 / 1000000 records.
#
# Usage: ruby ruby/bench_c.rb [fixtures_dir]
#   e.g. ruby ruby/bench_c.rb js/fixtures

require "json"
require_relative "ext/nxs/nxs_ext"
require_relative "nxs"

# ── Helpers ──────────────────────────────────────────────────────────────────

def bench(iters)
  3.times { yield }   # warmup
  t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  iters.times { yield }
  (Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0) / iters
end

def fmt_time(s)
  return "#{(s * 1e9).round}  ns" if s < 1e-6
  return "#{"%.1f" % (s * 1e6)} µs" if s < 1e-3
  return "#{"%.2f" % (s * 1e3)} ms" if s < 1
  "#{"%.2f" % s}  s"
end

def fmt_bytes(n)
  return "#{n} B"             if n < 1024
  return "#{"%.1f" % (n / 1024.0)} KB" if n < 1024 * 1024
  "#{"%.2f" % (n / 1024.0 / 1024.0)} MB"
end

def row(label, avg, baseline)
  ratio = if avg == baseline
    "baseline"
  elsif avg < baseline
    "#{"%.1f" % (baseline / avg)}x faster"
  else
    "#{"%.1f" % (avg / baseline)}x slower"
  end
  printf "  │  %-48s %12s   %s\n", label, fmt_time(avg), ratio
end

def section(title)
  bar = "─" * [0, 76 - title.length].max
  puts "\n  ┌─ #{title} #{bar}┐"
end

def endsection
  puts "  └#{"─" * 79}┘"
end

# ── Parity checks ────────────────────────────────────────────────────────────

PASS = "\e[32mPASS\e[0m"
FAIL = "\e[31mFAIL\e[0m"

def check(label, &blk)
  result = blk.call
  status = result ? PASS : FAIL
  puts "  #{status}  #{label}"
  result
rescue => e
  puts "  #{FAIL}  #{label} — exception: #{e}"
  false
end

def run_parity(fixture_dir)
  nxb_path  = File.join(fixture_dir, "records_1000.nxb")
  json_path = File.join(fixture_dir, "records_1000.json")
  return unless File.exist?(nxb_path) && File.exist?(json_path)

  buf  = File.binread(nxb_path)
  json = JSON.parse(File.read(json_path, encoding: "UTF-8"))
  cr   = Nxs::CReader.new(buf)
  pr   = Nxs::Reader.new(buf)

  puts
  puts "Parity checks (n=1000, CReader vs JSON ground truth)"
  puts "━" * 60

  passes = fails = 0
  [
    check("record_count == 1000")            { cr.record_count == 1000 },
    check("keys includes 'username'")        { cr.keys.include?("username") },
    check("keys includes 'score'")           { cr.keys.include?("score")    },
    check("keys includes 'active'")          { cr.keys.include?("active")   },
    check("keys includes 'email'")           { cr.keys.include?("email")    },

    check("record(42).get_str('username') == json[42]['username']") {
      cr.record(42).get_str("username") == json[42]["username"]
    },
    check("record(0).get_str('username') == json[0]['username']") {
      cr.record(0).get_str("username") == json[0]["username"]
    },
    check("record(999).get_str('username') == json[999]['username']") {
      cr.record(999).get_str("username") == json[999]["username"]
    },

    check("record(500).get_f64('score').round(6) == json[500]['score'].round(6)") {
      cr.record(500).get_f64("score").round(6) == json[500]["score"].to_f.round(6)
    },
    check("record(42).get_f64('score').round(6) == json[42]['score'].round(6)") {
      cr.record(42).get_f64("score").round(6) == json[42]["score"].to_f.round(6)
    },

    check("record(999).get_bool('active') == json[999]['active']") {
      cr.record(999).get_bool("active") == json[999]["active"]
    },
    check("record(0).get_bool('active') == json[0]['active']") {
      cr.record(0).get_bool("active") == json[0]["active"]
    },

    check("record(42).get_i64('id') == json[42]['id']") {
      cr.record(42).get_i64("id") == json[42]["id"]
    },
    check("record(999).get_i64('age') == json[999]['age']") {
      cr.record(999).get_i64("age") == json[999]["age"]
    },

    check("sum_f64('score').round(4) == json.sum.round(4)") {
      nxs_sum  = cr.sum_f64("score")
      json_sum = json.sum { |r| r["score"].to_f }
      nxs_sum.round(4) == json_sum.round(4)
    },
    check("sum_f64 C == sum_f64 pure-Ruby") {
      cr.sum_f64("score").round(4) == pr.sum_f64("score").round(4)
    },
    check("sum_i64('id') == json.sum{id}") {
      cr.sum_i64("id") == json.sum { |r| r["id"].to_i }
    },

    check("min_f64('score') is a Float") {
      cr.min_f64("score").is_a?(Float)
    },
    check("max_f64('score') >= min_f64('score')") {
      cr.max_f64("score") >= cr.min_f64("score")
    },

    check("unknown key returns nil") {
      cr.record(0).get_str("__nonexistent__").nil?
    },

    check("out-of-bounds record(-1) raises IndexError") {
      begin; cr.record(-1); false
      rescue IndexError => e; e.message.include?("ERR_OUT_OF_BOUNDS")
      end
    },
    check("out-of-bounds record(1000) raises IndexError") {
      begin; cr.record(1000); false
      rescue IndexError => e; e.message.include?("ERR_OUT_OF_BOUNDS")
      end
    },
  ].each { |r| r ? (passes += 1) : (fails += 1) }

  puts "━" * 60
  puts "  Results: #{passes} passed, #{fails} failed"
  puts
end

# ── Per-size benchmark ───────────────────────────────────────────────────────

def run_scale(fixture_dir, n)
  nxb_path  = File.join(fixture_dir, "records_#{n}.nxb")
  json_path = File.join(fixture_dir, "records_#{n}.json")

  unless File.exist?(nxb_path) && File.exist?(json_path)
    puts "\n  ⚠  skipping n=#{n}: fixtures missing"
    return
  end

  nxb_buf  = File.binread(nxb_path)
  json_str = File.read(json_path, encoding: "UTF-8")

  puts "\n#{"━" * 38}  n = #{n.to_s.reverse.gsub(/(\d{3})(?=\d)/, '\\1,').reverse}  #{"━" * 36}"
  printf "  .nxb  size:  %10s\n", fmt_bytes(nxb_buf.bytesize)
  printf "  .json size:  %10s  (%.2fx NXS)\n", fmt_bytes(json_str.bytesize),
         json_str.bytesize.to_f / nxb_buf.bytesize

  # Iteration counts scaled to keep wall-clock reasonable
  if n >= 1_000_000
    parse_iters, rand_iters, iter_iters, cold_iters = 3, 5_000, 2, 3
  elsif n >= 100_000
    parse_iters, rand_iters, iter_iters, cold_iters = 20, 20_000, 5, 15
  elsif n >= 10_000
    parse_iters, rand_iters, iter_iters, cold_iters = 200, 50_000, 50, 100
  else
    parse_iters, rand_iters, iter_iters, cold_iters = 2_000, 100_000, 500, 500
  end

  rng     = Random.new(0)
  indices = Array.new(rand_iters) { rng.rand(n) }

  # ── 1. Open file ────────────────────────────────────────────────────────────
  section("Open file (parse / index full structure)")
  t_json_open = bench(parse_iters) { JSON.parse(json_str) }
  row("JSON.parse(entire document)",          t_json_open, t_json_open)

  t_ruby_open = bench(parse_iters) { Nxs::Reader.new(nxb_buf) }
  row("Nxs::Reader.new(buffer) [pure Ruby]",  t_ruby_open, t_json_open)

  t_c_open = bench(parse_iters) { Nxs::CReader.new(nxb_buf) }
  row("Nxs::CReader.new(buffer) [C ext]",     t_c_open, t_json_open)
  endsection

  # Pre-build warm readers
  parsed  = JSON.parse(json_str)
  creader = Nxs::CReader.new(nxb_buf)
  reader  = Nxs::Reader.new(nxb_buf)

  # ── 2. Warm random access ───────────────────────────────────────────────────
  section("Warm random-access read (1 field from 1 record)")
  ri = 0
  t_json_rand = bench(rand_iters) { parsed[indices[ri = (ri + 1) % rand_iters]]["username"] }
  row("parsed_json[k]['username']",              t_json_rand, t_json_rand)

  ri2 = 0
  t_ruby_rand = bench(rand_iters) { reader.record(indices[ri2 = (ri2 + 1) % rand_iters]).get_str("username") }
  row("Nxs::Reader.record(k).get_str [Ruby]",   t_ruby_rand, t_json_rand)

  ri3 = 0
  t_c_rand = bench(rand_iters) { creader.record(indices[ri3 = (ri3 + 1) % rand_iters]).get_str("username") }
  row("Nxs::CReader.record(k).get_str [C]",     t_c_rand, t_json_rand)
  endsection

  # ── 3. Cold start ───────────────────────────────────────────────────────────
  section("Cold start — open file + read 1 field")
  cold_k = n / 2
  t_json_cold = bench(cold_iters) { JSON.parse(json_str)[cold_k]["username"] }
  row("JSON.parse + arr[k]['username']",              t_json_cold, t_json_cold)

  t_ruby_cold = bench(cold_iters) { Nxs::Reader.new(nxb_buf).record(cold_k).get_str("username") }
  row("Nxs::Reader.new + record.get_str [Ruby]",     t_ruby_cold, t_json_cold)

  t_c_cold = bench(cold_iters) { Nxs::CReader.new(nxb_buf).record(cold_k).get_str("username") }
  row("Nxs::CReader.new + record.get_str [C]",       t_c_cold, t_json_cold)
  endsection

  # ── 4. Full scan per-record ──────────────────────────────────────────────────
  section("Full scan per-record — sum score via record(i).get_f64")
  t_json_scan = bench(iter_iters) {
    s = 0.0; parsed.each { |r| s += r["score"].to_f }; s
  }
  row("json: arr.each sum scores",                     t_json_scan, t_json_scan)

  t_ruby_scan = bench(iter_iters) {
    s = 0.0
    n.times { |i| v = reader.record(i).get_f64("score"); s += v if v }
    s
  }
  row("Nxs::Reader: n.times record.get_f64 [Ruby]",   t_ruby_scan, t_json_scan)

  t_c_scan = bench(iter_iters) {
    s = 0.0
    n.times { |i| v = creader.record(i).get_f64("score"); s += v if v }
    s
  }
  row("Nxs::CReader: n.times record.get_f64 [C]",     t_c_scan, t_json_scan)
  endsection

  # ── 5. Reducer ──────────────────────────────────────────────────────────────
  section("Reducer — sum_f64('score') tight C loop")
  t_json_red = bench(iter_iters) {
    s = 0.0; parsed.each { |r| s += r["score"].to_f }; s
  }
  row("JSON: arr.each sum (baseline)",             t_json_red, t_json_red)

  t_ruby_red = bench(iter_iters) { reader.sum_f64("score") }
  row("Nxs::Reader.sum_f64 [pure Ruby]",           t_ruby_red, t_json_red)

  t_c_red = bench(iter_iters) { creader.sum_f64("score") }
  row("Nxs::CReader.sum_f64 [C ext]",              t_c_red, t_json_red)

  speedup_vs_ruby = (t_ruby_red / t_c_red).round(1)
  speedup_vs_json = (t_json_red / t_c_red).round(1)
  printf "  │  C speedup vs pure-Ruby: %sx  vs JSON: %sx\n",
         speedup_vs_ruby, speedup_vs_json
  endsection

  # ── 6. Cold pipeline ────────────────────────────────────────────────────────
  section("Cold pipeline — open + sum_f64 from scratch")
  t_json_pipe = bench(cold_iters) {
    s = 0.0; JSON.parse(json_str).each { |r| s += r["score"].to_f }; s
  }
  row("JSON.parse + arr.each sum",                t_json_pipe, t_json_pipe)

  t_ruby_pipe = bench(cold_iters) { Nxs::Reader.new(nxb_buf).sum_f64("score") }
  row("Nxs::Reader.new + sum_f64 [pure Ruby]",    t_ruby_pipe, t_json_pipe)

  t_c_pipe = bench(cold_iters) { Nxs::CReader.new(nxb_buf).sum_f64("score") }
  row("Nxs::CReader.new + sum_f64 [C ext]",       t_c_pipe, t_json_pipe)
  endsection
end

# ── Main ─────────────────────────────────────────────────────────────────────

fixture_dir = ARGV[0] || "../js/fixtures"

puts
puts "╔══════════════════════════════════════════════════════════════════════════════════╗"
puts "║         NXS: C Extension vs Pure-Ruby vs JSON  —  Benchmark                     ║"
puts "╚══════════════════════════════════════════════════════════════════════════════════╝"
printf "\n  Ruby:     %s\n", RUBY_DESCRIPTION
printf "  Platform: %s\n",  RUBY_PLATFORM
printf "  Fixtures: %s\n",  fixture_dir

run_parity(fixture_dir)

[1_000, 10_000, 100_000, 1_000_000].each do |n|
  begin
    run_scale(fixture_dir, n)
  rescue => e
    puts "\n  ⚠  n=#{n} failed: #{e.message}"
    puts e.backtrace.first(3).map { |l| "       #{l}" }
  end
end

puts
puts "═" * 82
puts "  Notes:"
puts "    • Nxs::CReader / Nxs::CObject — MRI C extension, TypedData_Make_Struct."
puts "    • sum_f64 C loop: zero VALUE allocations per record (single rb_float_new at end)."
puts "    • LEB128 bitmask walk is inline C in scan_field_offset()."
puts "    • Cold pipeline = parse preamble + tail-index + C scan, no schema re-read."
puts
