/// @file bench_udp_relay.cpp
/// UDP relay latency benchmark with TX/RX single-leg breakdown.
///
/// Measures store-and-forward latency through a UDP relay node.
/// Topology: Client (port C) → Relay (port A → port B) → Client (port B).
///
/// Compiled twice by xmake:
///   - bench_udp_relay:      kernel socket path
///   - bench_udp_relay_dpdk: DPDK UdpSender + rte_eth_rx_burst path
///
/// Usage (kernel):
///   bench_udp_relay --server-ip IP [--payload-sizes 64,128,512]
///       [--duration 10] [--warmup 2] [--poll-cpu 2] [--output FILE.jsonl]
///
/// Usage (DPDK):
///   bench_udp_relay_dpdk [EAL args] -- --server-ip IP --local-ip IP
///       --gateway-ip IP [options]

#include <cstdint>
#include <cstring>
#include <vector>

#include <spdlog/spdlog.h>

#include "bench_config.hpp"
#include "bench_loop.hpp"
#include "mock/udp_relay_server.hpp"
#include "eph/utils/hdr_histogram.hpp"
#include "eph/utils/time.hpp"

#if defined(EPH_USE_DPDK)
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include "eph/dpdk/arp.hpp"
#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/net_header.hpp"
#include "eph/dpdk/platform.hpp"
#include "eph/dpdk/udp.hpp"
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// Port allocation:
//   relay listens on port 9996
//   relay forwards to port 9995 (client recv port)
//   client sends from ephemeral port to relay port 9996
constexpr uint16_t kRelayListenPort  = 9996;
constexpr uint16_t kClientRecvPort   = 9995;

static uint64_t tsc_to_ns(uint64_t cycles) {
    auto opt = eph::utils::TSC::to_ns(cycles);
    return static_cast<uint64_t>(opt.value_or(0.0));
}

// ── Kernel implementation ────────────────────────────────────────────────

#if !defined(EPH_USE_DPDK)

static int run_kernel(bench::BenchConfig& cfg) {
    // Start in-process relay (both kernel and DPDK variants use in-process relay
    // since the relay itself is the measured component)
    auto relay_mock = bench::mock::start_udp_relay(
        {.bind_ip = cfg.server_ip, .listen_port = kRelayListenPort,
         .forward_ip = cfg.server_ip, .forward_port = kClientRecvPort},
        cfg.mock_cpu);

    bench::pin_or_die(cfg.poll_cpu, "bench-poll");

    // Client send socket (sends to relay)
    int send_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (send_fd < 0) {
        spdlog::error("socket() failed: {}", std::strerror(errno));
        bench::stop_mock(relay_mock);
        return 1;
    }

    // Client recv socket (receives forwarded data from relay)
    int recv_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (recv_fd < 0) {
        spdlog::error("socket() failed: {}", std::strerror(errno));
        ::close(send_fd);
        bench::stop_mock(relay_mock);
        return 1;
    }

    int reuse = 1;
    ::setsockopt(recv_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in recv_addr{};
    recv_addr.sin_family = AF_INET;
    recv_addr.sin_port = htons(kClientRecvPort);
    ::inet_pton(AF_INET, cfg.server_ip.c_str(), &recv_addr.sin_addr);

    if (::bind(recv_fd, reinterpret_cast<sockaddr*>(&recv_addr), sizeof(recv_addr)) < 0) {
        spdlog::error("bind() recv port failed: {}", std::strerror(errno));
        ::close(send_fd);
        ::close(recv_fd);
        bench::stop_mock(relay_mock);
        return 1;
    }

    timeval tv{.tv_sec = 2, .tv_usec = 0};
    ::setsockopt(recv_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in relay_addr{};
    relay_addr.sin_family = AF_INET;
    relay_addr.sin_port = htons(kRelayListenPort);
    ::inet_pton(AF_INET, cfg.server_ip.c_str(), &relay_addr.sin_addr);

    bench::JsonlWriter jsonl(cfg.output_path);

    for (size_t payload : cfg.payload_sizes) {
        if (payload < 24) {
            spdlog::error("payload size {} < 24", payload);
            continue;
        }

        spdlog::info("UDP relay (kernel): relay={}:{}, payload={}B, warmup={}s, duration={}s",
                     cfg.server_ip, kRelayListenPort, payload,
                     cfg.warmup.count(), cfg.duration.count());

        eph::utils::HdrHistogram rtt_hist{10, 1'000'000'000ULL, 3};
        eph::utils::HdrHistogram tx_hist{10, 1'000'000'000ULL, 3};
        eph::utils::HdrHistogram rx_hist{10, 1'000'000'000ULL, 3};
        eph::utils::HdrHistogram relay_hist{10, 1'000'000'000ULL, 3};

        std::vector<uint8_t> buf(payload, 0xAB);
        uint8_t recv_buf[2048];
        uint64_t timeouts = 0;

        bench::BenchTimer timer;
        timer.start(cfg.warmup, cfg.duration);

        while (timer.is_running() && bench::g_running.load(std::memory_order_relaxed)) {
            uint64_t client_send_tsc = eph::utils::TSC::now();
            std::memcpy(buf.data(), &client_send_tsc, 8);

            ssize_t sent = ::sendto(send_fd, buf.data(), payload, 0,
                                    reinterpret_cast<sockaddr*>(&relay_addr),
                                    sizeof(relay_addr));
            if (sent < 0) continue;

            ssize_t n = ::recvfrom(recv_fd, recv_buf, sizeof(recv_buf), 0, nullptr, nullptr);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) { ++timeouts; continue; }
                continue;
            }

            if (timer.is_warmup()) continue;

            uint64_t client_recv_tsc = eph::utils::TSC::now();
            [[maybe_unused]] auto _ = rtt_hist.record(
                tsc_to_ns(client_recv_tsc - client_send_tsc));

            if (n >= 24) {
                uint64_t relay_recv_tsc, relay_send_tsc;
                std::memcpy(&relay_recv_tsc, recv_buf + 8, 8);
                std::memcpy(&relay_send_tsc, recv_buf + 16, 8);
                [[maybe_unused]] auto _1 = tx_hist.record(
                    tsc_to_ns(relay_recv_tsc - client_send_tsc));
                [[maybe_unused]] auto _2 = rx_hist.record(
                    tsc_to_ns(client_recv_tsc - relay_send_tsc));
                [[maybe_unused]] auto _3 = relay_hist.record(
                    tsc_to_ns(relay_send_tsc - relay_recv_tsc));
            }
        }

        auto result = bench::BenchResult{
            bench::compute_stats(rtt_hist), bench::compute_stats(tx_hist),
            bench::compute_stats(rx_hist), bench::compute_stats(relay_hist),
        };
        bench::print_bench_result("UDP Relay (kernel)", payload, result);
        jsonl.write("udp_relay", "kernel", payload, result);

        if (timeouts > 0) {
            spdlog::info("  timeouts: {}", timeouts);
        }
    }

    ::close(send_fd);
    ::close(recv_fd);
    bench::stop_mock(relay_mock);
    return 0;
}

#endif // !EPH_USE_DPDK

// ── DPDK implementation ──────────────────────────────────────────────────

#if defined(EPH_USE_DPDK)

static int run_dpdk(int argc, char** argv) {
    spdlog::info("Initializing DPDK EAL...");
    auto eal = eph::dpdk::EalGuard::init(argc, argv);
    if (!eal) {
        spdlog::error("EAL init failed: {}", eal.error());
        return 1;
    }

    int app_argc = 0;
    char** app_argv = nullptr;
    for (int i = 0; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--") {
            app_argc = argc - i - 1;
            app_argv = argv + i + 1;
            break;
        }
    }

    auto cfg = bench::parse_bench_config(app_argc, app_argv);
    if (cfg.payload_sizes.empty()) cfg.payload_sizes = bench::kUdpPayloads;

    if (cfg.server_ip.empty() || cfg.local_ip.empty() || cfg.gateway_ip.empty()) {
        spdlog::error("--server-ip, --local-ip, --gateway-ip are all required");
        return 1;
    }

    for (size_t p : cfg.payload_sizes) {
        if (p < 24) {
            spdlog::error("payload size {} < 24", p);
            return 1;
        }
    }

    // In-process relay (kernel socket on NIC-A, relay is the measured component)
    auto relay_mock = bench::mock::start_udp_relay(
        {.bind_ip = cfg.server_ip, .listen_port = kRelayListenPort,
         .forward_ip = cfg.local_ip, .forward_port = kClientRecvPort},
        cfg.mock_cpu);

    eph::dpdk::PlatformConfig pcfg{.port_id = cfg.dpdk_port_id};
    auto platform = eph::dpdk::Platform::create(pcfg);
    if (!platform) {
        spdlog::error("Platform create failed: {}", platform.error());
        bench::stop_mock(relay_mock);
        return 1;
    }

    auto* pool = platform->mempool();
    auto src_ip = eph::dpdk::net::parse_ipv4(cfg.local_ip.c_str());
    auto dst_ip = eph::dpdk::net::parse_ipv4(cfg.server_ip.c_str());
    auto gw_ip = eph::dpdk::net::parse_ipv4(cfg.gateway_ip.c_str());

    rte_ether_addr src_mac{};
    rte_eth_macaddr_get(cfg.dpdk_port_id, &src_mac);

    spdlog::info("Resolving gateway MAC via ARP...");
    auto gw_mac = eph::dpdk::arp::resolve(
        cfg.dpdk_port_id, 0, pool, src_mac, src_ip, gw_ip,
        std::chrono::milliseconds{3000});
    if (!gw_mac) {
        spdlog::error("ARP resolve failed: {}", gw_mac.error());
        bench::stop_mock(relay_mock);
        return 1;
    }

    constexpr uint16_t local_send_port = 55556;
    auto sender = eph::dpdk::UdpSender::create({
        .src_ip = src_ip, .dst_ip = dst_ip,
        .src_port = local_send_port, .dst_port = kRelayListenPort,
        .src_mac = src_mac, .dst_mac = *gw_mac,
        .port_id = cfg.dpdk_port_id, .tx_queue_id = 0,
        .pool = pool,
    });
    if (!sender) {
        spdlog::error("UdpSender create failed: {}", sender.error());
        bench::stop_mock(relay_mock);
        return 1;
    }

    bench::pin_or_die(cfg.poll_cpu, "bench-poll");

    uint64_t timeout_cycles = static_cast<uint64_t>(
        2'000'000'000.0 / eph::utils::TSC::to_ns(1).value_or(1.0));

    bench::JsonlWriter jsonl(cfg.output_path);

    for (size_t payload : cfg.payload_sizes) {
        spdlog::info("UDP relay (DPDK): relay={}:{}, payload={}B, warmup={}s, duration={}s",
                     cfg.server_ip, kRelayListenPort, payload,
                     cfg.warmup.count(), cfg.duration.count());

        eph::utils::HdrHistogram rtt_hist{10, 1'000'000'000ULL, 3};
        eph::utils::HdrHistogram tx_hist{10, 1'000'000'000ULL, 3};
        eph::utils::HdrHistogram rx_hist{10, 1'000'000'000ULL, 3};
        eph::utils::HdrHistogram relay_hist{10, 1'000'000'000ULL, 3};

        std::vector<uint8_t> pkt_buf(payload, 0xAB);
        uint64_t timeouts = 0;

        bench::BenchTimer timer;
        timer.start(cfg.warmup, cfg.duration);

        while (timer.is_running() && bench::g_running.load(std::memory_order_relaxed)) {
            uint64_t client_send_tsc = eph::utils::TSC::now();
            std::memcpy(pkt_buf.data(), &client_send_tsc, 8);

            if (!sender->send(pkt_buf.data(), static_cast<uint16_t>(payload))) {
                continue;
            }

            bool got_reply = false;
            uint64_t deadline = client_send_tsc + timeout_cycles;

            while (!got_reply && eph::utils::TSC::now() < deadline) {
                rte_mbuf* pkts[32];
                uint16_t nb_rx = rte_eth_rx_burst(cfg.dpdk_port_id, 0, pkts, 32);
                for (uint16_t j = 0; j < nb_rx; ++j) {
                    auto parsed = eph::dpdk::net::parse_udp_packet(pkts[j]);
                    if (parsed && parsed.dst_port() == kClientRecvPort) {
                        if (!timer.is_warmup()) {
                            uint64_t client_recv_tsc = eph::utils::TSC::now();
                            [[maybe_unused]] auto _ = rtt_hist.record(
                                tsc_to_ns(client_recv_tsc - client_send_tsc));

                            if (parsed.payload_len >= 24) {
                                uint64_t relay_recv_tsc, relay_send_tsc;
                                std::memcpy(&relay_recv_tsc, parsed.payload + 8, 8);
                                std::memcpy(&relay_send_tsc, parsed.payload + 16, 8);
                                [[maybe_unused]] auto _1 = tx_hist.record(
                                    tsc_to_ns(relay_recv_tsc - client_send_tsc));
                                [[maybe_unused]] auto _2 = rx_hist.record(
                                    tsc_to_ns(client_recv_tsc - relay_send_tsc));
                                [[maybe_unused]] auto _3 = relay_hist.record(
                                    tsc_to_ns(relay_send_tsc - relay_recv_tsc));
                            }
                        }
                        got_reply = true;
                    }
                    rte_pktmbuf_free(pkts[j]);
                }
            }
            if (!got_reply) ++timeouts;
        }

        auto result = bench::BenchResult{
            bench::compute_stats(rtt_hist), bench::compute_stats(tx_hist),
            bench::compute_stats(rx_hist), bench::compute_stats(relay_hist),
        };
        bench::print_bench_result("UDP Relay (DPDK)", payload, result);
        jsonl.write("udp_relay", "dpdk", payload, result);

        if (timeouts > 0) {
            spdlog::info("  timeouts: {}", timeouts);
        }
    }

    bench::stop_mock(relay_mock);
    return 0;
}

#endif // EPH_USE_DPDK

// ── main ─────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    bench::install_signal_handlers();

    if (!eph::utils::TSC::init()) {
        spdlog::error("TSC calibration failed");
        return 1;
    }

#if defined(EPH_USE_DPDK)
    return run_dpdk(argc, argv);
#else
    auto cfg = bench::parse_bench_config(argc, argv);
    if (cfg.payload_sizes.empty()) cfg.payload_sizes = bench::kUdpPayloads;

    if (cfg.server_ip.empty()) {
        spdlog::error("--server-ip is required");
        return 1;
    }

    return run_kernel(cfg);
#endif
}
