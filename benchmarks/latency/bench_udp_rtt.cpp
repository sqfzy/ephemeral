/// @file bench_udp_rtt.cpp
/// Kernel socket UDP latency benchmark with TX/RX single-leg breakdown.
///
/// Sends UDP datagrams to bench_udp_echo_server with embedded TSC timestamp,
/// receives echo with server-side timestamps, reports:
///   - RTT:        client_send → client_recv
///   - TX latency: client_send → server_recv (client → server one-way)
///   - RX latency: server_send → client_recv (server → client one-way)
///   - Server:     server_recv → server_send (echo processing)
///
/// Run inside a network namespace (via bench_latency.sh) to force traffic
/// through real NICs and avoid kernel loopback optimization.
///
/// Usage: bench_udp_rtt --server-ip IP [--port PORT] [--msg-size N]
///            [--count N] [--poll-cpu N]

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "eph/utils/cpu.hpp"
#include "eph/utils/hdr_histogram.hpp"
#include "eph/utils/time.hpp"

static std::atomic<bool> g_running{true};
static void sig_handler(int) { g_running.store(false, std::memory_order_relaxed); }

static void report_hist(const char* label, eph::utils::HdrHistogram& h) {
    spdlog::info("  {:<12s} p50={:.1f}us  p99={:.1f}us  p999={:.1f}us  max={:.1f}us",
                 label,
                 h.get_value_at_percentile(50.0) / 1000.0,
                 h.get_value_at_percentile(99.0) / 1000.0,
                 h.get_value_at_percentile(99.9) / 1000.0,
                 h.get_max_value() / 1000.0);
}

int main(int argc, char** argv) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    std::string server_ip;
    uint16_t port = 9997;
    size_t msg_size = 64;
    size_t count = 100000;
    int poll_cpu = 2;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--server-ip" && i + 1 < argc) server_ip = argv[++i];
        else if (arg == "--port" && i + 1 < argc) port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--msg-size" && i + 1 < argc) msg_size = static_cast<size_t>(std::stoi(argv[++i]));
        else if (arg == "--count" && i + 1 < argc) count = static_cast<size_t>(std::stoi(argv[++i]));
        else if (arg == "--poll-cpu" && i + 1 < argc) poll_cpu = std::stoi(argv[++i]);
        else if (arg == "--help" || arg == "-h") {
            spdlog::info("Usage: bench_udp_rtt --server-ip IP [--port PORT] [--msg-size N] [--count N] [--poll-cpu N]");
            return 0;
        }
    }

    if (server_ip.empty()) {
        spdlog::error("--server-ip is required");
        return 1;
    }

    if (msg_size < 24) {
        spdlog::error("--msg-size must be >= 24 (need 24 bytes for 3 TSC timestamps)");
        return 1;
    }

    if (!eph::utils::TSC::init()) {
        spdlog::error("TSC calibration failed");
        return 1;
    }

    if (auto r = eph::utils::set_thread_affinity(poll_cpu, "udp-rtt"); !r) {
        spdlog::warn("Failed to pin to core {}: {}", poll_cpu, r.error());
    }

    spdlog::info("bench_udp (kernel socket): server={}:{}, msg_size={}, count={}",
                 server_ip, port, msg_size, count);

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        spdlog::error("socket() failed: {}", strerror(errno));
        return 1;
    }

    timeval tv{.tv_sec = 2, .tv_usec = 0};
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr);

    std::vector<uint8_t> buf(msg_size, 0xAB);

    // 4 histograms: RTT, TX (client→server), RX (server→client), server processing
    eph::utils::HdrHistogram rtt_hist{10, 1'000'000'000ULL, 3};
    eph::utils::HdrHistogram tx_hist{10, 1'000'000'000ULL, 3};
    eph::utils::HdrHistogram rx_hist{10, 1'000'000'000ULL, 3};
    eph::utils::HdrHistogram srv_hist{10, 1'000'000'000ULL, 3};

    uint64_t timeouts = 0;
    auto bench_start = std::chrono::steady_clock::now();

    for (size_t i = 0; i < count && g_running.load(std::memory_order_relaxed); ++i) {
        uint64_t client_send_tsc = eph::utils::TSC::now();
        std::memcpy(buf.data(), &client_send_tsc, 8);

        ssize_t sent = sendto(sockfd, buf.data(), msg_size, 0,
                              reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr));
        if (sent < 0) continue;

        uint8_t recv_buf[2048];
        ssize_t n = recvfrom(sockfd, recv_buf, sizeof(recv_buf), 0, nullptr, nullptr);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { ++timeouts; continue; }
            continue;
        }

        uint64_t client_recv_tsc = eph::utils::TSC::now();

        // Extract timestamps
        auto to_ns = [](uint64_t cycles) -> uint64_t {
            auto opt = eph::utils::TSC::to_ns(cycles);
            return static_cast<uint64_t>(opt.value_or(0.0));
        };

        [[maybe_unused]] auto _ = rtt_hist.record(to_ns(client_recv_tsc - client_send_tsc));

        if (n >= 24) {
            uint64_t server_recv_tsc, server_send_tsc;
            std::memcpy(&server_recv_tsc, recv_buf + 8, 8);
            std::memcpy(&server_send_tsc, recv_buf + 16, 8);

            [[maybe_unused]] auto _tx = tx_hist.record(to_ns(server_recv_tsc - client_send_tsc));
            [[maybe_unused]] auto _rx = rx_hist.record(to_ns(client_recv_tsc - server_send_tsc));
            [[maybe_unused]] auto _sv = srv_hist.record(to_ns(server_send_tsc - server_recv_tsc));
        }
    }

    auto elapsed = std::chrono::steady_clock::now() - bench_start;
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    close(sockfd);

    spdlog::info("──────────────────────────────────────────");
    spdlog::info("UDP Latency (kernel socket):  samples={}  timeouts={}  elapsed={}ms",
                 rtt_hist.get_total_count(), timeouts, elapsed_ms);
    report_hist("RTT", rtt_hist);
    report_hist("TX (c→s)", tx_hist);
    report_hist("RX (s→c)", rx_hist);
    report_hist("Server", srv_hist);
    spdlog::info("──────────────────────────────────────────");

    return 0;
}
