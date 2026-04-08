/// @file framework/tcp_io.hpp
/// TCP send/recv adapters for raw TCP scenarios.
///
/// Provides KernelTcpStream (kernel mode) and DpdkTcpStream (DPDK mode)
/// with the same duck-typed interface required by TcpEchoScenario:
///
///   bool send_all(const void* data, size_t n)
///   bool recv_exact(uint8_t* buf, size_t n)
///
/// Kernel mode: simple blocking sockets with TCP_NODELAY.
/// DPDK mode:  wraps eph::dpdk::TcpSession with helpers to fill the
/// fixed-size recv buffer from poll_rx() callbacks.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <sys/types.h>

#include <spdlog/spdlog.h>

#if defined(EPH_USE_DPDK)
#include "eph/dpdk/tcp.hpp"
#include "eph/utils/time.hpp"
#include "tsc_protocol.hpp"
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace bench {

#if !defined(EPH_USE_DPDK)

// ── Kernel TCP stream ────────────────────────────────────────────────────

class KernelTcpStream {
public:
    KernelTcpStream(const std::string& server_ip, uint16_t server_port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            spdlog::error("KernelTcpStream: socket() failed: {}", std::strerror(errno));
            return;
        }
        int opt = 1;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(server_port);
        ::inet_pton(AF_INET, server_ip.c_str(), &addr.sin_addr);

        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            spdlog::error("KernelTcpStream: connect() failed: {}", std::strerror(errno));
            ::close(fd_);
            fd_ = -1;
        }
    }

    ~KernelTcpStream() { if (fd_ >= 0) ::close(fd_); }

    KernelTcpStream(const KernelTcpStream&) = delete;
    KernelTcpStream& operator=(const KernelTcpStream&) = delete;

    bool valid() const noexcept { return fd_ >= 0; }

    bool send_all(const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        size_t sent = 0;
        while (sent < n) {
            ssize_t r = ::send(fd_, p + sent, n - sent, MSG_NOSIGNAL);
            if (r < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            sent += static_cast<size_t>(r);
        }
        return true;
    }

    bool recv_exact(uint8_t* buf, size_t n) {
        size_t total = 0;
        while (total < n) {
            ssize_t r = ::recv(fd_, buf + total, n - total, 0);
            if (r <= 0) return false;
            total += static_cast<size_t>(r);
        }
        return true;
    }

private:
    int fd_ = -1;
};

#else // EPH_USE_DPDK

// ── DPDK TCP stream wrapper ──────────────────────────────────────────────

/// Wrap eph::dpdk::TcpSession to provide synchronous send_all / recv_exact.
///
/// recv_exact polls poll_rx() and copies received bytes into the caller's
/// buffer until exactly `n` bytes have been collected, or the deadline
/// expires. The session handles TCP segment reassembly internally.
class DpdkTcpStream {
public:
    DpdkTcpStream(eph::dpdk::TcpSession<>& session, uint64_t timeout_cycles)
        : session_(session), timeout_cycles_(timeout_cycles) {}

    bool send_all(const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        size_t sent = 0;
        while (sent < n) {
            auto r = session_.send(p + sent, n - sent);
            if (!r) return false;
            sent += *r;
        }
        return true;
    }

    bool recv_exact(uint8_t* buf, size_t n) {
        size_t got = 0;
        const uint64_t deadline = eph::utils::TSC::now() + timeout_cycles_;
        while (got < n && eph::utils::TSC::now() < deadline) {
            auto r = session_.poll_rx([&](const uint8_t* data, uint16_t len) {
                size_t copy_n = std::min(static_cast<size_t>(len), n - got);
                std::memcpy(buf + got, data, copy_n);
                got += copy_n;
            });
            (void)r;
        }
        return got == n;
    }

private:
    eph::dpdk::TcpSession<>& session_;
    uint64_t timeout_cycles_;
};

#endif // EPH_USE_DPDK

} // namespace bench
