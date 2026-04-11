"""Busy-loop rate limiter for Python mocks.

Uses clock_gettime(CLOCK_MONOTONIC_RAW) via _clock to achieve the
lowest jitter the Python interpreter can provide. time.sleep() is
deliberately avoided — at 100 kHz the period is 10 us, well below
the typical Linux tick granularity.

Sustained rate ceiling on modern CPUs is roughly 200 kHz per D-7;
for anything higher these mocks should be replaced with C.
"""

from _clock import monotonic_raw_ns


class RateLimiter:
    """Schedule one action per (1/hz) second using a busy-spin wait."""

    __slots__ = ("_period_ns", "_next_ns")

    def __init__(self, hz: float) -> None:
        if hz <= 0:
            raise ValueError("hz must be positive")
        self._period_ns = int(1_000_000_000 / hz)
        # 0 means "not yet initialized"; first wait() picks up 'now'.
        self._next_ns = 0

    def wait(self) -> None:
        """Block until the next scheduled slot, then advance the cursor."""
        now = monotonic_raw_ns()
        if self._next_ns == 0:
            self._next_ns = now
        # Busy-spin — time.sleep is too coarse for sub-ms slots.
        while now < self._next_ns:
            now = monotonic_raw_ns()
        self._next_ns += self._period_ns
        # If we've fallen > 1 period behind, drop queued slots rather
        # than emit a catch-up burst that would corrupt the rate curve.
        if self._next_ns < now - self._period_ns:
            self._next_ns = now + self._period_ns
