/// @file mock/tcp_echo_server.hpp
/// Header-only raw TCP echo server for latency benchmarks.
///
/// Uses fixed-size message framing: both client and server agree on a
/// message size via configuration. Each message is exactly N bytes.
///
/// Timestamp protocol (all uint64_t little-endian, same as UDP echo):
///   [0:8]   client_send_tsc   — written by client, preserved by server
///   [8:16]  server_recv_tsc   — written by server after full recv
///   [16:24] server_send_tsc   — written by server just before send
///
/// Requires msg_size >= 24. Accepts ONE client at a time (benchmark is 1:1).

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
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "eph/utils/cpu.hpp"
#include "eph/utils/time.hpp"
#include "../bench_loop.hpp"

namespace bench::mock {

// ── Configuration ────────────────────────────────────────────────────────

struct TcpEchoConfig {
    std::string bind_ip;    // required
    uint16_t port = 9998;
    size_t msg_size = 64;   // fixed message size (both directions)
};

// ── Helpers ──────────────────────────────────────────────────────────────

namespace detail_tcp {

/// Read exactly `n` bytes from fd. Returns false on error/disconnect.
inline bool recv_exact(int fd, uint8_t* buf, size_t n) {
    size_t total = 0;
    while (total < n) {
        ssize_t r = ::recv(fd, buf + total, n - total, 0);
        if (r <= 0) return false;
        total += static_cast<size_t>(r);
    }
    return true;
}

/// Send exactly `n` bytes to fd. Returns false on error.
inline bool send_all(int fd, const uint8_t* buf, size_t n) {
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

} // namespace detail_tcp

// ── Core echo loop ──────────────────────────────────────────────────────

/// Run the TCP echo server. Blocks until `running` becomes false.
/// Accepts one client at a time, reads fixed-size messages, embeds TSC
/// timestamps, and echoes back.
inline void run_tcp_echo_server(const TcpEchoConfig& config,
                                std::atomic<bool>& running) {
    spdlog::info("tcp_echo_server: starting on {}:{} (msg_size={})",
                 config.bind_ip, config.port, config.msg_size);

    int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        spdlog::error("tcp_echo_server: socket() failed: {}", std::strerror(errno));
        return;
    }

    int opt = 1;
    ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config.port);
    if (::inet_pton(AF_INET, config.bind_ip.c_str(), &addr.sin_addr) != 1) {
        spdlog::error("tcp_echo_server: invalid bind IP \"{}\"", config.bind_ip);
        ::close(server_fd);
        return;
    }

    if (::bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        spdlog::error("tcp_echo_server: bind() failed on {}:{}: {}",
                      config.bind_ip, config.port, std::strerror(errno));
        ::close(server_fd);
        return;
    }

    if (::listen(server_fd, 1) < 0) {
        spdlog::error("tcp_echo_server: listen() failed: {}", std::strerror(errno));
        ::close(server_fd);
        return;
    }

    spdlog::info("tcp_echo_server: listening on {}:{}", config.bind_ip, config.port);

    std::vector<uint8_t> buf(config.msg_size);

    // Outer loop: accept clients until shutdown
    while (running.load(std::memory_order_relaxed)) {
        // Poll for incoming connection with timeout (for graceful shutdown)
        pollfd pfd{.fd = server_fd, .events = POLLIN, .revents = 0};
        int ret = ::poll(&pfd, 1, 200);
        if (ret <= 0) continue;

        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = ::accept(server_fd,
                                 reinterpret_cast<sockaddr*>(&client_addr),
                                 &client_len);
        if (client_fd < 0) continue;

        // TCP_NODELAY for low-latency echo
        ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        char client_ip[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        spdlog::info("tcp_echo_server: client connected from {}:{}", client_ip,
                     ntohs(client_addr.sin_port));

        uint64_t echo_count = 0;

        // Inner loop: echo messages for this client
        while (running.load(std::memory_order_relaxed)) {
            if (!detail_tcp::recv_exact(client_fd, buf.data(), config.msg_size)) {
                spdlog::info("tcp_echo_server: client disconnected after {} echoes",
                             echo_count);
                break;
            }

            // Embed server-side TSC timestamps
            if (config.msg_size >= 24) {
                uint64_t server_recv_tsc = eph::utils::TSC::now();
                std::memcpy(buf.data() + 8, &server_recv_tsc, 8);
                uint64_t server_send_tsc = eph::utils::TSC::now();
                std::memcpy(buf.data() + 16, &server_send_tsc, 8);
            }

            if (!detail_tcp::send_all(client_fd, buf.data(), config.msg_size)) {
                spdlog::error("tcp_echo_server: send failed");
                break;
            }
            ++echo_count;
        }

        ::close(client_fd);
    }

    ::close(server_fd);
    spdlog::info("tcp_echo_server: stopped");
}

// ── In-process launcher ──────────────────────────────────────────────────

inline MockHandle start_tcp_echo(const TcpEchoConfig& config, int cpu) {
    MockHandle h;
    auto run_flag = h.running;
    h.thread = std::thread([config, cpu, run_flag]() {
        if (auto r = eph::utils::set_thread_affinity(cpu, "tcp-echo"); !r) {
            spdlog::warn("tcp_echo_server: failed to pin to core {}: {}", cpu, r.error());
        } else {
            spdlog::info("tcp_echo_server: pinned to core {}", cpu);
        }
        run_tcp_echo_server(config, *run_flag);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    return h;
}

} // namespace bench::mock
