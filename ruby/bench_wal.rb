#!/usr/bin/env ruby
# WAL append benchmark — NXS Writer vs JSON (Ruby).
# Measures span-append throughput for the canonical 10-field SpanSchema.

require 'json'
require_relative 'nxs_writer'

SPAN_KEYS = %w[
  trace_id_hi trace_id_lo span_id parent_span_id
  name service start_time_ns duration_ns status_code payload
].freeze

SERVICES = %w[
  gateway auth-svc session-svc catalogue-svc recommend-svc
  inventory-svc payment-svc notify-svc search-svc cdn-edge
  analytics-svc feature-flags config-svc vector-db
].freeze

OPS = %w[
  http.server http.client grpc.server grpc.unary
  db.select db.insert db.update db.index_scan db.ann_search
  cache.get cache.set cache.miss
  pubsub.publish pubsub.consume
  llm.inference llm.embed
  jwt.verify auth.token_exchange
  queue.send queue.receive
].freeze

OP_DUR_BASE = [
  12_000_000, 11_000_000, 2_100_000, 1_900_000,
  4_200_000,  5_800_000,  4_600_000, 8_100_000, 14_500_000,
  310_000,    290_000,    350_000,
  820_000,    790_000,
  1_800_000_000, 220_000_000,
  590_000,    1_200_000,
  1_480_000,  1_510_000,
].freeze

PAYLOADS = [
  '{"model":"gpt-4o-mini","prompt_tokens":418,"completion_tokens":91,"total_tokens":509,"finish_reason":"stop"}',
  '{"model":"text-embedding-3-small","prompt_tokens":256,"top_k":20,"reranked":8,"latency_to_first_token_ms":19}',
  '{"attempt":1,"provider":"stripe","error":"upstream_timeout","http_status":504}',
  '{"attempt":2,"provider":"adyen","transaction_id":"txn_9f3a21c8","http_status":200}',
  '{"query_plan":"index_scan","rows_examined":18420,"rows_returned":124,"execution_ms":7.3}',
  '{"cache_key":"sess:usr_0x3f8a","ttl_remaining_s":1740,"hit":true,"bytes":892}',
  '{"topic":"order.confirmed","partition":3,"offset":8847219,"ack_ms":0.8}',
].freeze

START_NS = 1_715_018_000_000_000_000

def span_dur_ns(op_idx, i)
  base   = OP_DUR_BASE[op_idx]
  h      = (i * 2_654_435_761) & 0xFFFFFFFF
  jitter = h % (base * 0.8).to_i
  (base + jitter - base * 0.4).to_i
end

def span_status(i)
  h = (i * 2_246_822_519) & 0xFFFFFFFF
  return 1 if h < 0x07AE147A
  return 2 if h < 0x0A3D70A4
  0
end

def span_payload(op_idx, i)
  is_llm = op_idx == 14 || op_idx == 15
  is_pay = op_idx == 1 && i % 7 == 0
  h      = (i * 1_664_525 + 1_013_904_223) & 0xFFFFFFFF
  return PAYLOADS[i % PAYLOADS.size] if is_llm || is_pay || h < 0x26666666
  nil
end

def make_span(i)
  op_idx  = i % OPS.size
  payload = span_payload(op_idx, i)
  sp = {
    "trace_id_hi"    => i * 1_000_003,
    "trace_id_lo"    => -(i * 999_983 + 1),
    "span_id"        => i + 1,
    "parent_span_id" => i % 8 == 0 ? 0 : (i - 1),
    "name"           => OPS[op_idx],
    "service"        => SERVICES[i % SERVICES.size],
    "start_time_ns"  => START_NS + i * 1_000_000,
    "duration_ns"    => span_dur_ns(op_idx, i),
    "status_code"    => span_status(i),
  }
  sp["payload"] = payload if payload
  sp
end

def bench_nxs_wal(n)
  schema = Nxs::Schema.new(SPAN_KEYS)
  w      = Nxs::Writer.new(schema)
  spans  = n.times.map { |i| make_span(i) }
  t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  spans.each do |sp|
    w.begin_object
    w.write_i64(0, sp["trace_id_hi"])
    w.write_i64(1, sp["trace_id_lo"])
    w.write_i64(2, sp["span_id"])
    w.write_i64(3, sp["parent_span_id"])
    w.write_str(4, sp["name"])
    w.write_str(5, sp["service"])
    w.write_i64(6, sp["start_time_ns"])
    w.write_i64(7, sp["duration_ns"])
    w.write_i64(8, sp["status_code"])
    w.write_str(9, sp["payload"]) if sp["payload"]
    w.end_object
  end
  elapsed = Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0
  elapsed / n * 1e9
end

def bench_json(n)
  spans = n.times.map { |i| make_span(i) }
  t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  spans.each { |sp| sp.to_json }
  elapsed = Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0
  elapsed / n * 1e9
end

def fmt_ns(ns)
  if ns < 1_000
    "#{ns.round} ns"
  elsif ns < 1_000_000
    "#{"%.1f" % (ns / 1_000)} µs"
  else
    "#{"%.2f" % (ns / 1_000_000)} ms"
  end
end

# Try loading C extension
have_c = begin
  $LOAD_PATH.unshift File.join(__dir__, "ext/nxs")
  require 'nxs_ext'
  true
rescue LoadError
  false
end

def bench_nxs_wal_c(n)
  schema = Nxs::CSchema.new(SPAN_KEYS)
  spans  = n.times.map { |i| make_span(i) }
  w = Nxs::CWriter.new(schema)
  t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  spans.each do |sp|
    w.reset
    w.begin_object
    w.write_i64(0, sp["trace_id_hi"])
    w.write_i64(1, sp["trace_id_lo"])
    w.write_i64(2, sp["span_id"])
    w.write_i64(3, sp["parent_span_id"])
    w.write_str(4, sp["name"])
    w.write_str(5, sp["service"])
    w.write_i64(6, sp["start_time_ns"])
    w.write_i64(7, sp["duration_ns"])
    w.write_i64(8, sp["status_code"])
    w.write_str(9, sp["payload"]) if sp["payload"]
    w.end_object
    w.data_sector
  end
  elapsed = Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0
  elapsed / n * 1e9
end

def fmt_result(label, ns_best, json_best)
  kps = (1e9 / ns_best / 1000).round
  puts "  #{label.ljust(16)} #{fmt_ns(ns_best).rjust(10)}  (#{kps} k spans/s)"
  if json_best >= ns_best
    puts "  #{"".ljust(16)} #{"%.2fx faster than JSON" % (json_best / ns_best)}"
  else
    puts "  #{"".ljust(16)} #{"%.2fx slower than JSON" % (ns_best / json_best)}"
  end
end

counts = ARGV.empty? ? [1_000, 10_000, 100_000] : ARGV.map(&:to_i)
puts "WAL append benchmark — Ruby"
counts.each do |n|
  label = n.to_s.reverse.gsub(/(\d{3})(?=\d)/, '\1,').reverse
  puts "\n  n = #{label}"

  bench_nxs_wal([n, 1000].min)
  bench_json([n, 1000].min)
  bench_nxs_wal_c([n, 1000].min) if have_c

  nxs_best  = 3.times.map { bench_nxs_wal(n) }.min
  json_best = 3.times.map { bench_json(n) }.min

  puts "  JSON             #{fmt_ns(json_best).rjust(10)}  (#{(1e9/json_best/1000).round} k spans/s)"
  fmt_result("NXS WAL (pure)", nxs_best, json_best)

  if have_c
    c_best = 3.times.map { bench_nxs_wal_c(n) }.min
    fmt_result("NXS WAL (C ext)", c_best, json_best)
  end
end
puts
