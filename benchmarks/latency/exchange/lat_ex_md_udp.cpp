/// @file lat_ex_md_udp.cpp
/// Phase 10 latency benchmark: one-way UDP Mold64-style market-data push
/// against the Python mock `benchmarks/latency/mocks/ex_md_udp_push.py`.
///
/// Sub-phase 10.5. Per
/// `.artifacts/plan-phase-10-latency-bench-20260411-040540.md` §Sub-phase
/// 10.5 and §Interface design → "Client API pattern" (one-way variant).
///
/// Semantics:
///
///   * Client binds `KernelUdpSocket<RawDatagramCodec>` on
///     `client_ip:port` (or 127.0.0.1:port for loopback smoke) BEFORE the
///     mock starts pushing. The `lat` wrapper forks the mock after a
///     500 ms sleep; starting the client first (via the wrapper's
///     sequence) avoids "destination unreachable" bursts at startup.
///   * The mock (ex_md_udp_push.py) is unilateral: it never receives, it
///     just sendto()s Mold64 datagrams at `push_rate_hz` for
///     `duration_seconds`. The client is purely passive — it only polls.
///   * On each datagram the client stamps `t_recv = monotonic_raw_ns()`
///     immediately, parses the 20-byte Mold64 header, then walks the
///     inner 25-byte messages and records `t_recv - server_send_ns`
///     per inner message into the Recorder (subject to warmup gate).
///
/// Parsing decision (plan D-5 — no eph-json, but that doesn't apply
/// here; the question is Mold64Codec vs RawDatagramCodec):
///
///   We use `RawDatagramCodec` and hand-parse the packet. `Mold64Codec`
///   expects the stock Nasdaq ITCH Mold64 framing where each inner
///   message has its own 2-byte length prefix — the bench mock's
///   `benchmarks/latency/mocks/ex_md_udp_push.py` does NOT emit a
///   length prefix (each inner message is fixed 25 B and the count
///   lives in the Mold64 header), so feeding the mock's bytes to
///   Mold64Codec would be a mismatch. Hand-parsing keeps the client
///   coupled to the mock format verbatim (20 B header + N × 25 B inner
///   trade messages) with no codec impedance.
///
/// Packet layout (from ex_md_udp_push.py, cross-checked byte-for-byte):
///
///     Mold64 header (20 B):
///       [0:10]   session_id  = "EPHBENCH\0\0"
///       [10:18]  seq         : uint64 BE  (we don't use this)
///       [18:20]  msg_count   : uint16 BE
///     Inner trade (25 B, repeated `msg_count` times):
///       [0:1]    msg_type    = 'T' (0x54)
///       [1:9]    server_send : uint64 LE  (CLOCK_MONOTONIC_RAW ns)
///       [9:17]   symbol      : 8 ASCII
///       [17:25]  price       : uint64 LE  (unused)
///
/// Measurement clock: both ends use CLOCK_MONOTONIC_RAW via vDSO, so on
/// a single host the timestamps share a base without TSC calibration.
/// See plan D-6 for the rationale.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

// eph-* headers (plan D-4: v3.3 API only).
#include "eph/codec/raw_datagram_codec.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/utils/recorder.hpp"

#if defined(EPH_USE_DPDK)
#  include "eph/net/dpdk/poller.hpp"
#  include "eph/net/dpdk/udp_socket.hpp"
#else
#  include "eph/net/kernel/config.hpp"
#  include "eph/net/kernel/poller.hpp"
#  include "eph/net/kernel/udp_socket.hpp"
#endif

#include "core/config.hpp"
#include "core/measurement.hpp"

namespace {

namespace ec = eph::codec;
namespace eu = eph::utils;
namespace en = eph::net;

#if defined(EPH_USE_DPDK)
namespace ed = eph::net::dpdk;
using DpdkSocket = ed::DpdkUdpSocket<ec::RawDatagramCodec>;
using DpdkPoller = ed::DpdkPoller<>;
static_assert(sizeof(DpdkSocket*) > 0);
static_assert(sizeof(DpdkPoller*) > 0);
#else
namespace ek = eph::net::kernel;
using Socket = ek::KernelUdpSocket<ec::RawDatagramCodec>;
#endif

// ── Packet layout constants (must match ex_md_udp_push.py) ───────────
constexpr std::size_t kMold64HeaderSize = 20;
constexpr std::size_t kInnerMsgSize     = 25;
constexpr uint8_t     kMsgTypeTrade     = 'T';
constexpr std::size_t kMsgCountOffset   = 18;
constexpr std::size_t kServerTsOffset   = 1;  // within an inner message

constexpr const char* kDefaultConfigPath = "benchmarks/latency/bench.conf";

[[nodiscard, maybe_unused]] const char* parse_config_path(int argc, char** argv) noexcept {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0) {
            return argv[i + 1];
        }
    }
    if (const char* env = std::getenv("BENCH_CONFIG"); env && *env) {
        return env;
    }
    return kDefaultConfigPath;
}

/// Read a big-endian uint16 from `p` (no bounds check — caller guarantees
/// ≥ 2 bytes available).
[[nodiscard]] inline uint16_t load_be_u16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) |
                                  static_cast<uint16_t>(p[1]));
}

/// Read a little-endian uint64 from `p` via byte-wise memcpy. This is
/// correct on both LE and BE hosts (the mock always writes LE via
/// Python's `struct.pack('<Q', ...)`). On x86_64/aarch64 LE the compiler
/// lowers this to a single mov; on a BE host it would become a bswap.
[[nodiscard]] inline uint64_t load_le_u64(const uint8_t* p) noexcept {
    uint64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    v = __builtin_bswap64(v);
#endif
    return v;
}

} // namespace

#if defined(EPH_USE_DPDK)
int main(int /*argc*/, char** /*argv*/) {
    spdlog::set_level(spdlog::level::info);
    std::printf("lat_ex_md_udp_dpdk: v3.3 DpdkUdpSocket<RawDatagramCodec> API compiled.\n"
                "Real DPDK measurement loop is deferred to a follow-up phase "
                "(EAL init + vfio-pci NIC plumbing). Use lat_ex_md_udp (kernel) "
                "for live measurements.\n");
    return 0;
}
#else
int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::info);

    const char* conf_path = parse_config_path(argc, argv);

    auto globals_r = bench::ScenarioConfig::load_globals(conf_path);
    if (!globals_r) {
        std::fprintf(stderr, "lat_ex_md_udp: %s\n", globals_r.error().c_str());
        return 1;
    }
    auto scenario_r = bench::ScenarioConfig::load(conf_path, "lat_ex_md_udp");
    if (!scenario_r) {
        std::fprintf(stderr, "lat_ex_md_udp: %s\n", scenario_r.error().c_str());
        return 1;
    }
    const auto& globals  = globals_r.value();
    const auto& scenario = scenario_r.value();

    auto port_r = scenario.get_u32("port");
    if (!port_r) {
        std::fprintf(stderr, "lat_ex_md_udp: %s\n", port_r.error().c_str());
        return 1;
    }
    const uint16_t port = static_cast<uint16_t>(port_r.value());

    auto duration_r = scenario.get_u32("duration_seconds", 30);
    if (!duration_r) {
        std::fprintf(stderr, "lat_ex_md_udp: %s\n", duration_r.error().c_str());
        return 1;
    }
    const uint64_t duration_s = duration_r.value();

    // push_rate_hz and msg_per_packet are informational for the client;
    // the mock consults them to pace its sender loop. We print them in
    // the banner so bench logs are self-describing.
    const uint32_t push_rate_hz =
        scenario.get_u32("push_rate_hz", 100000).value_or(100000);
    const uint32_t msg_per_packet =
        scenario.get_u32("msg_per_packet", 5).value_or(5);

    auto warmup_r = globals.get_u64("warmup_samples", 1000);
    if (!warmup_r) {
        std::fprintf(stderr, "lat_ex_md_udp: %s\n", warmup_r.error().c_str());
        return 1;
    }
    const uint64_t warmup_samples = warmup_r.value();

    // The client BINDS on client_ip:port — the mock sends TO that tuple.
    // Loopback smoke uses 127.0.0.1; real NIC-B runs use the bench.conf
    // client_ip which is the routable address on the client-side NIC.
    const std::string bind_ip_str = globals.get_string("client_ip", "127.0.0.1");
    auto bind_ip_r = en::Ipv4Addr::parse(bind_ip_str);
    if (!bind_ip_r) {
        std::fprintf(stderr, "lat_ex_md_udp: invalid client_ip '%s': %s\n",
                     bind_ip_str.c_str(), bind_ip_r.error().detail);
        return 1;
    }

    std::printf("=== lat_ex_md_udp ===\n");
    std::printf("config: bind=%s:%u push_rate_hz=%u msg_per_packet=%u "
                "duration=%llus warmup_samples=%llu\n",
                bind_ip_str.c_str(),
                static_cast<unsigned>(port),
                static_cast<unsigned>(push_rate_hz),
                static_cast<unsigned>(msg_per_packet),
                static_cast<unsigned long long>(duration_s),
                static_cast<unsigned long long>(warmup_samples));
    std::fflush(stdout);

    bench::install_signal_handler();

    // Construct the Recorder BEFORE Socket::create so that TSC::init's
    // calibration spin runs at startup, not while the mock is already
    // pushing frames. Same 10.4 lesson applied to all 10.5 scenarios.
    eu::Recorder rec{"lat_ex_md_udp"};

    auto poller_r = ek::KernelPoller::create({});
    if (!poller_r) {
        std::fprintf(stderr, "lat_ex_md_udp: Poller::create failed: %s\n",
                     poller_r.error().detail);
        return 1;
    }
    auto poller = std::move(poller_r.value());

    ek::UdpConfig sock_cfg{};
    sock_cfg.bind = en::SocketAddr{bind_ip_r.value(), port};
    // Generous RX buffer — at 100 kHz × 5 msgs × 145 B ≈ 72 MB/s the
    // kernel default (~200 KB) would drop packets during the brief
    // window between poller->add() and the first poll(). 4 MiB matches
    // the mock's SO_SNDBUF for symmetry.
    sock_cfg.rcv_buf   = 4 * 1024 * 1024;
    sock_cfg.reuse_addr = true;

    auto sock_r = Socket::create(sock_cfg);
    if (!sock_r) {
        std::fprintf(stderr, "lat_ex_md_udp: Socket::create failed: %s\n",
                     sock_r.error().detail);
        return 2;
    }
    auto sock = std::move(sock_r.value());

    // ── Per-datagram parse + per-inner-message record ───────────────────
    //
    // Captures by reference — all locals live in main() and outlive the
    // poller loop. `rec`, `sample_idx`, and the two bad-data counters
    // are the only mutable state touched here.
    uint64_t sample_idx       = 0;  // per-inner-message counter
    uint64_t malformed_count  = 0;
    uint64_t clock_skew_count = 0;

    sock->on_datagram = [&](const uint8_t* d, uint16_t n,
                            const en::SocketAddr& /*src*/) {
        const uint64_t t_recv = bench::monotonic_raw_ns();

        if (n < kMold64HeaderSize) {
            ++malformed_count;
            return;
        }

        // Header: skip session_id + seq — we only need msg_count.
        const uint16_t msg_count = load_be_u16(d + kMsgCountOffset);
        const std::size_t expected =
            kMold64HeaderSize + static_cast<std::size_t>(msg_count) * kInnerMsgSize;
        if (static_cast<std::size_t>(n) < expected) {
            ++malformed_count;
            return;
        }

        for (uint16_t i = 0; i < msg_count; ++i) {
            const uint8_t* msg =
                d + kMold64HeaderSize + static_cast<std::size_t>(i) * kInnerMsgSize;
            if (msg[0] != kMsgTypeTrade) {
                ++malformed_count;
                continue;
            }
            const uint64_t t_server = load_le_u64(msg + kServerTsOffset);

            // Sanity gate: t_recv must be strictly after t_server or we
            // have a clock skew / instrumentation bug. Drop rather than
            // poison the histogram.
            if (t_recv <= t_server) {
                ++clock_skew_count;
                continue;
            }

            if (sample_idx >= warmup_samples) {
                rec.record_ns(t_recv - t_server);
            }
            ++sample_idx;
        }
    };

    if (auto r = poller->add(sock.get()); !r) {
        std::fprintf(stderr, "lat_ex_md_udp: poller->add failed: %s\n",
                     r.error().detail);
        return 3;
    }

    // ── Poll loop ───────────────────────────────────────────────────────
    //
    // The client is passive: only polls. The mock drives the rate for
    // `duration_seconds`. We keep polling for `duration_s + 2` so a slow
    // startup (mock waits on RateLimiter calibration, etc.) does not
    // truncate the end of the measurement window. Early exit on SIGINT.
    const uint64_t t_start = bench::monotonic_raw_ns();
    const uint64_t t_deadline =
        t_start + (duration_s + 2) * 1'000'000'000ull;

    while (bench::monotonic_raw_ns() < t_deadline && !bench::shutdown_requested()) {
        poller->poll();
    }

    if (malformed_count > 0 || clock_skew_count > 0) {
        std::fprintf(stderr,
                     "lat_ex_md_udp: skipped %llu malformed + %llu clock-skew "
                     "samples\n",
                     static_cast<unsigned long long>(malformed_count),
                     static_cast<unsigned long long>(clock_skew_count));
    }

    const char* backend =
#if defined(EPH_USE_DPDK)
        "dpdk";
#else
        "kernel";
#endif
    bench::print_report("lat_ex_md_udp", backend, rec, warmup_samples);

    (void)poller->remove(sock.get());
    sock.reset();
    poller.reset();

    return 0;
}
#endif // EPH_USE_DPDK
