#pragma once

/// @file byte_socket.hpp
/// RAII wrapper around a non-blocking AF_INET SOCK_STREAM fd.
///
/// This is the byte-pipe
/// layer that `KernelTcpStream` wraps. It deliberately contains **no** TLS,
/// WebSocket, framing or reconnect logic — those sit above in the Stream
/// layer. What it does provide:
///
///   - non-blocking connect with a monotonic-deadline timeout
///   - blocking-free send / recv returning typed errors via std::expected
///   - TCP_NODELAY by default (HFT latency bias)
///   - SO_REUSEADDR for connect(...) is not set — this is a client-only socket
///   - move-only, close-on-destruct
///
/// Design rationale:
///   - Not header-only bloat: the whole thing is ~150 lines and inlines into
///     `KernelTcpStream` at -O2 so there is no indirection cost.
///   - Uses std::expected<..., ErrorInfo> everywhere per the project's
///     fallible-API convention.
///   - No exceptions (SPDLOG_NO_EXCEPTIONS is defined in tests).
///
/// The legacy `eph::net::SocketTransport` already has ~600 lines of hardened
/// connect/send/recv logic. We intentionally re-implement a minimal subset
/// here rather than wrapping it: the legacy class is tied to the v2.x
/// `TcpTransport` concept and carries timestamping / histogram baggage that
/// belongs at a higher layer.

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "eph/core/detail/logger.hpp"
#include "eph/core/error.hpp"
#include "eph/net/socket_addr.hpp"

namespace eph::net::kernel::detail {

// ---------------------------------------------------------------------------
// Logger
// ---------------------------------------------------------------------------

/// @brief Lazily-initialized logger for the kernel byte-socket subsystem.
inline spdlog::logger* byte_socket_logger() {
    static auto* l = ::eph::core::detail::make_logger("net.kernel.byte_socket");
    return l;
}

// ---------------------------------------------------------------------------
// ByteSocket — non-blocking AF_INET SOCK_STREAM fd
// ---------------------------------------------------------------------------

/// @brief Move-only RAII wrapper around a non-blocking TCP socket fd.
///
/// Every method returns `std::expected<T, eph::core::ErrorInfo>` so that
/// failures carry a typed error code plus a static-lifetime detail string.
/// The class holds no codec / TLS / framing state — it is a pure byte pipe.
class ByteSocket {
public:
    /// @brief Default-construct a closed socket (no fd allocated yet).
    ByteSocket() noexcept = default;

    ~ByteSocket() { close(); }

    ByteSocket(const ByteSocket&)            = delete;
    ByteSocket& operator=(const ByteSocket&) = delete;

    ByteSocket(ByteSocket&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)) {}

    ByteSocket& operator=(ByteSocket&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────

    /// @brief Open a non-blocking TCP socket and connect to `addr` with a
    ///        bounded timeout.
    ///
    /// Uses a monotonic deadline so signal storms (EINTR) cannot extend the
    /// connect budget beyond `timeout`. TCP_NODELAY is set before connect().
    ///
    /// Returns:
    ///   - Ok on successful connect
    ///   - Err(ConnectFailed) on socket()/connect()/getsockopt() failure
    ///   - Err(Timeout) when the deadline expires before completion
    [[nodiscard]] std::expected<void, core::ErrorInfo>
    connect(const SocketAddr& addr, std::chrono::milliseconds timeout) noexcept {
        [[maybe_unused]] auto* log = byte_socket_logger();
        SPDLOG_LOGGER_DEBUG(log,
            "ByteSocket::connect entry: addr={} timeout_ms={}",
            addr.to_string(), timeout.count());

        if (fd_ >= 0) {
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "ByteSocket::connect: already open"});
        }

        // SOCK_NONBLOCK | SOCK_CLOEXEC so fork()/exec() in another thread does
        // not leak this fd, and we never block on connect().
        const int s = ::socket(AF_INET,
                               SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                               IPPROTO_TCP);
        if (s < 0) {
            SPDLOG_LOGGER_ERROR(log, "ByteSocket::connect: socket() failed: {}",
                                std::strerror(errno));
            return std::unexpected(core::ErrorInfo{
                core::Error::ConnectFailed,
                "ByteSocket::connect: socket() failed"});
        }

        // TCP_NODELAY by default — HFT bias, matches legacy SocketTransport.
        int one = 1;
        (void)::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        struct sockaddr_in sa{};
        sa.sin_family      = AF_INET;
        sa.sin_port        = ::htons(addr.port);
        sa.sin_addr.s_addr = ::htonl(addr.ip.to_be32());

        const auto start    = std::chrono::steady_clock::now();
        const auto deadline = start + timeout;

        int rc = ::connect(s, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa));
        if (rc == 0) {
            // Loopback frequently completes synchronously.
            fd_ = s;
            SPDLOG_LOGGER_DEBUG(log,
                "ByteSocket::connect: immediate success fd={}", fd_);
            return {};
        }
        if (errno != EINPROGRESS) {
            const int err = errno;
            ::close(s);
            SPDLOG_LOGGER_ERROR(log,
                "ByteSocket::connect: ::connect() failed: {}", std::strerror(err));
            return std::unexpected(core::ErrorInfo{
                core::Error::ConnectFailed,
                "ByteSocket::connect: ::connect() failed"});
        }

        // Wait until writable or deadline, retrying on EINTR.
        for (;;) {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                ::close(s);
                SPDLOG_LOGGER_WARN(log,
                    "ByteSocket::connect: deadline expired ({}ms)",
                    timeout.count());
                return std::unexpected(core::ErrorInfo{
                    core::Error::Timeout,
                    "ByteSocket::connect: deadline expired"});
            }
            const auto remain_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - now).count();

            struct pollfd pfd{};
            pfd.fd     = s;
            pfd.events = POLLOUT;
            const int pr = ::poll(&pfd, 1, static_cast<int>(remain_ms));
            if (pr < 0 && errno == EINTR) continue;
            if (pr <= 0) {
                ::close(s);
                SPDLOG_LOGGER_WARN(log,
                    "ByteSocket::connect: poll timeout/error pr={}", pr);
                return std::unexpected(core::ErrorInfo{
                    core::Error::Timeout,
                    "ByteSocket::connect: poll timed out"});
            }
            break;
        }

        // Check SO_ERROR to distinguish success from async failure.
        int so_err = 0;
        socklen_t so_len = sizeof(so_err);
        if (::getsockopt(s, SOL_SOCKET, SO_ERROR, &so_err, &so_len) != 0) {
            ::close(s);
            SPDLOG_LOGGER_ERROR(log,
                "ByteSocket::connect: getsockopt(SO_ERROR) failed: {}",
                std::strerror(errno));
            return std::unexpected(core::ErrorInfo{
                core::Error::ConnectFailed,
                "ByteSocket::connect: getsockopt(SO_ERROR) failed"});
        }
        if (so_err != 0) {
            ::close(s);
            SPDLOG_LOGGER_WARN(log,
                "ByteSocket::connect: async connect failed: {}",
                std::strerror(so_err));
            return std::unexpected(core::ErrorInfo{
                core::Error::ConnectFailed,
                "ByteSocket::connect: SO_ERROR reported failure"});
        }

        fd_ = s;
        SPDLOG_LOGGER_DEBUG(log,
            "ByteSocket::connect: success fd={} addr={}", fd_, addr.to_string());
        return {};
    }

    /// @brief Close the fd if open. Idempotent.
    void close() noexcept {
        if (fd_ >= 0) {
            SPDLOG_LOGGER_TRACE(byte_socket_logger(),
                "ByteSocket::close fd={}", fd_);
            ::close(fd_);
            fd_ = -1;
        }
    }

    /// @brief Equivalent to `close()` — provided for API symmetry with the
    ///        higher-level `KernelTcpStream::close_gracefully`.
    void reset() noexcept { close(); }

    // ── I/O ───────────────────────────────────────────────────────────────

    /// @brief Send all of `data` via one or more ::send() calls.
    ///
    /// Retries on EAGAIN/EWOULDBLOCK via poll() with a short internal
    /// budget. Returns bytes written on success, or a typed error.
    [[nodiscard]] std::expected<std::size_t, core::ErrorInfo>
    send(std::span<const uint8_t> data) noexcept {
        if (fd_ < 0) {
            return std::unexpected(core::ErrorInfo{
                core::Error::Disconnected,
                "ByteSocket::send: fd closed"});
        }
        if (data.empty()) return std::size_t{0};

        std::size_t   sent = 0;
        const uint8_t* ptr = data.data();
        std::size_t remain = data.size();

        while (remain > 0) {
            const ssize_t n = ::send(fd_, ptr, remain, MSG_NOSIGNAL);
            if (n > 0) {
                ptr    += n;
                sent   += static_cast<std::size_t>(n);
                remain -= static_cast<std::size_t>(n);
                continue;
            }
            if (n == 0) {
                // POSIX: send() never returns 0 for stream sockets, but be
                // defensive.
                SPDLOG_LOGGER_WARN(byte_socket_logger(),
                    "ByteSocket::send: unexpected zero return");
                return std::unexpected(core::ErrorInfo{
                    core::Error::Disconnected,
                    "ByteSocket::send: zero return"});
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Wait up to 1s for writability. The Stream layer handles
                // higher-level timeouts.
                struct pollfd pfd{};
                pfd.fd     = fd_;
                pfd.events = POLLOUT;
                const int pr = ::poll(&pfd, 1, 1000);
                if (pr <= 0) {
                    SPDLOG_LOGGER_WARN(byte_socket_logger(),
                        "ByteSocket::send: poll for writable failed pr={}",
                        pr);
                    return std::unexpected(core::ErrorInfo{
                        core::Error::Timeout,
                        "ByteSocket::send: poll writable timeout"});
                }
                continue;
            }
            if (errno == EPIPE || errno == ECONNRESET) {
                SPDLOG_LOGGER_INFO(byte_socket_logger(),
                    "ByteSocket::send: peer closed ({})", std::strerror(errno));
                return std::unexpected(core::ErrorInfo{
                    core::Error::Disconnected,
                    "ByteSocket::send: peer closed"});
            }
            SPDLOG_LOGGER_ERROR(byte_socket_logger(),
                "ByteSocket::send: unexpected errno {} ({})",
                errno, std::strerror(errno));
            return std::unexpected(core::ErrorInfo{
                core::Error::Disconnected,
                "ByteSocket::send: unexpected I/O error"});
        }
        return sent;
    }

    /// @brief Non-blocking recv of up to `cap` bytes into `buf`.
    ///
    /// Return values:
    ///   - Ok(n), n > 0  : `n` bytes read
    ///   - Ok(0)         : **not** used (ambiguous); instead WouldBlock below
    ///   - Err(WouldBlock): no data available right now
    ///   - Err(Disconnected): peer FIN (recv returned 0) or RST
    ///   - Err(...)       : other I/O error
    [[nodiscard]] std::expected<std::size_t, core::ErrorInfo>
    recv(uint8_t* buf, std::size_t cap) noexcept {
        if (fd_ < 0) {
            return std::unexpected(core::ErrorInfo{
                core::Error::Disconnected,
                "ByteSocket::recv: fd closed"});
        }
        if (cap == 0) return std::size_t{0};

        for (;;) {
            const ssize_t n = ::recv(fd_, buf, cap, MSG_DONTWAIT);
            if (n > 0) {
                return static_cast<std::size_t>(n);
            }
            if (n == 0) {
                SPDLOG_LOGGER_DEBUG(byte_socket_logger(),
                    "ByteSocket::recv: peer FIN");
                return std::unexpected(core::ErrorInfo{
                    core::Error::Disconnected,
                    "ByteSocket::recv: peer closed (FIN)"});
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return std::unexpected(core::ErrorInfo{
                    core::Error::WouldBlock,
                    "ByteSocket::recv: would block"});
            }
            if (errno == ECONNRESET) {
                SPDLOG_LOGGER_INFO(byte_socket_logger(),
                    "ByteSocket::recv: RST from peer");
                return std::unexpected(core::ErrorInfo{
                    core::Error::Disconnected,
                    "ByteSocket::recv: peer RST"});
            }
            SPDLOG_LOGGER_ERROR(byte_socket_logger(),
                "ByteSocket::recv: errno {} ({})",
                errno, std::strerror(errno));
            return std::unexpected(core::ErrorInfo{
                core::Error::Disconnected,
                "ByteSocket::recv: unexpected I/O error"});
        }
    }

    // ── Accessors ─────────────────────────────────────────────────────────

    [[nodiscard]] int  fd() const noexcept { return fd_; }
    [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }

    /// @brief Enable / disable TCP_NODELAY on the open fd.
    [[nodiscard]] std::expected<void, core::ErrorInfo> set_no_delay(bool enable) noexcept {
        if (fd_ < 0) {
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "ByteSocket::set_no_delay: fd closed"});
        }
        int v = enable ? 1 : 0;
        if (::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v)) != 0) {
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "ByteSocket::set_no_delay: setsockopt failed"});
        }
        return {};
    }

private:
    int fd_{-1};
};

} // namespace eph::net::kernel::detail
