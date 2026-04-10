#pragma once

/// @file core/socket_io.hpp
/// Byte-level send / recv loops for kernel sockets.
///
/// Both the latency benchmarks and the DPDK E2E test mocks need exactly
/// the same primitive: "write all bytes" / "read exactly N bytes" with
/// EINTR retry, peer-close detection, and MSG_NOSIGNAL on the send side.
/// This header is the single canonical home for those helpers.
///
/// Header-only, no static state, lives in `bench` namespace alongside
/// `tcp_bind_listen` / `udp_bind` from socket_bind.hpp.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <sys/socket.h>

namespace bench {

/// Write the entire `len`-byte buffer to `fd`.
/// Retries on EINTR.  Returns false on any other error.
inline bool send_all_fd(int fd, const void* data, size_t len) noexcept {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, static_cast<const uint8_t*>(data) + sent,
                           len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

/// Read exactly `len` bytes from `fd` into `buf`.
/// Retries on EINTR.  Returns false on peer close (recv == 0) or error.
inline bool recv_exact_fd(int fd, void* buf, size_t len) noexcept {
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(fd, static_cast<uint8_t*>(buf) + got,
                           len - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false; // peer closed
        got += static_cast<size_t>(n);
    }
    return true;
}

} // namespace bench
