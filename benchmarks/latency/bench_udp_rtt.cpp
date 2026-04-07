/// @file bench_udp_rtt.cpp
/// Kernel socket UDP RTT benchmark.
///
/// Sends UDP datagrams to bench_udp_echo_server with embedded TSC timestamp,
/// receives echo, measures round-trip latency. Reports HdrHistogram percentiles.
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

    if (msg_size < 8) {
        spdlog::error("--msg-size must be >= 8 (need 8 bytes for TSC timestamp)");
        return 1;
    }

    if (!eph::utils::TSC::init()) {
        spdlog::error("TSC calibration failed");
        return 1;
    }

    if (auto r = eph::utils::set_thread_affinity(poll_cpu, "udp-rtt"); !r) {
        spdlog::warn("Failed to pin to core {}: {}", poll_cpu, r.error());
    }

    spdlog::info("bench_udp_rtt (kernel socket): server={}:{}, msg_size={}, count={}",
                 server_ip, port, msg_size, count);

    // Create UDP socket
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        spdlog::error("socket() failed: {}", strerror(errno));
        return 1;
    }

    // Set recv timeout
    timeval tv{.tv_sec = 2, .tv_usec = 0};
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr);

    std::vector<uint8_t> buf(msg_size, 0xAB);
    eph::utils::HdrHistogram rtt_hist{10, 1'000'000'000ULL, 3};  // 10ns - 1s, 3 sig digits

    uint64_t timeouts = 0;
    auto bench_start = std::chrono::steady_clock::now();

    for (size_t i = 0; i < count && g_running.load(std::memory_order_relaxed); ++i) {
        // Embed send TSC in payload[0:8]
        uint64_t send_tsc = eph::utils::TSC::now();
        std::memcpy(buf.data(), &send_tsc, 8);

        ssize_t sent = sendto(sockfd, buf.data(), msg_size, 0,
                              reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr));
        if (sent < 0) {
            spdlog::warn("sendto() failed at iteration {}: {}", i, strerror(errno));
            continue;
        }

        uint8_t recv_buf[2048];
        ssize_t n = recvfrom(sockfd, recv_buf, sizeof(recv_buf), 0, nullptr, nullptr);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ++timeouts;
                continue;
            }
            spdlog::warn("recvfrom() failed at iteration {}: {}", i, strerror(errno));
            continue;
        }

        uint64_t recv_tsc = eph::utils::TSC::now();
        auto rtt_ns_opt = eph::utils::TSC::to_ns(recv_tsc - send_tsc);
        uint64_t rtt_ns = static_cast<uint64_t>(rtt_ns_opt.value_or(0.0));
        [[maybe_unused]] auto _ = rtt_hist.record(rtt_ns);
    }

    auto elapsed = std::chrono::steady_clock::now() - bench_start;
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    close(sockfd);

    // Report
    spdlog::info("──────────────────────────────────────────");
    spdlog::info("UDP RTT (kernel socket):");
    spdlog::info("  samples: {}  timeouts: {}  elapsed: {}ms",
                 rtt_hist.get_total_count(), timeouts, elapsed_ms);
    spdlog::info("  p50:  {:.1f}us", rtt_hist.get_value_at_percentile(50.0) / 1000.0);
    spdlog::info("  p90:  {:.1f}us", rtt_hist.get_value_at_percentile(90.0) / 1000.0);
    spdlog::info("  p99:  {:.1f}us", rtt_hist.get_value_at_percentile(99.0) / 1000.0);
    spdlog::info("  p999: {:.1f}us", rtt_hist.get_value_at_percentile(99.9) / 1000.0);
    spdlog::info("  max:  {:.1f}us", rtt_hist.get_max_value() / 1000.0);
    spdlog::info("──────────────────────────────────────────");

    return 0;
}
