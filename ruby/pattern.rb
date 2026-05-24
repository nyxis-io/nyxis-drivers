# frozen_string_literal: true

# Access pattern detector (Adaptive-prefetch-spec §4).

module Nxs
  SEQUENTIAL_THRESHOLD = 10
  RANDOM_THRESHOLD = 100
  HISTORY_SIZE = 32
  MIN_OBSERVATIONS = 8
  UPGRADE_SEQUENTIAL_THRESHOLD = 100

  PATTERN_UNKNOWN = 'unknown'
  PATTERN_SEQUENTIAL = 'sequential'
  PATTERN_RANDOM = 'random'
  PATTERN_MIXED = 'mixed'

  # Observes record(index) / seek calls and classifies access patterns.
  class AccessPatternDetector
    def initialize
      @accesses = Array.new(HISTORY_SIZE, -1)
      @write_pos = 0
      @filled = 0
      @sequential_runs = 0
      @random_jumps = 0
      @last_index = -1
    end

    attr_reader :sequential_runs, :last_index

    def observe(index)
      idx = index
      if @last_index >= 0
        delta = (idx - @last_index).abs
        if delta <= SEQUENTIAL_THRESHOLD
          @sequential_runs = [@sequential_runs + 1, 0xffffffff].min
        elsif delta > RANDOM_THRESHOLD
          @random_jumps = [@random_jumps + 1, 0xffffffff].min
        end
      end
      @accesses[@write_pos] = idx
      @write_pos = (@write_pos + 1) % HISTORY_SIZE
      @filled += 1 if @filled < HISTORY_SIZE
      @last_index = idx
    end

    def pattern
      total = @sequential_runs + @random_jumps
      return PATTERN_UNKNOWN if total < MIN_OBSERVATIONS

      return PATTERN_SEQUENTIAL if @sequential_runs > @random_jumps * 3
      return PATTERN_RANDOM if @random_jumps > @sequential_runs

      PATTERN_MIXED
    end

    # Predicted next record indices when pattern is sequential (§4.4).
    def predict_next(depth, record_count)
      return [] unless pattern == PATTERN_SEQUENTIAL && @last_index >= 0

      start = @last_index + 1
      out = []
      depth.times do |i|
        idx = start + i
        out << idx if idx < record_count
      end
      out
    end
  end
end
