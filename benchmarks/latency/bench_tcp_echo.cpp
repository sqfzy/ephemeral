/// @file bench_tcp_echo.cpp
/// Raw TCP echo latency benchmark with TX/RX single-leg breakdown.
///
/// Uses fixed-size message framing (no WS, no length prefix) — client and
/// server agree on payload size via --payload-sizes. Each message carries
/// 3 TSC timestamps for 4-leg latency decomposition.
///
/// Compiled twice by xmake:
///   - bench_tcp_echo:      kernel socket path
///   - bench_tcp_echo_dpdk: DPDK TcpSession path
///
/// Usage (kernel):
///   bench_tcp_echo --server-ip IP [--port PORT] [--payload-sizes 64,128,512]
///       [--duration 10] [--warmup 2] [--poll-cpu 2] [--output FILE.jsonl]
///
/// Usage (DPDK):
///   bench_tcp_echo_dpdk [EAL args] -- --server-ip IP --local-ip IP
///       --gateway-ip IP [options]

#include <cstdint>
#include <cstring>
#include <vector>

#include <spdlog/spdlog.h>

#include "bench_config.hpp"
#include "bench_loop.hpp"
#include "eph/utils/hdr_histogram.hpp"
#include "eph/utils/time.hpp"

#if defined(EPH_USE_DPDK)
#include "eph/dpdk/arp.hpp"
#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/platform.hpp"
#include "eph/dpdk/tcp.hpp"
#include "eph/dpdk/net_header.hpp"
#include "mock/tcp_echo_server.hpp"
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// ── TSC helper ───────────────────────────────────────────────────────────

static uint64_t tsc_to_ns(uint64_t cycles) {
    auto opt = eph::utils::TSC::to_ns(cycles);
    return static_cast<uint64_t>(opt.value_or(0.0));
}

// ── Kernel helpers ───────────────────────────────────────────────────────

#if !defined(EPH_USE_DPDK)

/// Read exactly `n` bytes from socket. Returns false on error/disconnect.
static bool recv_exact(int fd, uint8_t* buf, size_t n) {
    size_t total = 0;
    while (total < n) {
        ssize_t r = ::recv(fd, buf + total, n - total, 0);
        if (r <= 0) return false;
        total += static_cast<size_t>(r);
    }
    return true;
}

/// Send exactly `n` bytes. Returns false on error.
static bool send_all(int fd, const uint8_t* buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t r = ::send(fd, buf + sent, n - sent, MSG_NOSIGNAL);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<size_t>(r);
    }
    return true;
}

static int run_kernel(bench::BenchConfig& cfg) {
    bench::pin_or_die(cfg.poll_cpu, "bench-poll");

    bench::JsonlWriter jsonl(cfg.output_path);

    for (size_t payload : cfg.payload_sizes) {
        if (payload < 24) {
            spdlog::error("payload size {} < 24 (need 24B for 3 TSC timestamps)", payload);
            continue;
        }

        spdlog::info("TCP echo (kernel): server={}:{}, payload={}B, warmup={}s, duration={}s",
                     cfg.server_ip, cfg.server_port, payload,
                     cfg.warmup.count(), cfg.duration.count());

        // Connect to echo server
        int sockfd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            spdlog::error("socket() failed: {}", std::strerror(errno));
            continue;
        }

        int opt = 1;
        ::setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(cfg.server_port);
        ::inet_pton(AF_INET, cfg.server_ip.c_str(), &server_addr.sin_addr);

        if (::connect(sockfd, reinterpret_cast<sockaddr*>(&server_addr),
                      sizeof(server_addr)) < 0) {
            spdlog::error("connect() failed: {}", std::strerror(errno));
            ::close(sockfd);
            continue;
        }

        eph::utils::HdrHistogram rtt_hist{10, 1'000'000'000ULL, 3};
        eph::utils::HdrHistogram tx_hist{10, 1'000'000'000ULL, 3};
        eph::utils::HdrHistogram rx_hist{10, 1'000'000'000ULL, 3};
        eph::utils::HdrHistogram srv_hist{10, 1'000'000'000ULL, 3};

        std::vector<uint8_t> send_buf(payload, 0xAB);
        std::vector<uint8_t> recv_buf(payload);

        bench::BenchTimer timer;
        timer.start(cfg.warmup, cfg.duration);

        while (timer.is_running() && bench::g_running.load(std::memory_order_relaxed)) {
            uint64_t client_send_tsc = eph::utils::TSC::now();
            std::memcpy(send_buf.data(), &client_send_tsc, 8);

            if (!send_all(sockfd, send_buf.data(), payload)) {
                spdlog::error("send failed");
                break;
            }

            if (!recv_exact(sockfd, recv_buf.data(), payload)) {
                spdlog::error("recv failed");
                break;
            }

            if (timer.is_warmup()) continue;

            uint64_t client_recv_tsc = eph::utils::TSC::now();
            [[maybe_unused]] auto _ = rtt_hist.record(
                tsc_to_ns(client_recv_tsc - client_send_tsc));

            if (payload >= 24) {
                uint64_t server_recv_tsc, server_send_tsc;
                std::memcpy(&server_recv_tsc, recv_buf.data() + 8, 8);
                std::memcpy(&server_send_tsc, recv_buf.data() + 16, 8);
                [[maybe_unused]] auto _1 = tx_hist.record(
                    tsc_to_ns(server_recv_tsc - client_send_tsc));
                [[maybe_unused]] auto _2 = rx_hist.record(
                    tsc_to_ns(client_recv_tsc - server_send_tsc));
                [[maybe_unused]] auto _3 = srv_hist.record(
                    tsc_to_ns(server_send_tsc - server_recv_tsc));
            }
        }

        ::close(sockfd);

        auto result = bench::BenchResult{
            bench::compute_stats(rtt_hist), bench::compute_stats(tx_hist),
            bench::compute_stats(rx_hist), bench::compute_stats(srv_hist),
        };
        bench::print_bench_result("TCP Echo (kernel)", payload, result);
        jsonl.write("tcp_echo", "kernel", payload, result);
    }

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

    auto cfg = bench::parse_bench_config(app_argc, app_argv);
    if (cfg.payload_sizes.empty()) cfg.payload_sizes = bench::kTcpPayloads;

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

    // Start in-process TCP echo server (kernel socket on NIC-A)
    // Use the largest payload size for the mock server's msg_size.
    // The mock reads exactly msg_size bytes, so we reconnect per payload size.
    auto max_payload = *std::max_element(cfg.payload_sizes.begin(), cfg.payload_sizes.end());

    // Initialize DPDK Platform (NIC-B)
    eph::dpdk::PlatformConfig pcfg{.port_id = cfg.dpdk_port_id};
    auto platform = eph::dpdk::Platform::create(pcfg);
    if (!platform) {
        spdlog::error("Platform create failed: {}", platform.error());
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
        return 1;
    }

    bench::pin_or_die(cfg.poll_cpu, "bench-poll");
    bench::JsonlWriter jsonl(cfg.output_path);

    for (size_t payload : cfg.payload_sizes) {
        spdlog::info("TCP echo (DPDK): server={}:{}, payload={}B, warmup={}s, duration={}s",
                     cfg.server_ip, cfg.server_port, payload,
                     cfg.warmup.count(), cfg.duration.count());

        // Start mock for this payload size (fixed-size framing)
        auto echo_mock = bench::mock::start_tcp_echo(
            {.bind_ip = cfg.server_ip, .port = cfg.server_port,
             .msg_size = payload},
            cfg.mock_cpu);

        // Create DPDK TCP session
        eph::dpdk::TcpConfig tcp_cfg{
            .tuple = {
                .src_ip = src_ip, .dst_ip = dst_ip,
                .src_port = 44444, .dst_port = cfg.server_port,
            },
            .src_mac = src_mac, .dst_mac = *gw_mac,
            .port_id = cfg.dpdk_port_id,
        };

        eph::dpdk::TcpSession tcp(tcp_cfg, pool);
        auto conn_result = tcp.connect(std::chrono::milliseconds{3000});
        if (!conn_result) {
            spdlog::error("DPDK TCP connect failed: {}", conn_result.error());
            bench::stop_mock(echo_mock);
            continue;
        }

        eph::utils::HdrHistogram rtt_hist{10, 1'000'000'000ULL, 3};
        eph::utils::HdrHistogram tx_hist{10, 1'000'000'000ULL, 3};
        eph::utils::HdrHistogram rx_hist{10, 1'000'000'000ULL, 3};
        eph::utils::HdrHistogram srv_hist{10, 1'000'000'000ULL, 3};

        std::vector<uint8_t> send_buf(payload, 0xAB);

        bench::BenchTimer timer;
        timer.start(cfg.warmup, cfg.duration);

        // DPDK TCP echo: send fixed-size, poll for response
        while (timer.is_running() && bench::g_running.load(std::memory_order_relaxed)) {
            uint64_t client_send_tsc = eph::utils::TSC::now();
            std::memcpy(send_buf.data(), &client_send_tsc, 8);

            auto send_result = tcp.send(send_buf.data(), payload);
            if (!send_result) continue;

            // Poll for response
            bool got_response = false;
            auto poll_result = tcp.poll_rx([&](const uint8_t* data, uint16_t len) {
                if (len < payload) return;  // partial, wait for more
                if (timer.is_warmup()) { got_response = true; return; }

                uint64_t client_recv_tsc = eph::utils::TSC::now();
                [[maybe_unused]] auto _ = rtt_hist.record(
                    tsc_to_ns(client_recv_tsc - client_send_tsc));

                if (len >= 24) {
                    uint64_t server_recv_tsc, server_send_tsc;
                    std::memcpy(&server_recv_tsc, data + 8, 8);
                    std::memcpy(&server_send_tsc, data + 16, 8);
                    [[maybe_unused]] auto _1 = tx_hist.record(
                        tsc_to_ns(server_recv_tsc - client_send_tsc));
                    [[maybe_unused]] auto _2 = rx_hist.record(
                        tsc_to_ns(client_recv_tsc - server_send_tsc));
                    [[maybe_unused]] auto _3 = srv_hist.record(
                        tsc_to_ns(server_send_tsc - server_recv_tsc));
                }
                got_response = true;
            });

            // Busy-poll until response or timeout
            uint64_t deadline = client_send_tsc +
                static_cast<uint64_t>(2'000'000'000.0 /
                    eph::utils::TSC::to_ns(1).value_or(1.0));
            while (!got_response && eph::utils::TSC::now() < deadline) {
                tcp.poll_rx([&](const uint8_t* data, uint16_t len) {
                    if (len < payload) return;
                    if (timer.is_warmup()) { got_response = true; return; }

                    uint64_t client_recv_tsc = eph::utils::TSC::now();
                    [[maybe_unused]] auto _ = rtt_hist.record(
                        tsc_to_ns(client_recv_tsc - client_send_tsc));

                    if (len >= 24) {
                        uint64_t srv_recv, srv_send;
                        std::memcpy(&srv_recv, data + 8, 8);
                        std::memcpy(&srv_send, data + 16, 8);
                        [[maybe_unused]] auto _1 = tx_hist.record(
                            tsc_to_ns(srv_recv - client_send_tsc));
                        [[maybe_unused]] auto _2 = rx_hist.record(
                            tsc_to_ns(client_recv_tsc - srv_send));
                        [[maybe_unused]] auto _3 = srv_hist.record(
                            tsc_to_ns(srv_send - srv_recv));
                    }
                    got_response = true;
                });
            }
        }

        tcp.close();
        bench::stop_mock(echo_mock);

        auto result = bench::BenchResult{
            bench::compute_stats(rtt_hist), bench::compute_stats(tx_hist),
            bench::compute_stats(rx_hist), bench::compute_stats(srv_hist),
        };
        bench::print_bench_result("TCP Echo (DPDK)", payload, result);
        jsonl.write("tcp_echo", "dpdk", payload, result);
    }

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
    if (cfg.payload_sizes.empty()) cfg.payload_sizes = bench::kTcpPayloads;

    if (cfg.server_ip.empty()) {
        spdlog::error("--server-ip is required");
        return 1;
    }

    return run_kernel(cfg);
#endif
}
