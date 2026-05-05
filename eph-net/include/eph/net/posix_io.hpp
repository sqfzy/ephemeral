#pragma once

/// @file posix_io.hpp
/// Byte-level send / recv loops for kernel sockets.
///
/// Server-side mocks and test fixtures need exactly the same primitive:
/// "write all bytes" / "read exactly N bytes" with EINTR retry,
/// peer-close detection, and MSG_NOSIGNAL on the send side.  Pair
/// with posix_listener.hpp.
///
/// Originally lived under benchmarks/latency/core/socket_io.hpp;
/// promoted to eph-net so that tests can use it without
/// reverse-including the bench tree.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sys/socket.h>

#include <spdlog/spdlog.h>

namespace eph::net::posix {

/// Write the entire `len`-byte buffer to `fd`.
/// Retries on EINTR.  Returns false on any other error.
///
/// Errors (non-EINTR errno) and EOF-equivalent shortfalls are logged at
/// WARN with errno context — these helpers are used inside test mocks
/// and integration fixtures where a silent false return otherwise
/// surfaces only as a flaky timeout much later.
[[nodiscard]] inline bool send_all(int fd, const void* data, size_t len) noexcept {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, static_cast<const uint8_t*>(data) + sent,
                           len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            SPDLOG_WARN("posix::send_all: send() failed fd={} sent={}/{} errno={} ({})",
                        fd, sent, len, errno, std::strerror(errno));
            return false;
        }
        if (n == 0) [[unlikely]] {
            // POSIX: send() on stream sockets should never return 0 with
            // a non-zero len request. If it ever does (defensive against
            // a future kernel quirk or a misconfigured AF_UNIX SOCK_SEQPACKET
            // mock), treat it as a fatal stall — without this guard the
            // outer `while (sent < len)` loop spins forever on a 0-byte
            // return path (sent stays unchanged, len - sent stays the same).
            // Test mocks deserve the same actionable diagnostic the
            // production byte_socket.hpp::send already emits for the same
            // edge case.
            SPDLOG_WARN("posix::send_all: send() returned 0 fd={} sent={}/{} "
                        "(treating as stall to avoid infinite loop)",
                        fd, sent, len);
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

/// Read exactly `len` bytes from `fd` into `buf`.
/// Retries on EINTR.  Returns false on peer close (recv == 0) or error.
///
/// Both the peer-close and the errno-bearing failure paths are logged
/// at DEBUG and WARN respectively — DEBUG for peer close because in
/// test fixtures peer close is the *expected* terminator on a clean
/// teardown.
[[nodiscard]] inline bool recv_exact(int fd, void* buf, size_t len) noexcept {
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(fd, static_cast<uint8_t*>(buf) + got,
                           len - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            SPDLOG_WARN("posix::recv_exact: recv() failed fd={} got={}/{} errno={} ({})",
                        fd, got, len, errno, std::strerror(errno));
            return false;
        }
        if (n == 0) {
            SPDLOG_DEBUG("posix::recv_exact: peer closed fd={} got={}/{}",
                         fd, got, len);
            return false; // peer closed
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

} // namespace eph::net::posix
