/// @file framework/udp_io.hpp
/// UDP send/recv adapters for raw UDP scenarios.
///
/// Provides two pairs of (Sender, Receiver) adapters:
///
///   - KernelUdpSender / KernelUdpReceiver — for `bench_*.cpp` kernel mode
///   - DpdkUdpSender   / DpdkUdpReceiver   — for `bench_*_dpdk.cpp` mode
///
/// Both pairs satisfy the duck-typed interface required by
/// `bench::scenario::UdpEchoScenario` and `UdpRelayScenario`:
///
///   bool   Sender::send(const void* data, uint16_t len)
///   ssize_t Receiver::recv(uint8_t* buf, size_t buf_size)
///
/// The kernel adapters wrap a single `sockfd`. The DPDK adapters wrap
/// an `eph::dpdk::UdpSender` and a port_id + expected dst_port for
/// filtering RX packets.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <sys/types.h>

#include <spdlog/spdlog.h>

#if defined(EPH_USE_DPDK)
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include "eph/dpdk/net_header.hpp"
#include "eph/dpdk/udp.hpp"
#include "eph/utils/time.hpp"
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace bench {

#if !defined(EPH_USE_DPDK)

// ── Kernel UDP adapters ──────────────────────────────────────────────────

class KernelUdpSocket {
public:
    /// Create a UDP socket connected to `server_ip:server_port`.
    /// Sets a 100ms recv timeout (single dropped packet should not stall
    /// the measurement loop for seconds).
    KernelUdpSocket(const std::string& server_ip, uint16_t server_port) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) {
            spdlog::error("KernelUdpSocket: socket() failed: {}", std::strerror(errno));
            return;
        }
        timeval tv{.tv_sec = 0, .tv_usec = 100'000};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        server_addr_.sin_family = AF_INET;
        server_addr_.sin_port = htons(server_port);
        ::inet_pton(AF_INET, server_ip.c_str(), &server_addr_.sin_addr);
    }

    ~KernelUdpSocket() { if (fd_ >= 0) ::close(fd_); }

    KernelUdpSocket(const KernelUdpSocket&) = delete;
    KernelUdpSocket& operator=(const KernelUdpSocket&) = delete;

    bool valid() const noexcept { return fd_ >= 0; }
    int  fd() const noexcept { return fd_; }

    bool send(const void* data, uint16_t len) {
        ssize_t r = ::sendto(fd_, data, len, 0,
                             reinterpret_cast<sockaddr*>(&server_addr_),
                             sizeof(server_addr_));
        return r >= 0;
    }

    ssize_t recv(uint8_t* buf, size_t buf_size) {
        return ::recvfrom(fd_, buf, buf_size, 0, nullptr, nullptr);
    }

private:
    int fd_ = -1;
    sockaddr_in server_addr_{};
};

/// Two-socket UDP client for relay scenarios.
/// One socket sends to a target (relay's listen port).
/// One socket binds to a local recv port (where the relay forwards back to).
class KernelUdpRelayClient {
public:
    KernelUdpRelayClient(const std::string& relay_ip, uint16_t relay_listen_port,
                         uint16_t local_recv_port) {
        send_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        recv_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (send_fd_ < 0 || recv_fd_ < 0) {
            spdlog::error("KernelUdpRelayClient: socket() failed: {}", std::strerror(errno));
            return;
        }
        int reuse = 1;
        ::setsockopt(recv_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        // Bind recv_fd to INADDR_ANY (any iface in this netns)
        sockaddr_in recv_addr{};
        recv_addr.sin_family = AF_INET;
        recv_addr.sin_port = htons(local_recv_port);
        recv_addr.sin_addr.s_addr = INADDR_ANY;
        if (::bind(recv_fd_, reinterpret_cast<sockaddr*>(&recv_addr), sizeof(recv_addr)) < 0) {
            spdlog::error("KernelUdpRelayClient: bind() failed: {}", std::strerror(errno));
            ::close(send_fd_); ::close(recv_fd_);
            send_fd_ = recv_fd_ = -1;
            return;
        }
        timeval tv{.tv_sec = 0, .tv_usec = 100'000};
        ::setsockopt(recv_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // Configure send target
        relay_addr_.sin_family = AF_INET;
        relay_addr_.sin_port = htons(relay_listen_port);
        ::inet_pton(AF_INET, relay_ip.c_str(), &relay_addr_.sin_addr);
    }

    ~KernelUdpRelayClient() {
        if (send_fd_ >= 0) ::close(send_fd_);
        if (recv_fd_ >= 0) ::close(recv_fd_);
    }

    KernelUdpRelayClient(const KernelUdpRelayClient&) = delete;
    KernelUdpRelayClient& operator=(const KernelUdpRelayClient&) = delete;

    bool valid() const noexcept { return send_fd_ >= 0 && recv_fd_ >= 0; }

    bool send(const void* data, uint16_t len) {
        ssize_t r = ::sendto(send_fd_, data, len, 0,
                             reinterpret_cast<sockaddr*>(&relay_addr_),
                             sizeof(relay_addr_));
        return r >= 0;
    }

    ssize_t recv(uint8_t* buf, size_t buf_size) {
        return ::recvfrom(recv_fd_, buf, buf_size, 0, nullptr, nullptr);
    }

private:
    int send_fd_ = -1;
    int recv_fd_ = -1;
    sockaddr_in relay_addr_{};
};

#else // EPH_USE_DPDK

// ── DPDK UDP adapters ────────────────────────────────────────────────────

/// Wrap an eph::dpdk::UdpSender for the Sender concept.
class DpdkUdpSenderAdapter {
public:
    explicit DpdkUdpSenderAdapter(eph::dpdk::UdpSender& sender) : sender_(sender) {}

    bool send(const void* data, uint16_t len) {
        return sender_.send(data, len);
    }

private:
    eph::dpdk::UdpSender& sender_;
};

/// Polls rte_eth_rx_burst until a packet matching `expected_dst_port`
/// arrives or the timeout expires. Copies payload into the caller's
/// buffer and returns its length.
class DpdkUdpReceiver {
public:
    DpdkUdpReceiver(uint16_t port_id, uint16_t queue_id,
                    uint16_t expected_dst_port, uint64_t timeout_cycles)
        : port_id_(port_id)
        , queue_id_(queue_id)
        , expected_dst_port_(expected_dst_port)
        , timeout_cycles_(timeout_cycles) {}

    /// Returns bytes copied to buf (>= 0), or -1 on timeout / error.
    ssize_t recv(uint8_t* buf, size_t buf_size) {
        const uint64_t deadline = eph::utils::TSC::now() + timeout_cycles_;
        rte_mbuf* pkts[32];
        while (eph::utils::TSC::now() < deadline) {
            uint16_t nb_rx = rte_eth_rx_burst(port_id_, queue_id_, pkts, 32);
            for (uint16_t i = 0; i < nb_rx; ++i) {
                auto parsed = eph::dpdk::net::parse_udp_packet(pkts[i]);
                if (parsed && parsed.dst_port() == expected_dst_port_) {
                    size_t n = std::min(static_cast<size_t>(parsed.payload_len), buf_size);
                    std::memcpy(buf, parsed.payload, n);
                    rte_pktmbuf_free(pkts[i]);
                    // Free remaining
                    for (uint16_t j = i + 1; j < nb_rx; ++j) {
                        rte_pktmbuf_free(pkts[j]);
                    }
                    return static_cast<ssize_t>(n);
                }
                rte_pktmbuf_free(pkts[i]);
            }
        }
        return -1;
    }

private:
    uint16_t port_id_;
    uint16_t queue_id_;
    uint16_t expected_dst_port_;
    uint64_t timeout_cycles_;
};

#endif // EPH_USE_DPDK

} // namespace bench
