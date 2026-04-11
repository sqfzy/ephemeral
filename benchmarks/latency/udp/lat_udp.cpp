/// @file lat_udp.cpp
/// Phase 10 latency benchmark: raw UDP RTT against a kernel Python echo mock.
///
/// Sub-phase 10.3 rewrite. Same structure as lat_tcp.cpp but using
/// `KernelUdpSocket<RawDatagramCodec>`:
///   - bind a local unconnected UDP socket (0.0.0.0 ephemeral port)
///   - send_to(mock_ip, scenario.port) for each sample
///   - wait for the echoed datagram via poller->poll()
///   - record `t1 - t0` (ns, CLOCK_MONOTONIC_RAW) into the Recorder
///
/// The Python echo mock is `benchmarks/latency/mocks/udp_echo.py`, forked by
/// the `lat` wrapper script. This binary contains no mock / NIC plumbing.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/codec/raw_datagram_codec.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/utils/recorder.hpp"

#if defined(EPH_USE_DPDK)
// Phase 11.0: DpdkUdpSocket + DpdkPoller real measurement loop (see lat_tcp
// for the template). The DPDK poller uses 4-tuple routing, so the registered
// legacy.src/dst_port pair must match the mock's kernel socket 2-tuple.
// UDP echo mock (udp_echo.py) binds a listener on (mock_ip, port) and
// responds from the SAME 4-tuple (kernel recvfrom/sendto preserves peer),
// which our registered (client_ip:src, mock_ip:port) matches after the
// Poller's direction-symmetric swap check.
#  include "eph/net/dpdk/poller.hpp"
#  include "eph/net/dpdk/udp_socket.hpp"
#else
#  include "eph/net/kernel/config.hpp"
#  include "eph/net/kernel/poller.hpp"
#  include "eph/net/kernel/udp_socket.hpp"
#endif

#include "core/config.hpp"
#include "core/measurement.hpp"
#include "core/timestamp_proto.hpp"
#if defined(EPH_USE_DPDK)
#  include "core/dpdk_env.hpp"
#endif

namespace {

namespace ec = eph::codec;
namespace eu = eph::utils;
namespace en = eph::net;

#if defined(EPH_USE_DPDK)
namespace ed = eph::net::dpdk;
using Socket = ed::DpdkUdpSocket<ec::RawDatagramCodec>;
using Poller = ed::DpdkPoller<>;
#else
namespace ek = eph::net::kernel;
using Socket = ek::KernelUdpSocket<ec::RawDatagramCodec>;
using Poller = ek::KernelPoller;
#endif

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

} // namespace

int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::info);

    const char* conf_path = parse_config_path(argc, argv);

    auto globals_r = bench::ScenarioConfig::load_globals(conf_path);
    if (!globals_r) {
        std::fprintf(stderr, "lat_udp: %s\n", globals_r.error().c_str());
        return 1;
    }
    auto scenario_r = bench::ScenarioConfig::load(conf_path, "lat_udp");
    if (!scenario_r) {
        std::fprintf(stderr, "lat_udp: %s\n", scenario_r.error().c_str());
        return 1;
    }
    const auto& globals  = globals_r.value();
    const auto& scenario = scenario_r.value();

    auto port_r = scenario.get_u32("port");
    if (!port_r) {
        std::fprintf(stderr, "lat_udp: %s\n", port_r.error().c_str());
        return 1;
    }
    const uint16_t port = static_cast<uint16_t>(port_r.value());

    auto payload_r = scenario.get_u32("payload_size", 256);
    if (!payload_r) {
        std::fprintf(stderr, "lat_udp: %s\n", payload_r.error().c_str());
        return 1;
    }
    const std::size_t payload_size = payload_r.value();

    // Phase 11.1 D-7: 24 B timestamp-block header requirement.
    if (payload_size < bench::kTimestampBlockSize) {
        std::fprintf(stderr,
                     "lat_udp: payload_size=%zu < kTimestampBlockSize=%zu "
                     "(TX/RX leg protocol requires a 24 B header)\n",
                     payload_size, bench::kTimestampBlockSize);
        return 1;
    }

    auto duration_r = scenario.get_u32("duration_seconds", 10);
    if (!duration_r) {
        std::fprintf(stderr, "lat_udp: %s\n", duration_r.error().c_str());
        return 1;
    }
    const uint64_t duration_s = duration_r.value();

    auto warmup_r = globals.get_u64("warmup_samples", 1000);
    if (!warmup_r) {
        std::fprintf(stderr, "lat_udp: %s\n", warmup_r.error().c_str());
        return 1;
    }
    const uint64_t warmup_samples = warmup_r.value();

    const std::string mock_ip_str = globals.get_string("mock_ip", "127.0.0.1");
    auto ip_r = en::Ipv4Addr::parse(mock_ip_str);
    if (!ip_r) {
        std::fprintf(stderr, "lat_udp: invalid mock_ip '%s': %s\n",
                     mock_ip_str.c_str(), ip_r.error().detail);
        return 1;
    }

    // Optional: bind client to `client_ip` so packets egress the right
    // NIC when the host has multiple interfaces. Loopback smoke tests
    // leave this at 0.0.0.0 — the kernel will pick lo because mock_ip
    // is 127.0.0.1.
    const std::string client_ip_str = globals.get_string("client_ip", "0.0.0.0");
    auto client_ip_r = en::Ipv4Addr::parse(client_ip_str);
    if (!client_ip_r) {
        std::fprintf(stderr, "lat_udp: invalid client_ip '%s': %s\n",
                     client_ip_str.c_str(), client_ip_r.error().detail);
        return 1;
    }

    const en::SocketAddr remote{ip_r.value(), port};

    std::printf("=== lat_udp ===\n");
    std::printf("config: mock=%s client_bind=%s port=%u payload_size=%zu "
                "duration=%llus warmup_samples=%llu\n",
                mock_ip_str.c_str(),
                client_ip_str.c_str(),
                static_cast<unsigned>(port),
                payload_size,
                static_cast<unsigned long long>(duration_s),
                static_cast<unsigned long long>(warmup_samples));
    std::fflush(stdout);

    bench::install_signal_handler();

#if defined(EPH_USE_DPDK)
    auto env_r = bench::load_dpdk_env(globals, /*port_id=*/0);
    if (!env_r) {
        std::fprintf(stderr, "lat_udp: %s\n", env_r.error().c_str());
        return 1;
    }
    auto env = std::move(*env_r);
    bench::print_dpdk_config_echo(env);

    ed::PollerConfig poller_cfg{};
    poller_cfg.port_id     = env.port_id;
    poller_cfg.rx_queue_id = 0;
    auto poller_r = Poller::create(poller_cfg);
#else
    auto poller_r = ek::KernelPoller::create({});
#endif
    if (!poller_r) {
        std::fprintf(stderr, "lat_udp: Poller::create failed: %s\n",
                     poller_r.error().detail);
        return 2;
    }
    auto poller = std::move(poller_r.value());

#if defined(EPH_USE_DPDK)
    // DPDK UDP uses fixed-peer 4-tuple routing: choose a random client
    // src_port and register (client_ip:src, mock_ip:port). The mock
    // (udp_echo.py) replies from the same 4-tuple so the Poller's direction-
    // symmetric lookup matches automatically.
    const uint16_t local_src_port = bench::random_src_port();
    ed::UdpConfig sock_cfg{};
    sock_cfg.legacy.src_ip      = env.src_ip;
    sock_cfg.legacy.dst_ip      = env.dst_ip;
    sock_cfg.legacy.src_port    = local_src_port;
    sock_cfg.legacy.dst_port    = port;
    sock_cfg.legacy.src_mac     = env.src_mac;
    sock_cfg.legacy.dst_mac     = env.gw_mac;
    sock_cfg.legacy.port_id     = env.port_id;
    sock_cfg.legacy.tx_queue_id = 0;
    sock_cfg.legacy.pool        = env.pool;
#else
    ek::UdpConfig sock_cfg{};
    sock_cfg.bind = en::SocketAddr{client_ip_r.value(), 0};
#endif

    auto sock_r = Socket::create(sock_cfg);
    if (!sock_r) {
        std::fprintf(stderr, "lat_udp: Socket::create failed: %s\n",
                     sock_r.error().detail);
        return 3;
    }
    auto sock = std::move(sock_r.value());

    // RawDatagramCodec delivers one datagram per on_datagram callback —
    // each sample is a single send/receive round trip, so a bool flag
    // is sufficient (no byte accounting needed).
    //
    // Phase 11.1: additionally copy the first 24 B into `ts_buf` so the
    // measurement loop can decode the mock-rewritten timestamp block
    // and compute TX/RX legs. Undersize datagrams are flagged via
    // `ts_filled < kTimestampBlockSize` and skipped.
    bool got_echo = false;
    std::array<uint8_t, bench::kTimestampBlockSize> ts_buf{};
    std::size_t ts_filled = 0;
    sock->on_datagram = [&](const uint8_t* data, uint16_t n,
                            const en::SocketAddr& /*src*/) {
        got_echo = true;
        ts_filled = 0;
        if (n >= bench::kTimestampBlockSize) {
            std::memcpy(ts_buf.data(), data, bench::kTimestampBlockSize);
            ts_filled = bench::kTimestampBlockSize;
        }
    };

    if (auto r = poller->add(sock.get()); !r) {
        std::fprintf(stderr, "lat_udp: poller->add failed: %s\n",
                     r.error().detail);
        return 3;
    }

    std::vector<uint8_t> payload(payload_size, 0xCD);
    const char* backend =
#if defined(EPH_USE_DPDK)
        "dpdk";
#else
        "kernel";
#endif
    eu::Recorder rec_rtt{std::string{"lat_udp_"} + backend + "_rtt"};
    eu::Recorder rec_tx {std::string{"lat_udp_"} + backend + "_tx" };
    eu::Recorder rec_rx {std::string{"lat_udp_"} + backend + "_rx" };

    const uint64_t t_start    = bench::monotonic_raw_ns();
    const uint64_t t_deadline = t_start + duration_s * 1'000'000'000ull;
    uint64_t       t_measure_start = 0;
    constexpr uint64_t kPerSampleTimeoutNs = 5ull * 1'000'000'000ull;

    uint64_t sample_idx = 0;
    bool     timed_out  = false;
    while (bench::monotonic_raw_ns() < t_deadline && !bench::shutdown_requested()) {
        const uint64_t t0 = bench::monotonic_raw_ns();
        bench::write_client_ts(
            std::span<uint8_t>{payload.data(), bench::kTimestampBlockSize},
            t0);

        auto send_r = sock->send_to(std::span<const uint8_t>{payload}, remote);
        if (!send_r) {
            std::fprintf(stderr, "lat_udp: send_to failed at sample %llu: %s\n",
                         static_cast<unsigned long long>(sample_idx),
                         send_r.error().detail);
            break;
        }

        got_echo  = false;
        ts_filled = 0;
        while (!got_echo && !bench::shutdown_requested()) {
            poller->poll();
            if (bench::monotonic_raw_ns() - t0 > kPerSampleTimeoutNs) {
                std::fprintf(stderr,
                             "lat_udp: echo timeout at sample %llu\n",
                             static_cast<unsigned long long>(sample_idx));
                timed_out = true;
                break;
            }
        }
        if (timed_out || bench::shutdown_requested()) break;

        const uint64_t t1 = bench::monotonic_raw_ns();
        if (sample_idx == warmup_samples) {
            t_measure_start = t0;
        }
        if (sample_idx >= warmup_samples &&
            ts_filled == bench::kTimestampBlockSize) {
            const auto ts  = bench::read_ts(
                std::span<const uint8_t>{ts_buf.data(), ts_buf.size()});
            const auto lgs = bench::compute_legs(ts, t1);
            rec_rtt.record_ns(lgs.rtt_ns);
            rec_tx .record_ns(lgs.tx_ns);
            rec_rx .record_ns(lgs.rx_ns);
        }
        ++sample_idx;
    }

    const uint64_t wall_time_ns =
        (t_measure_start != 0)
            ? (bench::monotonic_raw_ns() - t_measure_start)
            : 0;
    bench::print_leg_report("lat_udp", backend, rec_rtt, rec_tx, rec_rx,
                            warmup_samples, wall_time_ns);
    (void)bench::export_legs(rec_rtt, rec_tx, rec_rx);

    (void)poller->remove(sock.get());
    sock.reset();
    poller.reset();

    return timed_out ? 4 : 0;
}
