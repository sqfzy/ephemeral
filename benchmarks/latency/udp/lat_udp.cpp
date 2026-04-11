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

#if defined(EPH_USE_DPDK)
int main(int /*argc*/, char** /*argv*/) {
    spdlog::set_level(spdlog::level::info);
    std::printf("lat_udp_dpdk: v3.3 DpdkUdpSocket API compiled.\n"
                "Real DPDK measurement loop is deferred to a follow-up phase "
                "(EAL init + vfio-pci NIC plumbing). Use lat_udp (kernel) for "
                "live measurements.\n");
    return 0;
}
#else
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

    auto poller_r = ek::KernelPoller::create({});
    if (!poller_r) {
        std::fprintf(stderr, "lat_udp: Poller::create failed: %s\n",
                     poller_r.error().detail);
        return 1;
    }
    auto poller = std::move(poller_r.value());

    ek::UdpConfig sock_cfg{};
    sock_cfg.bind = en::SocketAddr{client_ip_r.value(), 0};

    auto sock_r = Socket::create(sock_cfg);
    if (!sock_r) {
        std::fprintf(stderr, "lat_udp: Socket::create failed: %s\n",
                     sock_r.error().detail);
        return 2;
    }
    auto sock = std::move(sock_r.value());

    // RawDatagramCodec delivers one datagram per on_datagram callback —
    // each sample is a single send/receive round trip, so a bool flag
    // is sufficient (no byte accounting needed).
    bool got_echo = false;
    sock->on_datagram = [&got_echo](const uint8_t* /*data*/, uint16_t /*n*/,
                                    const en::SocketAddr& /*src*/) {
        got_echo = true;
    };

    if (auto r = poller->add(sock.get()); !r) {
        std::fprintf(stderr, "lat_udp: poller->add failed: %s\n",
                     r.error().detail);
        return 3;
    }

    std::vector<uint8_t> payload(payload_size, 0xCD);
    eu::Recorder rec{"lat_udp"};

    const uint64_t t_start    = bench::monotonic_raw_ns();
    const uint64_t t_deadline = t_start + duration_s * 1'000'000'000ull;
    constexpr uint64_t kPerSampleTimeoutNs = 5ull * 1'000'000'000ull;

    uint64_t sample_idx = 0;
    bool     timed_out  = false;
    while (bench::monotonic_raw_ns() < t_deadline && !bench::shutdown_requested()) {
        const uint64_t t0 = bench::monotonic_raw_ns();

        auto send_r = sock->send_to(std::span<const uint8_t>{payload}, remote);
        if (!send_r) {
            std::fprintf(stderr, "lat_udp: send_to failed at sample %llu: %s\n",
                         static_cast<unsigned long long>(sample_idx),
                         send_r.error().detail);
            break;
        }

        got_echo = false;
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
        if (sample_idx >= warmup_samples) {
            rec.record_ns(t1 - t0);
        }
        ++sample_idx;
    }

    const char* backend =
#if defined(EPH_USE_DPDK)
        "dpdk";
#else
        "kernel";
#endif
    bench::print_report("lat_udp", backend, rec, warmup_samples);

    (void)poller->remove(sock.get());
    sock.reset();
    poller.reset();

    return timed_out ? 4 : 0;
}
#endif // EPH_USE_DPDK
