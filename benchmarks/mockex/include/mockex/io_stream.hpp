/// @file mockex/io_stream.hpp
/// IoStream adapters used by mockex scenario handlers to operate on
/// plain TCP fds or TLS-wrapped fds uniformly via templates.
///
/// Two adapters with the same interface:
///   - `RawFdStream` — thin wrapper over a BSD-socket fd
///   - `mockex::tls::TlsConn` — already exposes `read_exact` / `write_all`
///
/// Scenario handlers (tcp_echo / ws_server / …) become
/// `template <class IoStream> void handle(IoStream& io, …)` and call
/// `io.read_exact(...)` / `io.write_all(...)` without caring whether
/// they're talking to plain TCP or TLS underneath.
#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>

#include <sys/socket.h>
#include <unistd.h>

#include "eph/net/posix_io.hpp"  // recv_exact / send_all

namespace mockex::io {

/// Adapter wrapping a plain BSD-socket fd. Move-only; closes the fd
/// on destruction (mirrors `mockex::tls::TlsConn`).
class RawFdStream {
public:
    RawFdStream() noexcept = default;
    explicit RawFdStream(int fd) noexcept : fd_(fd) {}

    RawFdStream(const RawFdStream&) = delete;
    RawFdStream& operator=(const RawFdStream&) = delete;

    RawFdStream(RawFdStream&& other) noexcept
        : fd_(other.fd_) { other.fd_ = -1; }

    RawFdStream& operator=(RawFdStream&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    ~RawFdStream() noexcept { close(); }

    [[nodiscard]] int fd() const noexcept { return fd_; }

    [[nodiscard]] bool read_exact(void* buf, std::size_t n) noexcept {
        return eph::net::posix::recv_exact(fd_, buf, n);
    }

    [[nodiscard]] bool write_all(const void* buf, std::size_t n) noexcept {
        return eph::net::posix::send_all(fd_, buf, n);
    }

    /// `read(2)`-like single-shot read: blocks until ≥1 byte arrives
    /// (or peer closes / EINTR retried). Returns bytes read (>0),
    /// 0 on clean peer close, -1 on error.
    [[nodiscard]] ssize_t read_some(void* buf, std::size_t n) noexcept {
        for (;;) {
            const ssize_t r = ::recv(fd_, buf, n, 0);
            if (r >= 0) return r;
            if (errno == EINTR) continue;
            return -1;
        }
    }

    /// Release ownership (caller takes responsibility for closing).
    [[nodiscard]] int release() noexcept {
        const int f = fd_;
        fd_ = -1;
        return f;
    }

    void close() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

} // namespace mockex::io
