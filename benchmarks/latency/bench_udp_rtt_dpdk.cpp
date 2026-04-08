/// @file bench_udp_rtt_dpdk.cpp
/// DPDK UDP RTT benchmark — kernel-bypass send + receive.
///
/// Uses UdpSender for TX and raw rte_eth_rx_burst + parse_udp_packet for RX.
/// Mock UDP echo server runs in-process on NIC-A (kernel socket).
/// DPDK accesses NIC-B via PMD. Traffic goes through real NICs.
///
/// Usage: bench_udp_rtt_dpdk [EAL args] -- --server-ip IP --local-ip IP
///            --gateway-ip IP [--port PORT] [--msg-size N] [--count N]
///            [--poll-cpu N] [--mock-cpu N]

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include <rte_ethdev.h>
#include <rte_mbuf.h>

#include "eph/dpdk/arp.hpp"
#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/platform.hpp"
#include "eph/dpdk/udp.hpp"
#include "eph/dpdk/net_header.hpp"
#include "eph/utils/cpu.hpp"
#include "eph/utils/hdr_histogram.hpp"
#include "eph/utils/time.hpp"

// In-process UDP echo server (kernel socket, runs on NIC-A)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static std::atomic<bool> g_running{true};
static void sig_handler(int) { g_running.store(false, std::memory_order_relaxed); }

namespace {

struct UdpEchoServer {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> running = std::make_shared<std::atomic<bool>>(true);
};

UdpEchoServer start_udp_echo(const std::string& bind_ip, uint16_t port, int cpu) {
    UdpEchoServer srv;
    auto run_flag = srv.running;
    srv.thread = std::thread([run_flag, bind_ip, port, cpu]() {
        if (auto r = eph::utils::set_thread_affinity(cpu, "udp-echo"); !r)
            spdlog::warn("Failed to pin udp-echo to core {}", cpu);

        int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0) { spdlog::error("echo socket failed"); return; }

        int reuse = 1;
        setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, bind_ip.c_str(), &addr.sin_addr);
        if (bind(sockfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            spdlog::error("echo bind failed: {}", strerror(errno));
            close(sockfd);
            return;
        }

        timeval tv{.tv_sec = 1, .tv_usec = 0};
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        spdlog::info("In-process UDP echo server on {}:{}", bind_ip, port);

        uint8_t buf[2048];
        while (run_flag->load(std::memory_order_relaxed)) {
            sockaddr_in client{};
            socklen_t clen = sizeof(client);
            ssize_t n = recvfrom(sockfd, buf, sizeof(buf), 0,
                                 reinterpret_cast<sockaddr*>(&client), &clen);
            if (n > 0) {
                // Embed server timestamps for single-leg measurement
                if (n >= 24) {
                    uint64_t srv_recv = eph::utils::TSC::now();
                    std::memcpy(buf + 8, &srv_recv, 8);
                    uint64_t srv_send = eph::utils::TSC::now();
                    std::memcpy(buf + 16, &srv_send, 8);
                }
                sendto(sockfd, buf, static_cast<size_t>(n), 0,
                       reinterpret_cast<sockaddr*>(&client), clen);
            }
        }
        close(sockfd);
    });
    // Wait for server to be ready
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    return srv;
}

} // namespace

int main(int argc, char** argv) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    if (!eph::utils::TSC::init()) {
        spdlog::error("TSC calibration failed");
        return 1;
    }

    // Split EAL args from app args at "--"
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
    if (!eal) { spdlog::error("EAL init failed: {}", eal.error()); return 1; }

    std::string server_ip, local_ip, gateway_ip;
    uint16_t port = 9997, dpdk_port_id = 0;
    size_t msg_size = 64, count = 100000;
    int poll_cpu = 2, mock_cpu = 4;

    for (int i = 0; i < app_argc; ++i) {
        std::string arg = app_argv[i];
        if (arg == "--server-ip" && i + 1 < app_argc) server_ip = app_argv[++i];
        else if (arg == "--local-ip" && i + 1 < app_argc) local_ip = app_argv[++i];
        else if (arg == "--gateway-ip" && i + 1 < app_argc) gateway_ip = app_argv[++i];
        else if (arg == "--port" && i + 1 < app_argc) port = static_cast<uint16_t>(std::stoi(app_argv[++i]));
        else if (arg == "--dpdk-port" && i + 1 < app_argc) dpdk_port_id = static_cast<uint16_t>(std::stoi(app_argv[++i]));
        else if (arg == "--msg-size" && i + 1 < app_argc) msg_size = static_cast<size_t>(std::stoi(app_argv[++i]));
        else if (arg == "--count" && i + 1 < app_argc) count = static_cast<size_t>(std::stoi(app_argv[++i]));
        else if (arg == "--poll-cpu" && i + 1 < app_argc) poll_cpu = std::stoi(app_argv[++i]);
        else if (arg == "--mock-cpu" && i + 1 < app_argc) mock_cpu = std::stoi(app_argv[++i]);
        else if (arg == "--help") {
            spdlog::info("Usage: bench_udp_rtt_dpdk [EAL] -- --server-ip IP --local-ip IP --gateway-ip IP [options]");
            return 0;
        }
    }

    if (server_ip.empty() || local_ip.empty() || gateway_ip.empty()) {
        spdlog::error("--server-ip, --local-ip, --gateway-ip are all required");
        return 1;
    }
    if (msg_size < 24) {
        spdlog::error("--msg-size must be >= 24 (need 24 bytes for 3 TSC timestamps)");
        return 1;
    }

    spdlog::info("bench_udp_rtt_dpdk: server={}:{}, local={}, gw={}, msg={}B, count={}",
                 server_ip, port, local_ip, gateway_ip, msg_size, count);

    // Start in-process UDP echo server (kernel socket on NIC-A)
    auto echo_srv = start_udp_echo(server_ip, port, mock_cpu);

    // Initialize DPDK Platform (NIC-B)
    eph::dpdk::PlatformConfig pcfg{.port_id = dpdk_port_id};
    auto platform = eph::dpdk::Platform::create(pcfg);
    if (!platform) {
        spdlog::error("Platform create failed: {}", platform.error());
        echo_srv.running->store(false);
        echo_srv.thread.join();
        return 1;
    }

    auto* pool = platform->mempool();
    auto src_ip = eph::dpdk::net::parse_ipv4(local_ip.c_str());
    auto dst_ip = eph::dpdk::net::parse_ipv4(server_ip.c_str());
    auto gw_ip = eph::dpdk::net::parse_ipv4(gateway_ip.c_str());

    // Get local MAC
    rte_ether_addr src_mac{};
    rte_eth_macaddr_get(dpdk_port_id, &src_mac);

    // ARP resolve gateway MAC
    spdlog::info("Resolving gateway MAC via ARP...");
    auto gw_mac = eph::dpdk::arp::resolve(
        dpdk_port_id, 0, pool, src_mac, src_ip, gw_ip,
        std::chrono::milliseconds{3000});
    if (!gw_mac) {
        spdlog::error("ARP resolve failed: {}", gw_mac.error());
        echo_srv.running->store(false);
        echo_srv.thread.join();
        return 1;
    }
    spdlog::info("Gateway MAC resolved: {}",
                 eph::dpdk::net::format_mac(*gw_mac).data());

    // Create UdpSender
    constexpr uint16_t local_udp_port = 55555;
    auto sender = eph::dpdk::UdpSender::create({
        .src_ip = src_ip, .dst_ip = dst_ip,
        .src_port = local_udp_port, .dst_port = port,
        .src_mac = src_mac, .dst_mac = *gw_mac,
        .port_id = dpdk_port_id, .tx_queue_id = 0,
        .pool = pool,
    });
    if (!sender) {
        spdlog::error("UdpSender create failed: {}", sender.error());
        echo_srv.running->store(false);
        echo_srv.thread.join();
        return 1;
    }

    if (auto r = eph::utils::set_thread_affinity(poll_cpu, "udp-rtt-dpdk"); !r)
        spdlog::warn("Failed to pin to core {}", poll_cpu);

    auto to_ns = [](uint64_t cycles) -> uint64_t {
        auto opt = eph::utils::TSC::to_ns(cycles);
        return static_cast<uint64_t>(opt.value_or(0.0));
    };

    auto report_hist = [](const char* label, eph::utils::HdrHistogram& h) {
        spdlog::info("  {:<12s} p50={:.1f}us  p99={:.1f}us  p999={:.1f}us  max={:.1f}us",
                     label,
                     h.get_value_at_percentile(50.0) / 1000.0,
                     h.get_value_at_percentile(99.0) / 1000.0,
                     h.get_value_at_percentile(99.9) / 1000.0,
                     h.get_max_value() / 1000.0);
    };

    // Latency measurement loop
    eph::utils::HdrHistogram rtt_hist{10, 1'000'000'000ULL, 3};
    eph::utils::HdrHistogram tx_hist{10, 1'000'000'000ULL, 3};
    eph::utils::HdrHistogram rx_hist{10, 1'000'000'000ULL, 3};
    eph::utils::HdrHistogram srv_hist{10, 1'000'000'000ULL, 3};
    std::vector<uint8_t> payload(msg_size, 0xAB);
    uint64_t timeouts = 0;
    auto bench_start = std::chrono::steady_clock::now();

    // Precompute timeout in TSC cycles (~2 seconds)
    auto cycles_per_ns = eph::utils::TSC::to_ns(1);
    uint64_t timeout_cycles = static_cast<uint64_t>(2'000'000'000.0 / cycles_per_ns.value_or(1.0));

    for (size_t i = 0; i < count && g_running.load(std::memory_order_relaxed); ++i) {
        uint64_t client_send_tsc = eph::utils::TSC::now();
        std::memcpy(payload.data(), &client_send_tsc, 8);

        if (!sender->send(payload.data(), static_cast<uint16_t>(msg_size))) {
            continue;
        }

        bool got_reply = false;
        uint64_t deadline = client_send_tsc + timeout_cycles;

        while (!got_reply && eph::utils::TSC::now() < deadline) {
            rte_mbuf* pkts[32];
            uint16_t nb_rx = rte_eth_rx_burst(dpdk_port_id, 0, pkts, 32);
            for (uint16_t j = 0; j < nb_rx; ++j) {
                auto parsed = eph::dpdk::net::parse_udp_packet(pkts[j]);
                if (parsed && parsed.dst_port() == local_udp_port) {
                    uint64_t client_recv_tsc = eph::utils::TSC::now();
                    [[maybe_unused]] auto _ = rtt_hist.record(to_ns(client_recv_tsc - client_send_tsc));

                    // Extract server timestamps for single-leg breakdown
                    if (parsed.payload_len >= 24) {
                        uint64_t server_recv_tsc, server_send_tsc;
                        std::memcpy(&server_recv_tsc, parsed.payload + 8, 8);
                        std::memcpy(&server_send_tsc, parsed.payload + 16, 8);
                        [[maybe_unused]] auto _tx = tx_hist.record(to_ns(server_recv_tsc - client_send_tsc));
                        [[maybe_unused]] auto _rx = rx_hist.record(to_ns(client_recv_tsc - server_send_tsc));
                        [[maybe_unused]] auto _sv = srv_hist.record(to_ns(server_send_tsc - server_recv_tsc));
                    }
                    got_reply = true;
                }
                rte_pktmbuf_free(pkts[j]);
            }
        }
        if (!got_reply) ++timeouts;
    }

    auto elapsed = std::chrono::steady_clock::now() - bench_start;
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    spdlog::info("──────────────────────────────────────────");
    spdlog::info("UDP Latency (DPDK):  samples={}  timeouts={}  elapsed={}ms",
                 rtt_hist.get_total_count(), timeouts, elapsed_ms);
    report_hist("RTT", rtt_hist);
    report_hist("TX (c→s)", tx_hist);
    report_hist("RX (s→c)", rx_hist);
    report_hist("Server", srv_hist);
    spdlog::info("──────────────────────────────────────────");

    // Cleanup
    echo_srv.running->store(false);
    echo_srv.thread.join();
    return 0;
}
