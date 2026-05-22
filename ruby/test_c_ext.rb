# frozen_string_literal: true
# NXS C-extension correctness tests.
#
# Verifies Nxs::CReader (C extension) against the 1000-record JSON fixture.
# Called by `make test-ruby-ci` after `bash ruby/ext/build.sh`.
#
# Usage: ruby ruby/test_c_ext.rb [fixtures_dir]
#   e.g. ruby ruby/test_c_ext.rb js/fixtures

require 'json'
require_relative 'ext/nxs/nxs_ext'
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

buf     = File.binread(nxb_path)
json    = JSON.parse(File.read(json_path, encoding: 'UTF-8'))
creader = Nxs::CReader.new(buf)
preader = Nxs::Reader.new(buf)

passes = 0
fails  = 0

puts
puts 'NXS Ruby C-Extension — Correctness Tests'
puts '━' * 60
puts "  Fixture: #{nxb_path}"
puts

results = [
  check('record_count == 1000')             { creader.record_count == 1000 },
  check("keys includes 'username'")         { creader.keys.include?('username') },
  check("keys includes 'score'")            { creader.keys.include?('score') },
  check("keys includes 'active'")           { creader.keys.include?('active') },
  check("keys includes 'email'")            { creader.keys.include?('email') },

  check("record(42).get_str('username') matches JSON") do
    creader.record(42).get_str('username') == json[42]['username']
  end,

  check("record(0).get_str('username') matches JSON") do
    creader.record(0).get_str('username') == json[0]['username']
  end,

  check("record(999).get_str('username') matches JSON") do
    creader.record(999).get_str('username') == json[999]['username']
  end,

  check("record(500).get_f64('score').round(6) matches JSON") do
    creader.record(500).get_f64('score').round(6) == json[500]['score'].to_f.round(6)
  end,

  check("record(42).get_f64('score').round(6) matches JSON") do
    creader.record(42).get_f64('score').round(6) == json[42]['score'].to_f.round(6)
  end,

  check("record(999).get_bool('active') matches JSON") do
    creader.record(999).get_bool('active') == json[999]['active']
  end,

  check("record(0).get_bool('active') matches JSON") do
    creader.record(0).get_bool('active') == json[0]['active']
  end,

  check("record(42).get_i64('id') matches JSON") do
    creader.record(42).get_i64('id') == json[42]['id']
  end,

  check("record(999).get_i64('age') matches JSON") do
    creader.record(999).get_i64('age') == json[999]['age']
  end,

  check("sum_f64('score') C == pure-Ruby (4 dp)") do
    creader.sum_f64('score').round(4) == preader.sum_f64('score').round(4)
  end,

  check("sum_f64('score') matches JSON sum (4 dp)") do
    creader.sum_f64('score').round(4) == json.sum { |r| r['score'].to_f }.round(4)
  end,

  check("sum_i64('id') matches JSON sum") do
    creader.sum_i64('id') == json.sum { |r| r['id'].to_i }
  end,

  check("min_f64('score') is a Float") do
    creader.min_f64('score').is_a?(Float)
  end,

  check("max_f64('score') >= min_f64('score')") do
    creader.max_f64('score') >= creader.min_f64('score')
  end,

  check('unknown key returns nil') do
    creader.record(0).get_str('__nonexistent__').nil?
  end,

  check('out-of-bounds record(-1) raises IndexError') do
    creader.record(-1)
    false
  rescue IndexError => e
    e.message.include?('ERR_OUT_OF_BOUNDS')
  end,

  check('out-of-bounds record(1000) raises IndexError') do
    creader.record(1000)
    false
  rescue IndexError => e
    e.message.include?('ERR_OUT_OF_BOUNDS')
  end
]

results.each { |r| r ? (passes += 1) : (fails += 1) }

puts
puts '━' * 60
puts "  Results: #{passes} passed, #{fails} failed"
puts

exit(fails.zero? ? 0 : 1)
