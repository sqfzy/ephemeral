/// @file mock/udp_relay_server.hpp
/// Header-only UDP relay server for latency benchmarks.
///
/// Receives UDP datagrams on listen_port, embeds relay-side TSC timestamps,
/// and forwards to forward_ip:forward_port. Simulates a store-and-forward
/// relay node (e.g. market data relay).
///
/// Timestamp protocol (all uint64_t little-endian):
///   [0:8]   client_send_tsc   — written by client (publisher), preserved
///   [8:16]  relay_recv_tsc    — written by relay on recvfrom
///   [16:24] relay_send_tsc    — written by relay just before sendto
///
/// Requires payload >= 24 bytes. The client measures:
///   - RTT:   client_send → client_recv (full relay round-trip)
///   - TX:    client_send → relay_recv  (client → relay)
///   - RX:    relay_send  → client_recv (relay → client)
///   - Relay: relay_recv  → relay_send  (relay processing)

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
#include "mock_handle.hpp"

namespace bench::mock {

// ── Configuration ────────────────────────────────────────────────────────

struct UdpRelayConfig {
    std::string bind_ip;            // required: listen address
    uint16_t listen_port = 9996;    // recv from client
    std::string forward_ip;         // required: forward destination
    uint16_t forward_port = 9995;   // send to client's recv port
};

// ── Core relay loop ─────────────────────────────────────────────────────

/// Run the UDP relay server. Blocks until `running` becomes false.
/// Receives datagrams, embeds relay TSC timestamps, forwards to the
/// configured destination.
inline void run_udp_relay_server(const UdpRelayConfig& config,
                                 std::atomic<bool>& running) {
    spdlog::info("udp_relay_server: starting on {}:{} → {}:{}",
                 config.bind_ip, config.listen_port,
                 config.forward_ip, config.forward_port);

    int sockfd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        spdlog::error("udp_relay_server: socket() failed: {}", std::strerror(errno));
        return;
    }

    int reuse = 1;
    ::setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config.listen_port);
    if (::inet_pton(AF_INET, config.bind_ip.c_str(), &addr.sin_addr) != 1) {
        spdlog::error("udp_relay_server: invalid bind IP \"{}\"", config.bind_ip);
        ::close(sockfd);
        return;
    }

    if (::bind(sockfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        spdlog::error("udp_relay_server: bind() failed on {}:{}: {}",
                      config.bind_ip, config.listen_port, std::strerror(errno));
        ::close(sockfd);
        return;
    }

    // Forward destination address
    sockaddr_in fwd_addr{};
    fwd_addr.sin_family = AF_INET;
    fwd_addr.sin_port = htons(config.forward_port);
    ::inet_pton(AF_INET, config.forward_ip.c_str(), &fwd_addr.sin_addr);

    timeval tv{.tv_sec = 1, .tv_usec = 0};
    ::setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    spdlog::info("udp_relay_server: listening on {}:{}", config.bind_ip, config.listen_port);

    uint8_t buf[2048];
    uint64_t relay_count = 0;

    while (running.load(std::memory_order_relaxed)) {
        ssize_t n = ::recvfrom(sockfd, buf, sizeof(buf), 0, nullptr, nullptr);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            spdlog::error("udp_relay_server: recvfrom() failed: {}", std::strerror(errno));
            break;
        }

        // Embed relay-side TSC timestamps
        if (n >= 24) {
            uint64_t relay_recv_tsc = eph::utils::TSC::now();
            std::memcpy(buf + 8, &relay_recv_tsc, 8);
            uint64_t relay_send_tsc = eph::utils::TSC::now();
            std::memcpy(buf + 16, &relay_send_tsc, 8);
        }

        // Forward to destination
        ::sendto(sockfd, buf, static_cast<size_t>(n), 0,
                 reinterpret_cast<const sockaddr*>(&fwd_addr), sizeof(fwd_addr));
        ++relay_count;
    }

    spdlog::info("udp_relay_server: stopped after {} relays", relay_count);
    ::close(sockfd);
}

// ── In-process launcher ──────────────────────────────────────────────────

inline MockHandle start_udp_relay(const UdpRelayConfig& config, int cpu) {
    MockHandle h;
    auto run_flag = h.running;
    h.thread = std::thread([config, cpu, run_flag]() {
        if (auto r = eph::utils::set_thread_affinity(cpu, "udp-relay"); !r) {
            spdlog::warn("udp_relay_server: failed to pin to core {}: {}", cpu, r.error());
        } else {
            spdlog::info("udp_relay_server: pinned to core {}", cpu);
        }
        run_udp_relay_server(config, *run_flag);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    return h;
}

} // namespace bench::mock
