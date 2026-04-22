/// @file lat_ex_md_udp.cpp
/// Latency benchmark: UDP RTT echo with the 24 B timestamp protocol,
/// against the `ex_md_udp` scenario of `benchmarks/mockex/mockex`.
///
/// Real-server note: the Phase-4 `endpoint = wss://...` switch is
/// WebSocket-over-TLS only. UDP cannot carry wss:// and Binance's UDP
/// market-data stream is not publicly exposed either. This scenario
/// stays mock-only by design.
///
/// Structurally identical to `lat_udp` (RawDatagramCodec + 24 B TS
/// prefix + echo); the exchange-pass suffix lives on so users that
/// sweep the full `lat_*` matrix get the same scenario count.
///
/// Measurement clock: both ends use CLOCK_MONOTONIC_RAW via vDSO. Bench
/// client stamps bytes [0:8]
/// before send; mock overwrites [8:24] with its recv/send timestamps.
/// Client computes rtt / tx / rx legs and records each into its own
/// Recorder.

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

// eph-* headers.
#include "eph/codec/raw_datagram_codec.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/utils/recorder.hpp"

#if defined(EPH_USE_DPDK)
// DpdkUdpSocket<RawDatagramCodec> RTT measurement over NIC_B, structurally
// identical to lat_udp's DPDK branch — only the config section + mock
// script name differ.
#  include "eph/net/dpdk/poller.hpp"
#  include "eph/net/dpdk/udp_socket.hpp"
#else
#  include "eph/net/kernel/config.hpp"
#  include "eph/net/kernel/poller.hpp"
#  include "eph/net/kernel/udp_socket.hpp"
#endif

#include "core/config.hpp"
#include "core/measurement.hpp"
#include "core/pin_client.hpp"
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

    auto cfg_r = bench::load_bench_conf(conf_path);
    if (!cfg_r) {
        std::fprintf(stderr, "lat_ex_md_udp: %s\n",
                     bench::format_error(cfg_r.error()).c_str());
        return 1;
    }
    const bench::BenchConfig& bench_cfg = *cfg_r;
    const bench::Scenario* sc = bench_cfg.scenario("lat_ex_md_udp");
    if (sc == nullptr) {
        std::fprintf(stderr,
                     "lat_ex_md_udp: [scenarios.lat_ex_md_udp] not found in %s\n",
                     conf_path);
        return 1;
    }
    const bench::Scenario& scenario = *sc;

    bench::pin_client_from_cfg(bench_cfg, "lat_ex_md_udp");

    auto port_r = scenario.get<uint16_t>("port");
    if (!port_r) {
        std::fprintf(stderr, "lat_ex_md_udp: %s\n",
                     bench::format_error(port_r.error()).c_str());
        return 1;
    }
    const uint16_t port = *port_r;

    const std::size_t payload_size =
        scenario.get_or<uint32_t>("payload_size", 256);

    // 24 B timestamp-block header requirement.
    if (payload_size < bench::kTimestampBlockSize) {
        std::fprintf(stderr,
                     "lat_ex_md_udp: payload_size=%zu < kTimestampBlockSize=%zu "
                     "(TX/RX leg protocol requires a 24 B header)\n",
                     payload_size, bench::kTimestampBlockSize);
        return 1;
    }

    const uint64_t duration_s =
        scenario.get_or<uint32_t>("duration_seconds", 30);
    const uint64_t warmup_samples = bench_cfg.measurement.warmup_samples;

    const std::string mock_ip_str = bench_cfg.networking.server_ip.empty()
                                        ? std::string{"127.0.0.1"}
                                        : bench_cfg.networking.server_ip;
    auto ip_r = en::Ipv4Addr::parse(mock_ip_str);
    if (!ip_r) {
        std::fprintf(stderr, "lat_ex_md_udp: invalid mock_ip '%s': %s\n",
                     mock_ip_str.c_str(), ip_r.error().detail);
        return 1;
    }

    const std::string client_ip_str = bench_cfg.networking.client_ip.empty()
                                          ? std::string{"0.0.0.0"}
                                          : bench_cfg.networking.client_ip;
    auto client_ip_r = en::Ipv4Addr::parse(client_ip_str);
    if (!client_ip_r) {
        std::fprintf(stderr, "lat_ex_md_udp: invalid client_ip '%s': %s\n",
                     client_ip_str.c_str(), client_ip_r.error().detail);
        return 1;
    }

    const en::SocketAddr remote{ip_r.value(), port};

    std::printf("=== lat_ex_md_udp ===\n");
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
    auto env_r = bench::load_dpdk_env(bench_cfg, /*port_id=*/0);
    if (!env_r) {
        std::fprintf(stderr, "lat_ex_md_udp: %s\n", env_r.error().c_str());
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
        std::fprintf(stderr, "lat_ex_md_udp: Poller::create failed: %s\n",
                     poller_r.error().detail);
        return 2;
    }
    auto poller = std::move(poller_r.value());

#if defined(EPH_USE_DPDK)
    // DPDK UDP uses fixed-peer 5-tuple routing: random local src port,
    // mock echoes back from (mock_ip, port) → (client_ip, src_port).
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

#if defined(EPH_USE_DPDK)
    if (auto rr = env.platform.register_poller(0, poller.get()); !rr) {
        std::fprintf(stderr, "lat_ex_md_udp: register_poller failed: %s\n",
                     rr.error().detail);
        return 3;
    }
    auto sock_r = Socket::create_and_attach(std::move(sock_cfg), env.platform);
#else
    auto sock_r = Socket::create(sock_cfg);
#endif
    if (!sock_r) {
        std::fprintf(stderr, "lat_ex_md_udp: Socket::create failed: %s\n",
                     sock_r.error().detail);
        return 3;
    }
    auto sock = std::move(sock_r.value());

    // One datagram per on_datagram callback. Copy the first 24 B for
    // the leg decomposition; mark `ts_filled` so an undersized reply
    // is skipped rather than contributing a bogus sample.
    bool got_echo = false;
    std::array<uint8_t, bench::kTimestampBlockSize> ts_buf{};
    std::size_t ts_filled = 0;
    sock->on_datagram = [&](std::span<const uint8_t> app_datagram,
                            const en::SocketAddr& /*src*/) {
        got_echo = true;
        ts_filled = 0;
        if (app_datagram.size() >= bench::kTimestampBlockSize) {
            std::memcpy(ts_buf.data(), app_datagram.data(),
                        bench::kTimestampBlockSize);
            ts_filled = bench::kTimestampBlockSize;
        }
    };

#if !defined(EPH_USE_DPDK)
    if (auto r = poller->add(sock.get()); !r) {
        std::fprintf(stderr, "lat_ex_md_udp: poller->add failed: %s\n",
                     r.error().detail);
        return 3;
    }
#endif

    // ── Measurement loop ─────────────────────────────────────────────────
    std::vector<uint8_t> payload(payload_size, 0xEF);
    const char* backend =
#if defined(EPH_USE_DPDK)
        "dpdk";
#else
        "kernel";
#endif
    eu::Recorder rec_rtt{std::string{"lat_ex_md_udp_"} + backend + "_rtt"};
    eu::Recorder rec_tx {std::string{"lat_ex_md_udp_"} + backend + "_tx" };
    eu::Recorder rec_rx {std::string{"lat_ex_md_udp_"} + backend + "_rx" };

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
            std::fprintf(stderr,
                         "lat_ex_md_udp: send_to failed at sample %llu: %s\n",
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
                             "lat_ex_md_udp: echo timeout at sample %llu\n",
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
    bench::print_leg_report("lat_ex_md_udp", backend, rec_rtt, rec_tx, rec_rx,
                            warmup_samples, wall_time_ns);
    (void)bench::export_legs(rec_rtt, rec_tx, rec_rx);

    (void)poller->remove(sock.get());
    sock.reset();
    poller.reset();

    return timed_out ? 4 : 0;
}
