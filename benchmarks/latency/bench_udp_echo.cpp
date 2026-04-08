/// @file bench_udp_echo.cpp
/// Raw UDP echo latency benchmark with TX/RX single-leg breakdown.
///
/// Sends UDP datagrams to a UDP echo server with embedded TSC timestamps,
/// receives echo with server-side timestamps, reports:
///   - RTT:    client_send → client_recv
///   - TX:     client_send → server_recv (one-way ingress)
///   - RX:     server_send → client_recv (one-way egress)
///   - Server: server_recv → server_send (echo processing)
///
/// Compiled twice by xmake:
///   - bench_udp_echo:      kernel socket path
///   - bench_udp_echo_dpdk: DPDK UdpSender + rte_eth_rx_burst path
///
/// Usage (kernel):
///   bench_udp_echo --server-ip IP [--port PORT] [--payload-sizes 64,128,512]
///       [--duration 10] [--warmup 2] [--poll-cpu 2] [--output FILE.jsonl]
///
/// Usage (DPDK):
///   bench_udp_echo_dpdk [EAL args] -- --server-ip IP --local-ip IP
///       --gateway-ip IP [--port PORT] [--payload-sizes 64,128,512]
///       [--duration 10] [--warmup 2] [--poll-cpu 2] [--mock-cpu 4]
///       [--output FILE.jsonl]

#include <cstdint>
#include <cstring>
#include <vector>

#include <spdlog/spdlog.h>

#include "bench_config.hpp"
#include "bench_loop.hpp"
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
#include "mock/udp_echo_server.hpp"
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// ── TSC helpers ──────────────────────────────────────────────────────────

static uint64_t tsc_to_ns(uint64_t cycles) {
    auto opt = eph::utils::TSC::to_ns(cycles);
    return static_cast<uint64_t>(opt.value_or(0.0));
}

// ── Kernel socket implementation ─────────────────────────────────────────

#if !defined(EPH_USE_DPDK)

static int run_kernel(bench::BenchConfig& cfg) {
    bench::pin_or_die(cfg.poll_cpu, "bench-poll");

    int sockfd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        spdlog::error("socket() failed: {}", std::strerror(errno));
        return 1;
    }

    // 2-second recv timeout as safety valve
    timeval tv{.tv_sec = 2, .tv_usec = 0};
    ::setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(cfg.server_port);
    ::inet_pton(AF_INET, cfg.server_ip.c_str(), &server_addr.sin_addr);

    bench::JsonlWriter jsonl(cfg.output_path);

    for (size_t payload : cfg.payload_sizes) {
        if (payload < 24) {
            spdlog::error("payload size {} < 24 (need 24B for 3 TSC timestamps)", payload);
            continue;
        }

        spdlog::info("UDP echo (kernel): server={}:{}, payload={}B, warmup={}s, duration={}s",
                     cfg.server_ip, cfg.server_port, payload,
                     cfg.warmup.count(), cfg.duration.count());

        eph::utils::HdrHistogram rtt_hist{10, 1'000'000'000ULL, 3};
        eph::utils::HdrHistogram tx_hist{10, 1'000'000'000ULL, 3};
        eph::utils::HdrHistogram rx_hist{10, 1'000'000'000ULL, 3};
        eph::utils::HdrHistogram srv_hist{10, 1'000'000'000ULL, 3};

        std::vector<uint8_t> buf(payload, 0xAB);
        uint8_t recv_buf[2048];
        uint64_t timeouts = 0;

        bench::BenchTimer timer;
        timer.start(cfg.warmup, cfg.duration);

        while (timer.is_running() && bench::g_running.load(std::memory_order_relaxed)) {
            uint64_t client_send_tsc = eph::utils::TSC::now();
            std::memcpy(buf.data(), &client_send_tsc, 8);

            ssize_t sent = ::sendto(sockfd, buf.data(), payload, 0,
                                    reinterpret_cast<sockaddr*>(&server_addr),
                                    sizeof(server_addr));
            if (sent < 0) continue;

            ssize_t n = ::recvfrom(sockfd, recv_buf, sizeof(recv_buf), 0, nullptr, nullptr);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) { ++timeouts; continue; }
                continue;
            }

            if (timer.is_warmup()) continue;

            uint64_t client_recv_tsc = eph::utils::TSC::now();
            [[maybe_unused]] auto _ = rtt_hist.record(tsc_to_ns(client_recv_tsc - client_send_tsc));

            if (n >= 24) {
                uint64_t server_recv_tsc, server_send_tsc;
                std::memcpy(&server_recv_tsc, recv_buf + 8, 8);
                std::memcpy(&server_send_tsc, recv_buf + 16, 8);
                [[maybe_unused]] auto _1 = tx_hist.record(tsc_to_ns(server_recv_tsc - client_send_tsc));
                [[maybe_unused]] auto _2 = rx_hist.record(tsc_to_ns(client_recv_tsc - server_send_tsc));
                [[maybe_unused]] auto _3 = srv_hist.record(tsc_to_ns(server_send_tsc - server_recv_tsc));
            }
        }

        auto result = bench::BenchResult{
            bench::compute_stats(rtt_hist), bench::compute_stats(tx_hist),
            bench::compute_stats(rx_hist), bench::compute_stats(srv_hist),
        };
        bench::print_bench_result("UDP Echo (kernel)", payload, result);
        jsonl.write("udp_echo", "kernel", payload, result);

        if (timeouts > 0) {
            spdlog::info("  timeouts: {}", timeouts);
        }
    }

    ::close(sockfd);
    return 0;
}

#endif // !EPH_USE_DPDK

// ── DPDK implementation ──────────────────────────────────────────────────

#if defined(EPH_USE_DPDK)

static int run_dpdk(int argc, char** argv) {
    // Split EAL args from app args at "--" BEFORE EalGuard::init, since
    // EalGuard consumes EAL portion of argv.
    int app_argc = 0;
    char** app_argv = nullptr;
    for (int i = 0; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--") {
            app_argc = argc - i - 1;
            app_argv = argv + i + 1;
            break;
        }
    }

    spdlog::info("Initializing DPDK EAL...");
    auto eal = eph::dpdk::EalGuard::init(argc, argv);
    if (!eal) {
        spdlog::error("EAL init failed: {}", eal.error());
        return 1;
    }

    auto cfg = bench::parse_bench_config(app_argc, app_argv);
    if (cfg.payload_sizes.empty()) cfg.payload_sizes = bench::kUdpPayloads;

    if (cfg.server_ip.empty() || cfg.local_ip.empty() || cfg.gateway_ip.empty()) {
        spdlog::error("--server-ip, --local-ip, --gateway-ip are all required");
        return 1;
    }

    for (size_t p : cfg.payload_sizes) {
        if (p < 24) {
            spdlog::error("payload size {} < 24 (need 24B for 3 TSC timestamps)", p);
            return 1;
        }
    }

    // Start in-process UDP echo server (kernel socket on NIC-A)
    auto echo_mock = bench::mock::start_udp_echo(
        {.bind_ip = cfg.server_ip, .port = cfg.server_port}, cfg.mock_cpu);

    // Initialize DPDK Platform (NIC-B)
    eph::dpdk::PlatformConfig pcfg{.port_id = cfg.dpdk_port_id};
    auto platform = eph::dpdk::Platform::create(pcfg);
    if (!platform) {
        spdlog::error("Platform create failed: {}", platform.error());
        bench::stop_mock(echo_mock);
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
        bench::stop_mock(echo_mock);
        return 1;
    }
    spdlog::info("Gateway MAC resolved: {}", eph::dpdk::net::format_mac(*gw_mac).data());

    constexpr uint16_t local_udp_port = 55555;
    auto sender = eph::dpdk::UdpSender::create({
        .src_ip = src_ip, .dst_ip = dst_ip,
        .src_port = local_udp_port, .dst_port = cfg.server_port,
        .src_mac = src_mac, .dst_mac = *gw_mac,
        .port_id = cfg.dpdk_port_id, .tx_queue_id = 0,
        .pool = pool,
    });
    if (!sender) {
        spdlog::error("UdpSender create failed: {}", sender.error());
        bench::stop_mock(echo_mock);
        return 1;
    }

    bench::pin_or_die(cfg.poll_cpu, "bench-poll");

    // Precompute timeout in TSC cycles (~2 seconds)
    auto cycles_per_ns = eph::utils::TSC::to_ns(1);
    uint64_t timeout_cycles = static_cast<uint64_t>(
        2'000'000'000.0 / cycles_per_ns.value_or(1.0));

    bench::JsonlWriter jsonl(cfg.output_path);

    for (size_t payload : cfg.payload_sizes) {
        spdlog::info("UDP echo (DPDK): server={}:{}, local={}, payload={}B, "
                     "warmup={}s, duration={}s",
                     cfg.server_ip, cfg.server_port, cfg.local_ip, payload,
                     cfg.warmup.count(), cfg.duration.count());

        eph::utils::HdrHistogram rtt_hist{10, 1'000'000'000ULL, 3};
        eph::utils::HdrHistogram tx_hist{10, 1'000'000'000ULL, 3};
        eph::utils::HdrHistogram rx_hist{10, 1'000'000'000ULL, 3};
        eph::utils::HdrHistogram srv_hist{10, 1'000'000'000ULL, 3};

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
                    if (parsed && parsed.dst_port() == local_udp_port) {
                        if (!timer.is_warmup()) {
                            uint64_t client_recv_tsc = eph::utils::TSC::now();
                            [[maybe_unused]] auto _ = rtt_hist.record(
                                tsc_to_ns(client_recv_tsc - client_send_tsc));

                            if (parsed.payload_len >= 24) {
                                uint64_t server_recv_tsc, server_send_tsc;
                                std::memcpy(&server_recv_tsc, parsed.payload + 8, 8);
                                std::memcpy(&server_send_tsc, parsed.payload + 16, 8);
                                [[maybe_unused]] auto _1 = tx_hist.record(
                                    tsc_to_ns(server_recv_tsc - client_send_tsc));
                                [[maybe_unused]] auto _2 = rx_hist.record(
                                    tsc_to_ns(client_recv_tsc - server_send_tsc));
                                [[maybe_unused]] auto _3 = srv_hist.record(
                                    tsc_to_ns(server_send_tsc - server_recv_tsc));
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
            bench::compute_stats(rx_hist), bench::compute_stats(srv_hist),
        };
        bench::print_bench_result("UDP Echo (DPDK)", payload, result);
        jsonl.write("udp_echo", "dpdk", payload, result);

        if (timeouts > 0) {
            spdlog::info("  timeouts: {}", timeouts);
        }
    }

    bench::stop_mock(echo_mock);
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

    for (size_t p : cfg.payload_sizes) {
        if (p < 24) {
            spdlog::error("payload size {} < 24 (need 24B for 3 TSC timestamps)", p);
            return 1;
        }
    }

    return run_kernel(cfg);
#endif
}
