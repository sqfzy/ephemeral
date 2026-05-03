/// @file bench_helpers.hpp
/// Shared utilities for the eph-net-dpdk microbenchmarks.
///
/// Each bench used to carry its own copy of `fill_random` (byte-identical
/// across `bench_tcp_header.cpp`, `bench_udp.cpp`, `bench_multicast.cpp`).
/// Centralizing here keeps the seed-distribution-loop in one spot, so a
/// future tweak (e.g. switching to a faster PRNG, widening the byte
/// distribution to `uint8_t` directly when GCC stops the deprecation
/// noise) doesn't have to be repeated 3+ times.
///
/// Header-only by design — these helpers exist only inside the bench
/// binaries and never enter eph's public surface.
#pragma once

#include <cstddef>
#include <cstdint>
#include <random>

namespace eph::dpdk::bench {

/// Fill `[buf, buf + len)` with bytes derived from a deterministic
/// `mt19937(seed)` stream. Different seeds produce independent
/// pseudo-random streams useful for ensuring inputs across iterations
/// of a benchmark hit different mempool / cacheline patterns.
///
/// `uniform_int_distribution<uint16_t>` is used (not `<uint8_t>`)
/// because `<uint8_t>` is officially "implementation-defined" per the
/// C++ standard and libstdc++ emits a deprecation warning. The
/// `uint16_t` form is portable and we down-cast the result to `uint8_t`.
inline void fill_random(uint8_t* buf, std::size_t len, uint32_t seed = 42) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<uint16_t> dist(0, 255);
    for (std::size_t i = 0; i < len; ++i) {
        buf[i] = static_cast<uint8_t>(dist(rng));
    }
}

} // namespace eph::dpdk::bench
