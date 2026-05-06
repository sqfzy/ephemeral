/// @file bench_socket_addr.cpp
/// Microbenchmarks for `eph::net::Ipv4Addr::parse` and `Ipv4Addr::to_string`.
///
/// Coverage gap (loop-cleanup R169): `eph-net` has bench coverage for the
/// HTTP / HMAC / JWT / signed-request paths, but the `SocketAddr` /
/// `Ipv4Addr` parser was untested by any bench. The parser is a security
/// surface — it explicitly rejects CVE-2021-29921 leading-zero octets,
/// trailing garbage, and over-long octets — so any change that relaxes a
/// gate or adds extra logging on the reject branch should surface as a
/// timing change. Without an isolated bench, only the (cold) config
/// loaders in benchmarks/latency/ exercise the function and any
/// regression hides under steady_clock noise.
///
/// The parser is also called once per stream config in setups that
/// resolve hostnames externally and pass the resolved string through —
/// so the happy path matters too.
///
/// We bench:
///   - parse_happy:        valid "192.168.1.42" — full dot-decimal walk
///   - parse_localhost:    "127.0.0.1" — different digit count per octet
///   - parse_max:          "255.255.255.255" — every octet hits 3-digit
///                         range gate
///   - parse_reject_lz:    "192.168.001.42" — CVE-2021-29921 reject
///   - parse_reject_oob:   "192.168.256.42" — out-of-range octet
///   - parse_reject_short: "192.168.1" — missing-dot branch
///   - to_string:          render hot path

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <benchmark/benchmark.h>

#include "eph/net/socket_addr.hpp"

using eph::net::Ipv4Addr;

// ---------------------------------------------------------------------------
// parse — happy paths
// ---------------------------------------------------------------------------

static void BM_Ipv4ParseHappy(benchmark::State& state) {
    constexpr std::string_view s{"192.168.1.42"};
    for (auto _ : state) {
        auto r = Ipv4Addr::parse(s);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_Ipv4ParseHappy);

static void BM_Ipv4ParseLocalhost(benchmark::State& state) {
    constexpr std::string_view s{"127.0.0.1"};
    for (auto _ : state) {
        auto r = Ipv4Addr::parse(s);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_Ipv4ParseLocalhost);

static void BM_Ipv4ParseMax(benchmark::State& state) {
    // Every octet hits the 3-digit-loop ceiling and the >255 range gate.
    constexpr std::string_view s{"255.255.255.255"};
    for (auto _ : state) {
        auto r = Ipv4Addr::parse(s);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_Ipv4ParseMax);

// ---------------------------------------------------------------------------
// parse — reject paths (CVE-2021-29921 + bounds + format)
// ---------------------------------------------------------------------------

static void BM_Ipv4ParseRejectLeadingZero(benchmark::State& state) {
    // "001" octet — CVE-2021-29921 ambiguity. Pin the cost so a regression
    // that adds extra logging or moves the check off the [[unlikely]] path
    // is visible.
    constexpr std::string_view s{"192.168.001.42"};
    for (auto _ : state) {
        auto r = Ipv4Addr::parse(s);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_Ipv4ParseRejectLeadingZero);

static void BM_Ipv4ParseRejectOutOfRange(benchmark::State& state) {
    constexpr std::string_view s{"192.168.256.42"};
    for (auto _ : state) {
        auto r = Ipv4Addr::parse(s);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_Ipv4ParseRejectOutOfRange);

static void BM_Ipv4ParseRejectShort(benchmark::State& state) {
    constexpr std::string_view s{"192.168.1"};
    for (auto _ : state) {
        auto r = Ipv4Addr::parse(s);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_Ipv4ParseRejectShort);

static void BM_Ipv4ParseRejectEmpty(benchmark::State& state) {
    // Cheapest reject branch — pin so the early-out cost cannot creep up.
    constexpr std::string_view s{};
    for (auto _ : state) {
        auto r = Ipv4Addr::parse(s);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_Ipv4ParseRejectEmpty);

// ---------------------------------------------------------------------------
// to_string — rendering hot path. Allocates a std::string each call, so
// includes the small-string-optimisation cost. Pin so a future swap from
// snprintf to a hand-rolled formatter is visible.
// ---------------------------------------------------------------------------

static void BM_Ipv4ToString(benchmark::State& state) {
    Ipv4Addr ip{192, 168, 1, 42};
    for (auto _ : state) {
        auto s = ip.to_string();
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_Ipv4ToString);

BENCHMARK_MAIN();
