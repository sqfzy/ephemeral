"""clock_gettime(CLOCK_MONOTONIC_RAW) via ctypes.

Returns nanoseconds as int. Matches bench::monotonic_raw_ns() in C++.
This keeps Python mock timestamps in the same clock domain as the
C++ bench client, which is what allows server-stamped ``T`` fields
to be compared against ``TSC`` -> ``monotonic_raw`` conversions on
the client side.
"""

import ctypes

CLOCK_MONOTONIC_RAW = 4


class _timespec(ctypes.Structure):
    _fields_ = [("tv_sec", ctypes.c_long), ("tv_nsec", ctypes.c_long)]


_libc = ctypes.CDLL("libc.so.6", use_errno=True)
_libc.clock_gettime.argtypes = [ctypes.c_int, ctypes.POINTER(_timespec)]
_libc.clock_gettime.restype = ctypes.c_int


def monotonic_raw_ns() -> int:
    """Return CLOCK_MONOTONIC_RAW in nanoseconds as a Python int."""
    ts = _timespec()
    if _libc.clock_gettime(CLOCK_MONOTONIC_RAW, ctypes.byref(ts)) != 0:
        errno = ctypes.get_errno()
        raise OSError(errno, "clock_gettime failed: errno=%d" % errno)
    return ts.tv_sec * 1_000_000_000 + ts.tv_nsec
