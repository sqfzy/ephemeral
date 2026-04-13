/// @file core/timestamp_proto.hpp
/// Latency-bench timestamp protocol.
///
/// A fixed 24-byte block carried at the front of every RTT-scenario
/// payload (lat_{tcp,udp,ws,ex_md_udp}). Three little-endian `uint64`s
/// record the clock-domain-shared CLOCK_MONOTONIC_RAW timestamps that
/// decompose a round-trip into TX / SRV / RX legs:
///
///   [0:8]    client_ns     — set by the client before send()
///   [8:16]   mock_recv_ns  — set by the Python mock on recvfrom/recv
///   [16:24]  mock_send_ns  — set by the Python mock just before send
///
/// The client writes bytes `[0:8]` and zeros `[8:24]` before send. The
/// mock overwrites bytes `[8:24]` (and MUST NOT touch `[24:]`). On
/// receive the client reads all three fields and computes:
///
///   rtt_ns = recv_ns        - client_ns
///   tx_ns  = mock_recv_ns   - client_ns
///   rx_ns  = recv_ns        - mock_send_ns
///
/// `tx + rx` is not exactly equal to `rtt` — the difference is the
/// mock's internal service time `(mock_send_ns - mock_recv_ns)`, which
/// we deliberately do not report. Sanity expectation: `tx + rx ≤ rtt * 1.1`.
///
/// Both ends read the same vDSO clock (`CLOCK_MONOTONIC_RAW`) via
/// `bench::monotonic_raw_ns()` on the C++ side and `_clock.monotonic_raw_ns`
/// (ctypes → libc.clock_gettime) on the Python side. On a single host
/// this gives the two sides a shared time base without TSC calibration.
///
/// Header-only, noexcept, zero allocation. Uses `std::memcpy` rather
/// than reinterpret_cast for strict-aliasing safety — the compiler
/// lowers both to a single `mov`/`ldp` on x86_64 and aarch64.

#pragma once

#include <cstdint>
#include <cstring>
#include <span>

namespace bench {

/// POD holding the three timestamps carried in the 24 B protocol block.
/// Declared `struct` (not `class`) and trivially copyable so
/// `std::memcpy` round-trips are well-defined.
struct TimestampBlock {
    uint64_t client_ns;     ///< T_client — set by client before send
    uint64_t mock_recv_ns;  ///< T_mock_recv — set by mock on recv
    uint64_t mock_send_ns;  ///< T_mock_send — set by mock before send
};
static_assert(sizeof(TimestampBlock) == 24,
              "TimestampBlock must be exactly 24 bytes (3x LE uint64)");
static_assert(alignof(TimestampBlock) <= 8);

/// Size of the wire-format timestamp block in bytes.
inline constexpr std::size_t kTimestampBlockSize = sizeof(TimestampBlock);

/// Stamp the client timestamp into the first 8 bytes of `buf` and zero
/// the following 16 bytes (mock-owned region).
///
/// Pre-condition: `buf.size() >= kTimestampBlockSize`. The caller is
/// expected to have already performed the payload-size sanity check at
/// scenario startup, so this function does not re-check.
inline void write_client_ts(std::span<uint8_t> buf,
                            uint64_t client_ns) noexcept {
    TimestampBlock b{client_ns, 0, 0};
    std::memcpy(buf.data(), &b, sizeof(b));
}

/// Read the 24 B protocol block from the start of `buf`.
///
/// Pre-condition: `buf.size() >= kTimestampBlockSize`.
[[nodiscard]] inline TimestampBlock
read_ts(std::span<const uint8_t> buf) noexcept {
    TimestampBlock b{};
    std::memcpy(&b, buf.data(), sizeof(b));
    return b;
}

/// Three-leg timing result: full RTT plus the client→mock (TX) and
/// mock→client (RX) wire times computed from a `TimestampBlock` and
/// the client-side recv-complete timestamp.
struct LegTimings {
    uint64_t rtt_ns;  ///< client_recv_ns - ts.client_ns
    uint64_t tx_ns;   ///< ts.mock_recv_ns - ts.client_ns
    uint64_t rx_ns;   ///< client_recv_ns - ts.mock_send_ns
};

/// Given a populated `TimestampBlock` from the echoed payload and the
/// client's recv-complete timestamp, compute the three leg durations.
///
/// All subtractions are unsigned — callers must ensure the clock is
/// monotonic (it is: both ends use `CLOCK_MONOTONIC_RAW`). A negative
/// leg would wrap around to a huge positive value and immediately show
/// up in the Recorder histogram, so wrap is not silent.
[[nodiscard]] inline LegTimings
compute_legs(const TimestampBlock& ts, uint64_t client_recv_ns) noexcept {
    return LegTimings{
        .rtt_ns = client_recv_ns - ts.client_ns,
        .tx_ns  = ts.mock_recv_ns - ts.client_ns,
        .rx_ns  = client_recv_ns - ts.mock_send_ns,
    };
}

} // namespace bench
