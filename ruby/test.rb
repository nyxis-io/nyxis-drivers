# frozen_string_literal: true

# NXS parity tests — verifies the Ruby reader against the 1000-record JSON fixture.
#
# Usage: ruby ruby/test.rb [fixtures_dir]
#   e.g. ruby ruby/test.rb js/fixtures

require 'json'
require_relative 'nxs'

PASS = "\e[32mPASS\e[0m"
FAIL = "\e[31mFAIL\e[0m"

def check(label, &blk)
  result = blk.call
  if result
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

fixture_dir = ARGV[0] || '../js/fixtures'
nxb_path    = File.join(fixture_dir, 'records_1000.nxb')
json_path   = File.join(fixture_dir, 'records_1000.json')

abort "Fixtures not found in #{fixture_dir}" unless File.exist?(nxb_path) && File.exist?(json_path)

buf = File.binread(nxb_path)
json_str = File.read(json_path, encoding: 'UTF-8')
reader  = Nxs::Reader.new(buf)
json    = JSON.parse(json_str)

passes = 0
fails  = 0

puts
puts 'NXS Ruby Reader — Parity Tests'
puts '━' * 60
puts "  Fixture: #{nxb_path}"
puts

[
  check('record_count == 1000') { reader.record_count == 1000 },
  check("keys includes 'username'") { reader.keys.include?('username') },
  check("keys includes 'score'")    { reader.keys.include?('score')    },
  check("keys includes 'active'")   { reader.keys.include?('active')   },
  check("keys includes 'email'")    { reader.keys.include?('email')    },

  check("record(42).get_str('username') == json[42]['username']") do
    reader.record(42).get_str('username') == json[42]['username']
  end,

  check("record(0).get_str('username') == json[0]['username']") do
    reader.record(0).get_str('username') == json[0]['username']
  end,

  check("record(999).get_str('username') == json[999]['username']") do
    reader.record(999).get_str('username') == json[999]['username']
  end,

  check("record(500).get_f64('score').round(6) == json[500]['score'].round(6)") do
    nxs_val  = reader.record(500).get_f64('score')
    json_val = json[500]['score'].to_f
    nxs_val.round(6) == json_val.round(6)
  end,

  check("record(42).get_f64('score') == json[42]['score']") do
    reader.record(42).get_f64('score').round(6) == json[42]['score'].to_f.round(6)
  end,

  check("record(999).get_bool('active') == json[999]['active']") do
    reader.record(999).get_bool('active') == json[999]['active']
  end,

  check("record(0).get_bool('active') == json[0]['active']") do
    reader.record(0).get_bool('active') == json[0]['active']
  end,

  check("record(1).get_bool('active') (spot-check)") do
    reader.record(1).get_bool('active') == json[1]['active']
  end,

  check("record(42).get_i64('id') == json[42]['id']") do
    reader.record(42).get_i64('id') == json[42]['id']
  end,

  check("record(999).get_i64('age') == json[999]['age']") do
    reader.record(999).get_i64('age') == json[999]['age']
  end,

  check("sum_f64('score').round(4) == json.sum{score}.round(4)") do
    nxs_sum  = reader.sum_f64('score')
    json_sum = json.sum { |r| r['score'].to_f }
    nxs_sum.round(4) == json_sum.round(4)
  end,

  check("sum_i64('id') == json.sum{id}") do
    nxs_sum  = reader.sum_i64('id')
    json_sum = json.sum { |r| r['id'].to_i }
    nxs_sum == json_sum
  end,

  check("min_f64('score') is a Float") do
    v = reader.min_f64('score')
    v.is_a?(Float)
  end,

  check("max_f64('score') >= min_f64('score')") do
    reader.max_f64('score') >= reader.min_f64('score')
  end,

  check('out-of-bounds record(-1) raises') do
    reader.record(-1)
    false
  rescue Nxs::NxsError => e
    e.code == 'ERR_OUT_OF_BOUNDS'
  end,

  check('out-of-bounds record(1000) raises') do
    reader.record(1000)
    false
  rescue Nxs::NxsError => e
    e.code == 'ERR_OUT_OF_BOUNDS'
  end,

  check('unknown key returns nil') do
    reader.record(0).get_str('__nonexistent__').nil?
  end
].each { |r| r ? (passes += 1) : (fails += 1) }

# ── Writer round-trip tests ─────────────────────────────────────────────────
require_relative 'nxs_writer'

puts
puts 'NXS Ruby Writer — Round-trip Tests'
puts '━' * 60
puts

[
  check('writer round-trip: 3 records') do
    schema = Nxs::Schema.new(%w[id username score active])
    w = Nxs::Writer.new(schema)
    recs = [[1, 'alice', 9.5, true], [2, 'bob', 7.2, false], [3, 'carol', 8.8, true]]
    recs.each do |(id, name, score, active)|
      w.begin_object
      w.write_i64(0, id)
      w.write_str(1, name)
      w.write_f64(2, score)
      w.write_bool(3, active)
      w.end_object
    end
    r = Nxs::Reader.new(w.finish)
    r.record_count == 3 &&
      r.record(0).get_i64('id') == 1 &&
      r.record(1).get_str('username') == 'bob' &&
      (r.record(2).get_f64('score') - 8.8).abs < 1e-9 &&
      r.record(0).get_bool('active') == true &&
      r.record(1).get_bool('active') == false
  end,

  check('writer from_records convenience') do
    data = Nxs::Writer.from_records(
      %w[id name value],
      [{ 'id' => 10, 'name' => 'foo', 'value' => 1.5 },
       { 'id' => 20, 'name' => 'bar', 'value' => 2.5 }]
    )
    r = Nxs::Reader.new(data)
    r.record_count == 2 && r.record(1).get_str('name') == 'bar'
  end,

  check('writer null field') do
    schema = Nxs::Schema.new(%w[a b])
    w = Nxs::Writer.new(schema)
    w.begin_object
    w.write_i64(0, 99)
    w.write_null(1)
    w.end_object
    r = Nxs::Reader.new(w.finish)
    r.record(0).get_i64('a') == 99
  end,

  check('writer bool fields') do
    schema = Nxs::Schema.new(['flag'])
    w = Nxs::Writer.new(schema)
    w.begin_object
    w.write_bool(0, true)
    w.end_object
    w.begin_object
    w.write_bool(0, false)
    w.end_object
    r = Nxs::Reader.new(w.finish)
    r.record(0).get_bool('flag') == true && r.record(1).get_bool('flag') == false
  end,

  check('writer unicode string') do
    schema = Nxs::Schema.new(['msg'])
    w = Nxs::Writer.new(schema)
    w.begin_object
    w.write_str(0, 'héllo wörld')
    w.end_object
    r = Nxs::Reader.new(w.finish)
    r.record(0).get_str('msg') == 'héllo wörld'
  end,

  check('writer many fields (>7, multi-byte bitmask)') do
    keys = (0..8).map { |i| "f#{i}" }
    schema = Nxs::Schema.new(keys)
    w = Nxs::Writer.new(schema)
    w.begin_object
    keys.each_with_index { |_, i| w.write_i64(i, i * 100) }
    w.end_object
    r = Nxs::Reader.new(w.finish)
    keys.each_with_index.all? { |k, i| r.record(0).get_i64(k) == i * 100 }
  end

].each { |r| r ? (passes += 1) : (fails += 1) }

# ── Query engine tests ──────────────────────────────────────────────────────
puts
puts 'NXS Ruby Query Engine — Tests'
puts '━' * 60
puts

[
  check('test_query_all_count: reader.all.count == 1000') do
    reader.all.count == 1000
  end,

  check('test_query_eq_bool: Eq("active", true) count matches JSON') do
    expected = json.count { |r| r['active'] == true }
    reader.where(Nxs::Eq.new('active', true)).count == expected
  end,

  check('test_query_gt_float: Gt("score", 80.0) count matches JSON') do
    expected = json.count { |r| r['score'].to_f > 80.0 }
    reader.where(Nxs::Gt.new('score', 80.0)).count == expected
  end,

  check('test_query_and: Eq("active",true) & Gt("score",80.0) matches JSON') do
    pred     = Nxs::Eq.new('active', true) & Nxs::Gt.new('score', 80.0)
    expected = json.count { |r| r['active'] == true && r['score'].to_f > 80.0 }
    reader.where(pred).count == expected
  end,

  check('test_query_first: first active record matches JSON first active') do
    first_obj = reader.where(Nxs::Eq.new('active', true)).first
    expected_idx = json.index { |r| r['active'] == true }
    first_obj&.get_str('username') == json[expected_idx]['username']
  end,

  check('test_query_or: Gt("score",95.0) | Lt("score",10.0) count matches JSON') do
    pred     = Nxs::Gt.new('score', 95.0) | Nxs::Lt.new('score', 10.0)
    expected = json.count { |r| r['score'].to_f > 95.0 || r['score'].to_f < 10.0 }
    reader.where(pred).count == expected
  end,

  check('test_query_not: ~Eq("active",true) count == inactive count') do
    pred     = ~Nxs::Eq.new('active', true)
    expected = json.count { |r| r['active'] != true }
    reader.where(pred).count == expected
  end,

  check('test_query_enumerable: map username via all') do
    names = reader.all.map { |obj| obj.get_str('username') }
    names.length == 1000 && names.first == json[0]['username']
  end

].each { |r| r ? (passes += 1) : (fails += 1) }

# ── Security tests ──────────────────────────────────────────────────────────
[
  check('bad magic raises ERR_BAD_MAGIC') do
    bad = buf.dup
    bad.setbyte(0, 0x00)
    begin Nxs::Reader.new(bad)
          false
    rescue Nxs::NxsError => e; e.code == 'ERR_BAD_MAGIC'
    end
  end,

  check('truncated file raises NxsError') do
    Nxs::Reader.new(buf[0, 16])
    false
  rescue Nxs::NxsError; true
  end,

  check('corrupt DictHash raises ERR_DICT_MISMATCH') do
    bad = buf.dup
    bad.setbyte(8, bad.getbyte(8) ^ 0xFF)
    begin Nxs::Reader.new(bad)
          false
    rescue Nxs::NxsError => e; e.code == 'ERR_DICT_MISMATCH'
    end
  end
].each { |r| r ? (passes += 1) : (fails += 1) }

puts
puts '━' * 60
puts "  Results: #{passes} passed, #{fails} failed"
puts

exit(fails.zero? ? 0 : 1)
