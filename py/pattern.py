"""Access pattern detector (Adaptive-prefetch-spec §4)."""
from __future__ import annotations

SEQUENTIAL_THRESHOLD = 10
RANDOM_THRESHOLD = 100
HISTORY_SIZE = 32
MIN_OBSERVATIONS = 8
UPGRADE_SEQUENTIAL_THRESHOLD = 100

PATTERN_UNKNOWN = "unknown"
PATTERN_SEQUENTIAL = "sequential"
PATTERN_RANDOM = "random"
PATTERN_MIXED = "mixed"


class AccessPatternDetector:
    """Observes record(index) calls and classifies access patterns."""

    __slots__ = (
        "accesses",
        "write_pos",
        "filled",
        "_sequential_runs",
        "_random_jumps",
        "last_index",
    )

    def __init__(self) -> None:
        self.accesses: list[int] = [-1] * HISTORY_SIZE
        self.write_pos = 0
        self.filled = 0
        self._sequential_runs = 0
        self._random_jumps = 0
        self.last_index = -1

    def sequential_runs(self) -> int:
        return self._sequential_runs

    def observe(self, index: int) -> None:
        idx = index
        if self.last_index >= 0:
            delta = abs(idx - self.last_index)
            if delta <= SEQUENTIAL_THRESHOLD:
                self._sequential_runs = min(self._sequential_runs + 1, 0xFFFFFFFF)
            elif delta > RANDOM_THRESHOLD:
                self._random_jumps = min(self._random_jumps + 1, 0xFFFFFFFF)
        self.accesses[self.write_pos] = idx
        self.write_pos = (self.write_pos + 1) % HISTORY_SIZE
        if self.filled < HISTORY_SIZE:
            self.filled += 1
        self.last_index = idx

    def pattern(self) -> str:
        total = self._sequential_runs + self._random_jumps
        if total < MIN_OBSERVATIONS:
            return PATTERN_UNKNOWN
        if self._sequential_runs > self._random_jumps * 3:
            return PATTERN_SEQUENTIAL
        if self._random_jumps > self._sequential_runs:
            return PATTERN_RANDOM
        return PATTERN_MIXED

    def predict_next(self, depth: int, record_count: int) -> list[int]:
        """Predicted next record indices when pattern is sequential (§4.4)."""
        if self.pattern() != PATTERN_SEQUENTIAL or self.last_index < 0:
            return []
        start = self.last_index + 1
        out: list[int] = []
        for i in range(depth):
            idx = start + i
            if idx < record_count:
                out.append(idx)
        return out
