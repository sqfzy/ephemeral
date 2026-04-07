/// @file bench_udp_echo_server.cpp
/// Raw UDP echo server for latency benchmarks.
///
/// Receives UDP datagrams, writes server TSC into payload[0:8], echoes back.
/// Protocol: payload first 8 bytes = uint64_t client send TSC (little-endian).
/// Server overwrites bytes [8:16] with server timestamp before echo.
///
/// Usage: bench_udp_echo_server --bind-ip IP [--port PORT] [--cpu N]

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "eph/utils/cpu.hpp"
#include "eph/utils/time.hpp"

static std::atomic<bool> g_running{true};
static void sig_handler(int) { g_running.store(false, std::memory_order_relaxed); }

int main(int argc, char** argv) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    std::string bind_ip;
    uint16_t port = 9997;
    int cpu = 4;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--bind-ip" && i + 1 < argc) bind_ip = argv[++i];
        else if (arg == "--port" && i + 1 < argc) port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--cpu" && i + 1 < argc) cpu = std::stoi(argv[++i]);
        else if (arg == "--help" || arg == "-h") {
            spdlog::info("Usage: bench_udp_echo_server --bind-ip IP [--port PORT] [--cpu N]");
            return 0;
        }
    }

    if (bind_ip.empty()) {
        spdlog::error("--bind-ip is required");
        return 1;
    }

    if (!eph::utils::TSC::init()) {
        spdlog::error("TSC calibration failed");
        return 1;
    }

    if (auto r = eph::utils::set_thread_affinity(cpu, "udp-echo"); !r) {
        spdlog::warn("Failed to pin to core {}: {}", cpu, r.error());
    } else {
        spdlog::info("Pinned to core {}", cpu);
    }

    // Create UDP socket
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        spdlog::error("socket() failed: {}", strerror(errno));
        return 1;
    }

    // Allow port reuse
    int reuse = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // Bind
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, bind_ip.c_str(), &addr.sin_addr);

    if (bind(sockfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        spdlog::error("bind() failed: {}", strerror(errno));
        close(sockfd);
        return 1;
    }

    // Set recv timeout so we can check g_running periodically
    timeval tv{.tv_sec = 1, .tv_usec = 0};
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    spdlog::info("UDP echo server listening on {}:{}", bind_ip, port);

    uint8_t buf[2048];
    uint64_t echo_count = 0;
    auto start = std::chrono::steady_clock::now();

    while (g_running.load(std::memory_order_relaxed)) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        ssize_t n = recvfrom(sockfd, buf, sizeof(buf), 0,
                             reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            spdlog::error("recvfrom() failed: {}", strerror(errno));
            break;
        }

        // Echo back immediately
        sendto(sockfd, buf, static_cast<size_t>(n), 0,
               reinterpret_cast<sockaddr*>(&client_addr), client_len);
        ++echo_count;
    }

    auto elapsed = std::chrono::steady_clock::now() - start;
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    spdlog::info("UDP echo server stopped: {} echoed in {}s", echo_count, secs);

    close(sockfd);
    return 0;
}
