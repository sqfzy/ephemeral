/// @file mock/udp_echo_server.hpp
/// Header-only UDP echo server for latency benchmarks.
///
/// Receives UDP datagrams, embeds server-side TSC timestamps, echoes back.
///
/// Timestamp protocol (all uint64_t little-endian):
///   [0:8]   client_send_tsc   — written by client, preserved by server
///   [8:16]  server_recv_tsc   — written by server on recvfrom
///   [16:24] server_send_tsc   — written by server just before sendto
///
/// Requires payload >= 24 bytes. Smaller packets are echoed without timestamps.
///
/// Can be used in two ways:
///   1. In-process thread via start_udp_echo() — for DPDK benchmarks
///   2. Standalone binary via udp_echo_server_main.cpp

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "eph/utils/cpu.hpp"
#include "eph/utils/time.hpp"
#include "../bench_loop.hpp"

namespace bench::mock {

// ── Configuration ────────────────────────────────────────────────────────

struct UdpEchoConfig {
    std::string bind_ip;    // required
    uint16_t port = 9997;
};

// ── Core echo loop ──────────────────────────────────────────────────────

/// Run the UDP echo server. Blocks until `running` becomes false.
inline void run_udp_echo_server(const UdpEchoConfig& config,
                                std::atomic<bool>& running) {
    int sockfd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        spdlog::error("udp_echo_server: socket() failed: {}", std::strerror(errno));
        return;
    }

    int reuse = 1;
    ::setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config.port);
    if (::inet_pton(AF_INET, config.bind_ip.c_str(), &addr.sin_addr) != 1) {
        spdlog::error("udp_echo_server: invalid bind IP \"{}\"", config.bind_ip);
        ::close(sockfd);
        return;
    }

    if (::bind(sockfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        spdlog::error("udp_echo_server: bind() failed on {}:{}: {}",
                      config.bind_ip, config.port, std::strerror(errno));
        ::close(sockfd);
        return;
    }

    // Recv timeout so we can check `running` periodically
    timeval tv{.tv_sec = 1, .tv_usec = 0};
    ::setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    spdlog::info("udp_echo_server: listening on {}:{}", config.bind_ip, config.port);

    uint8_t buf[2048];
    uint64_t echo_count = 0;

    while (running.load(std::memory_order_relaxed)) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        ssize_t n = ::recvfrom(sockfd, buf, sizeof(buf), 0,
                               reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            spdlog::error("udp_echo_server: recvfrom() failed: {}", std::strerror(errno));
            break;
        }

        // Embed server-side TSC timestamps for single-leg latency measurement
        if (n >= 24) {
            uint64_t server_recv_tsc = eph::utils::TSC::now();
            std::memcpy(buf + 8, &server_recv_tsc, 8);
            uint64_t server_send_tsc = eph::utils::TSC::now();
            std::memcpy(buf + 16, &server_send_tsc, 8);
        }

        ::sendto(sockfd, buf, static_cast<size_t>(n), 0,
                 reinterpret_cast<sockaddr*>(&client_addr), client_len);
        ++echo_count;
    }

    spdlog::info("udp_echo_server: stopped after {} echoes", echo_count);
    ::close(sockfd);
}

// ── In-process launcher ──────────────────────────────────────────────────

/// Start a UDP echo server on a dedicated thread. Returns a MockHandle
/// for lifecycle management.
inline MockHandle start_udp_echo(const UdpEchoConfig& config, int cpu) {
    MockHandle h;
    auto run_flag = h.running;
    h.thread = std::thread([config, cpu, run_flag]() {
        if (auto r = eph::utils::set_thread_affinity(cpu, "udp-echo"); !r) {
            spdlog::warn("udp_echo_server: failed to pin to core {}: {}", cpu, r.error());
        } else {
            spdlog::info("udp_echo_server: pinned to core {}", cpu);
        }
        run_udp_echo_server(config, *run_flag);
    });

    // Brief sleep to let the server bind before clients connect
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    return h;
}

} // namespace bench::mock
