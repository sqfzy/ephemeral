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
/// History: ByteSocket grew out of (a minimal subset of) the pre-v3.3
/// `eph::net::SocketTransport` connect/send/recv loop, deliberately
/// stripped of the timestamping / histogram baggage that was tied to
/// the retired `TcpTransport` concept. Today this is just a small
/// non-blocking-fd RAII helper; the production transport surface is
/// `eph::net::kernel::KernelTcpStream` (this header is `detail/`).

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
// Tunables (compile-time constants, no config field to keep the byte layer
// free of StreamConfig knowledge).
// ---------------------------------------------------------------------------

/// @brief Upper bound on `::poll(2)` waits for writability inside
///        `ByteSocket::send` when the kernel returns EAGAIN/EWOULDBLOCK.
///
/// Previously hard-coded as `1000` inline — see batch3-round1 HIGH-2. An
/// HFT-grade default is much tighter than the 1-second value the legacy
/// SocketTransport inherited; 20 ms is still generous relative to typical
/// loopback/LAN backpressure clearance but bounds tail-latency impact when
/// the kernel socket send queue saturates. Callers who need a different
/// budget should drive the write loop from the higher-level Stream layer
/// (which owns the application timeout) rather than tuning this constant.
inline constexpr int kSendBackpressurePollMs = 20;

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
    connect(const SocketAddr& addr, std::chrono::milliseconds timeout,
            const SocketAddr& local = SocketAddr{}) noexcept {
        [[maybe_unused]] auto* log = byte_socket_logger();
        SPDLOG_LOGGER_DEBUG(log,
            "ByteSocket::connect entry: addr={} local={} timeout_ms={}",
            addr.to_string(), local.to_string(), timeout.count());

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
        // Previously the return value was silently discarded, which is a
        // latency footgun: if setsockopt fails (EOPNOTSUPP on an unusual
        // socket family, seccomp block, etc.) Nagle stays on and every
        // small send pays the coalescing penalty. Continue on failure so
        // an unusual environment doesn't become unusable, but WARN-log
        // the errno so operators can notice.
        int one = 1;
        if (::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0) {
            SPDLOG_LOGGER_WARN(log,
                "ByteSocket::connect: setsockopt(TCP_NODELAY) failed: "
                "errno={} ({}) — Nagle may remain enabled",
                errno, std::strerror(errno));
        }

        // Optional explicit local bind. The default-constructed SocketAddr
        // has port=0 and ip=0.0.0.0, which is the no-op case (kernel picks
        // both, same as if bind() were never called). Any non-zero field
        // means the caller wants to pin part of the 5-tuple — typically the
        // source port, so a paris-traceroute companion probe can hash into
        // the same ECMP bucket. We do NOT set SO_REUSEADDR: this is a
        // client-only socket, and a stale TIME_WAIT on the same 4-tuple is
        // a real conflict the caller should learn about, not paper over.
        if (local.port != 0 || local.ip.to_be32() != 0) {
            struct sockaddr_in lsa{};
            lsa.sin_family      = AF_INET;
            lsa.sin_port        = ::htons(local.port);
            lsa.sin_addr.s_addr = ::htonl(local.ip.to_be32());
            if (::bind(s, reinterpret_cast<struct sockaddr*>(&lsa),
                       sizeof(lsa)) != 0) {
                const int err = errno;
                ::close(s);
                SPDLOG_LOGGER_WARN(log,
                    "ByteSocket::connect: bind({}) failed: errno={} ({})",
                    local.to_string(), err, std::strerror(err));
                return std::unexpected(core::ErrorInfo{
                    core::Error::ConnectFailed,
                    "ByteSocket::connect: bind() failed"});
            }
        }

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

    /// @brief Send all of `wire_bytes` via one or more ::send() calls.
    ///
    /// `wire_bytes` are whatever the caller wants to put on the socket —
    /// the ByteSocket layer is payload-agnostic (plaintext on a plain
    /// TCP stream; ciphertext when the Stream layer above is wrapped in
    /// TLS). Retries on EAGAIN/EWOULDBLOCK via poll() with a short
    /// internal budget. Returns bytes written on success, or typed error.
    ///
    /// Partial-send contract on error: when the loop fails AFTER one or
    /// more `::send()` calls already deposited bytes into the kernel TX
    /// buffer, those bytes ARE on the wire — the syscall succeeded for
    /// them. The function still returns `unexpected(...)` because the
    /// REST of the buffer is undelivered, and the typed error doesn't
    /// carry a partial-progress count. Implications for the caller:
    ///   * **TLS streams**: the upper layer (`KernelTcpStream::send`) sees
    ///     this as opaque and latches `tls_corrupt_` so the next call
    ///     surfaces `Disconnected` — correct, because the AEAD seq has
    ///     already advanced past what the wire saw.
    ///   * **Plaintext streams**: a naive retry of the same `wire_bytes`
    ///     will re-send the prefix that was already delivered, causing
    ///     application-level duplication. Callers that need at-most-once
    ///     semantics on top of plaintext TCP must either tear down the
    ///     connection on every error here or implement their own
    ///     framing-with-progress (out of scope for this layer).
    /// The WARN/INFO/ERROR logs below report `sent` so operators can see
    /// when the wire-side prefix is non-empty.
    [[nodiscard]] std::expected<std::size_t, core::ErrorInfo>
    send(std::span<const uint8_t> wire_bytes) noexcept {
        if (fd_ < 0) {
            // WARN-log: caller invoked send() on a closed socket. A caller
            // that legitimately drops the expected<> would otherwise have
            // no signal that the send was a no-op — surfaces a programmer
            // error that's catastrophic on the order path.
            SPDLOG_LOGGER_WARN(byte_socket_logger(),
                "ByteSocket::send: fd closed (bytes={})", wire_bytes.size());
            return std::unexpected(core::ErrorInfo{
                core::Error::Disconnected,
                "ByteSocket::send: fd closed"});
        }
        if (wire_bytes.empty()) return std::size_t{0};

        std::size_t   sent = 0;
        const uint8_t* ptr = wire_bytes.data();
        std::size_t remain = wire_bytes.size();

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
                // Wait up to `kSendBackpressurePollMs` for writability. The
                // Stream layer (KernelTcpStream::send / close_gracefully)
                // owns higher-level timeouts — this is only the inner
                // backpressure bounce.
                struct pollfd pfd{};
                pfd.fd     = fd_;
                pfd.events = POLLOUT;
                const int pr = ::poll(&pfd, 1, kSendBackpressurePollMs);
                if (pr <= 0) {
                    // Actionable context: fd, remaining bytes, wait budget.
                    // Operators need this to distinguish a one-off spike
                    // from a systematic backpressure problem.
                    SPDLOG_LOGGER_WARN(byte_socket_logger(),
                        "ByteSocket::send: poll for writable failed "
                        "fd={} sent={} remain={} budget_ms={} pr={} errno={} ({})",
                        fd_, sent, remain, kSendBackpressurePollMs, pr,
                        (pr < 0 ? errno : 0),
                        (pr < 0 ? std::strerror(errno) : "timeout"));
                    return std::unexpected(core::ErrorInfo{
                        core::Error::Timeout,
                        "ByteSocket::send: poll writable timeout"});
                }
                continue;
            }
            if (errno == EPIPE || errno == ECONNRESET) {
                // Include sent / remain so operators can tell whether
                // any bytes already escaped to the wire before the peer
                // closed — see the partial-send contract docstring above.
                SPDLOG_LOGGER_INFO(byte_socket_logger(),
                    "ByteSocket::send: peer closed ({}) "
                    "fd={} sent={} remain={}",
                    std::strerror(errno), fd_, sent, remain);
                return std::unexpected(core::ErrorInfo{
                    core::Error::Disconnected,
                    "ByteSocket::send: peer closed"});
            }
            SPDLOG_LOGGER_ERROR(byte_socket_logger(),
                "ByteSocket::send: unexpected errno {} ({}) "
                "fd={} sent={} remain={}",
                errno, std::strerror(errno), fd_, sent, remain);
            return std::unexpected(core::ErrorInfo{
                core::Error::Disconnected,
                "ByteSocket::send: unexpected I/O error"});
        }
        return sent;
    }

    /// @brief Non-blocking recv of up to `cap` bytes into `wire_out`.
    ///
    /// `wire_out` is the destination buffer the syscall writes into — the
    /// bytes are raw wire bytes (plaintext on a plain stream; ciphertext
    /// when the Stream layer above is wrapped in TLS). The caller's
    /// upper layer decides how to interpret them.
    ///
    /// Return values:
    ///   - Ok(n), n > 0  : `n` bytes read into `wire_out`
    ///   - Ok(0)         : **not** used (ambiguous); instead WouldBlock below
    ///   - Err(WouldBlock): no data available right now
    ///   - Err(Disconnected): peer FIN (recv returned 0) or RST
    ///   - Err(...)       : other I/O error
    [[nodiscard]] std::expected<std::size_t, core::ErrorInfo>
    recv(uint8_t* wire_out, std::size_t cap) noexcept {
        if (fd_ < 0) {
            // WARN-log: pattern matches send() above — same rationale.
            SPDLOG_LOGGER_WARN(byte_socket_logger(),
                "ByteSocket::recv: fd closed (cap={})", cap);
            return std::unexpected(core::ErrorInfo{
                core::Error::Disconnected,
                "ByteSocket::recv: fd closed"});
        }
        if (cap == 0) return std::size_t{0};

        for (;;) {
            const ssize_t n = ::recv(fd_, wire_out, cap, MSG_DONTWAIT);
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
            // WARN-log: setting Nagle on a closed fd is a programmer error;
            // matches the existing setsockopt-failure WARN below.
            SPDLOG_LOGGER_WARN(byte_socket_logger(),
                "ByteSocket::set_no_delay: fd closed (enable={})", enable);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "ByteSocket::set_no_delay: fd closed"});
        }
        int v = enable ? 1 : 0;
        if (::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v)) != 0) {
            // WARN-log here as well so a caller who (legitimately) drops
            // the expected<> return still sees evidence in the log stream
            // that the option took effect — this matters in HFT because
            // silent Nagle-on is catastrophic for tail latency.
            SPDLOG_LOGGER_WARN(byte_socket_logger(),
                "ByteSocket::set_no_delay: setsockopt errno={} ({}) enable={}",
                errno, std::strerror(errno), enable);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "ByteSocket::set_no_delay: setsockopt failed"});
        }
        return {};
    }

    /// @brief Enable TCP keepalive on the open fd.
    ///
    /// Wires `SO_KEEPALIVE` + `TCP_KEEPIDLE` + `TCP_KEEPINTVL` + `TCP_KEEPCNT`
    /// in one shot. `interval_secs` drives both the idle-before-first-probe
    /// time and the inter-probe gap (the same pair the DPDK PMD honours
    /// in its TCP session machinery — keeping the surface uniform across
    /// backends). Caller is responsible for clamping `interval_secs` to a
    /// positive value before calling; passing 0 is treated as a no-op
    /// (returns success without issuing any setsockopt).
    [[nodiscard]] std::expected<void, core::ErrorInfo>
    set_keepalive(int interval_secs, uint8_t probes) noexcept {
        if (fd_ < 0) {
            // WARN-log: keepalive on a closed fd is a programmer error;
            // matches set_no_delay() pattern.
            SPDLOG_LOGGER_WARN(byte_socket_logger(),
                "ByteSocket::set_keepalive: fd closed "
                "(interval_secs={} probes={})", interval_secs, probes);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "ByteSocket::set_keepalive: fd closed"});
        }
        if (interval_secs <= 0) {
            return {}; // disabled — no syscall
        }
        // Validate `probes` BEFORE touching any socket option. Linux
        // setsockopt(TCP_KEEPCNT) rejects probes==0 with EINVAL, but
        // by then SO_KEEPALIVE / TCP_KEEPIDLE / TCP_KEEPINTVL have
        // already succeeded — the socket would be left in a half-
        // configured state (keepalive enabled, but probe count still
        // at the system default of 9). Reject up-front so an invalid
        // call cannot leak partial state into the kernel. The upper
        // layer KeepaliveConfig::validate() already enforces the same
        // [1,10] bound, but this lower-layer API is reachable directly
        // from tests and future callers, so it must defend itself.
        if (probes == 0) {
            SPDLOG_LOGGER_WARN(byte_socket_logger(),
                "ByteSocket::set_keepalive: probes=0 rejected — "
                "setsockopt(TCP_KEEPCNT, 0) is EINVAL on Linux and "
                "would leave keepalive partially configured");
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "ByteSocket::set_keepalive: probes must be > 0 "
                "when interval_secs > 0"});
        }
        int one = 1;
        if (::setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE,
                         &one, sizeof(one)) != 0) {
            SPDLOG_LOGGER_WARN(byte_socket_logger(),
                "ByteSocket::set_keepalive: SO_KEEPALIVE errno={} ({})",
                errno, std::strerror(errno));
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "ByteSocket::set_keepalive: SO_KEEPALIVE failed"});
        }
        if (::setsockopt(fd_, IPPROTO_TCP, TCP_KEEPIDLE,
                         &interval_secs, sizeof(interval_secs)) != 0) {
            SPDLOG_LOGGER_WARN(byte_socket_logger(),
                "ByteSocket::set_keepalive: TCP_KEEPIDLE errno={} ({}) "
                "interval_secs={}",
                errno, std::strerror(errno), interval_secs);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "ByteSocket::set_keepalive: TCP_KEEPIDLE failed"});
        }
        if (::setsockopt(fd_, IPPROTO_TCP, TCP_KEEPINTVL,
                         &interval_secs, sizeof(interval_secs)) != 0) {
            SPDLOG_LOGGER_WARN(byte_socket_logger(),
                "ByteSocket::set_keepalive: TCP_KEEPINTVL errno={} ({}) "
                "interval_secs={}",
                errno, std::strerror(errno), interval_secs);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "ByteSocket::set_keepalive: TCP_KEEPINTVL failed"});
        }
        int p = static_cast<int>(probes);
        if (::setsockopt(fd_, IPPROTO_TCP, TCP_KEEPCNT,
                         &p, sizeof(p)) != 0) {
            SPDLOG_LOGGER_WARN(byte_socket_logger(),
                "ByteSocket::set_keepalive: TCP_KEEPCNT errno={} ({}) "
                "probes={}",
                errno, std::strerror(errno), probes);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "ByteSocket::set_keepalive: TCP_KEEPCNT failed"});
        }
        SPDLOG_LOGGER_DEBUG(byte_socket_logger(),
            "ByteSocket::set_keepalive: fd={} interval={}s probes={}",
            fd_, interval_secs, probes);
        return {};
    }

private:
    int fd_{-1};
};

} // namespace eph::net::kernel::detail
