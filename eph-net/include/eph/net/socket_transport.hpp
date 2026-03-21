#pragma once

/// @file socket_transport.hpp
/// POSIX socket TCP transport — satisfies TcpTransport concept for non-DPDK use.
///
/// Uses non-blocking sockets with poll() for I/O multiplexing.
/// Designed for generic WebSocket clients that don't need DPDK's
/// kernel-bypass latency but want the same Transport<> API.

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <optional>
#include <string>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "eph/net/tcp_concept.hpp"

namespace eph::net {

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

struct SocketConfig {
    std::string host{};
    uint16_t    port = 0;
    bool        tcp_nodelay = true;   // Disable Nagle for lower latency
    int         recv_buf_size = 0;    // 0 = OS default
    int         send_buf_size = 0;    // 0 = OS default
    bool        tcp_keepalive = false;   // Enable TCP keepalive probes
    int         keepalive_idle = 60;     // Seconds before first probe (TCP_KEEPIDLE)
    int         keepalive_interval = 10; // Seconds between probes (TCP_KEEPINTVL)
    int         keepalive_count = 3;     // Probes before declaring dead (TCP_KEEPCNT)
};

// ─────────────────────────────────────────────────────────────────────────────
// Logger
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

inline std::shared_ptr<spdlog::logger> socket_logger() {
    static auto l = [] {
        auto lg = spdlog::get("net.socket");
        if (!lg) lg = spdlog::stdout_color_mt("net.socket");
        return lg;
    }();
    return l;
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// SocketTransport
// ─────────────────────────────────────────────────────────────────────────────

/// POSIX socket-based TCP transport satisfying the TcpTransport concept.
///
/// Provides blocking connect with timeout, non-blocking send/recv,
/// and graceful/forced close. Suitable for generic (non-DPDK) usage
/// of the Transport<> template.
class SocketTransport {
public:
    explicit SocketTransport(const SocketConfig& config) noexcept
        : config_(config) {
        SPDLOG_LOGGER_DEBUG(detail::socket_logger(),
            "SocketTransport created: {}:{}", config.host, config.port);
    }

    ~SocketTransport() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    SocketTransport(const SocketTransport&) = delete;
    SocketTransport& operator=(const SocketTransport&) = delete;

    SocketTransport(SocketTransport&& other) noexcept
        : config_(std::move(other.config_))
        , fd_(other.fd_)
        , state_(other.state_)
        , mss_(other.mss_) {
        other.fd_ = -1;
        other.state_ = TcpState::Closed;
    }

    SocketTransport& operator=(SocketTransport&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) ::close(fd_);
            config_ = std::move(other.config_);
            fd_ = other.fd_;
            state_ = other.state_;
            mss_ = other.mss_;
            other.fd_ = -1;
            other.state_ = TcpState::Closed;
        }
        return *this;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Connection
    // ─────────────────────────────────────────────────────────────────────────

    std::expected<void, std::string>
    connect(std::chrono::milliseconds timeout = std::chrono::milliseconds{3000}) {
        auto log = detail::socket_logger();

        if (state_ != TcpState::Closed) {
            return std::unexpected(std::format(
                "Cannot connect: state={}", tcp_state_name(state_)));
        }

        // Resolve hostname
        struct addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        auto port_str = std::to_string(config_.port);
        struct addrinfo* result = nullptr;
        int rc = ::getaddrinfo(config_.host.c_str(), port_str.c_str(),
                               &hints, &result);
        if (rc != 0) {
            SPDLOG_LOGGER_ERROR(log,
                "DNS resolution failed for {}:{}: {}",
                config_.host, config_.port, gai_strerror(rc));
            return std::unexpected(std::format(
                "DNS resolution failed: {}", gai_strerror(rc)));
        }

        // RAII cleanup for addrinfo
        struct AddrInfoGuard {
            addrinfo* p;
            ~AddrInfoGuard() { if (p) ::freeaddrinfo(p); }
        } guard{result};

        // Create socket
        fd_ = ::socket(result->ai_family, SOCK_STREAM | SOCK_NONBLOCK,
                       IPPROTO_TCP);
        if (fd_ < 0) {
            SPDLOG_LOGGER_ERROR(log, "socket() failed: {}", strerror(errno));
            return std::unexpected(std::format(
                "socket() failed: {}", strerror(errno)));
        }

        // Set TCP_NODELAY
        if (config_.tcp_nodelay) {
            int flag = 1;
            if (::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY,
                             &flag, sizeof(flag)) != 0) {
                SPDLOG_LOGGER_WARN(log, "Failed to set TCP_NODELAY: {}",
                                   strerror(errno));
            }
        }

        // Enable TCP keepalive probes
        if (config_.tcp_keepalive) {
            int flag = 1;
            if (::setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE,
                             &flag, sizeof(flag)) != 0) {
                SPDLOG_LOGGER_WARN(log, "Failed to set SO_KEEPALIVE: {}",
                                   strerror(errno));
            }
            if (::setsockopt(fd_, IPPROTO_TCP, TCP_KEEPIDLE,
                             &config_.keepalive_idle,
                             sizeof(config_.keepalive_idle)) != 0) {
                SPDLOG_LOGGER_WARN(log, "Failed to set TCP_KEEPIDLE: {}",
                                   strerror(errno));
            }
            if (::setsockopt(fd_, IPPROTO_TCP, TCP_KEEPINTVL,
                             &config_.keepalive_interval,
                             sizeof(config_.keepalive_interval)) != 0) {
                SPDLOG_LOGGER_WARN(log, "Failed to set TCP_KEEPINTVL: {}",
                                   strerror(errno));
            }
            if (::setsockopt(fd_, IPPROTO_TCP, TCP_KEEPCNT,
                             &config_.keepalive_count,
                             sizeof(config_.keepalive_count)) != 0) {
                SPDLOG_LOGGER_WARN(log, "Failed to set TCP_KEEPCNT: {}",
                                   strerror(errno));
            }
            SPDLOG_LOGGER_DEBUG(log,
                "TCP keepalive enabled: idle={}s, interval={}s, count={}",
                config_.keepalive_idle, config_.keepalive_interval,
                config_.keepalive_count);
        }

        // Set buffer sizes if configured
        if (config_.recv_buf_size > 0) {
            if (::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF,
                             &config_.recv_buf_size,
                             sizeof(config_.recv_buf_size)) != 0) {
                SPDLOG_LOGGER_WARN(log, "Failed to set SO_RCVBUF={}: {}",
                                   config_.recv_buf_size, strerror(errno));
            }
        }
        if (config_.send_buf_size > 0) {
            if (::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF,
                             &config_.send_buf_size,
                             sizeof(config_.send_buf_size)) != 0) {
                SPDLOG_LOGGER_WARN(log, "Failed to set SO_SNDBUF={}: {}",
                                   config_.send_buf_size, strerror(errno));
            }
        }

        state_ = TcpState::SynSent;
        SPDLOG_LOGGER_DEBUG(log, "Connecting to {}:{}...",
                            config_.host, config_.port);

        // Non-blocking connect
        rc = ::connect(fd_, result->ai_addr, result->ai_addrlen);
        if (rc == 0) {
            // Immediate connection (unlikely for TCP, but possible on loopback)
            state_ = TcpState::Established;
            query_mss();
            SPDLOG_LOGGER_INFO(log, "Connected to {}:{}",
                               config_.host, config_.port);
            return {};
        }

        if (errno != EINPROGRESS) {
            SPDLOG_LOGGER_ERROR(log, "connect() failed: {}", strerror(errno));
            close_fd();
            return std::unexpected(std::format(
                "connect() failed: {}", strerror(errno)));
        }

        // Wait for connection with poll()
        struct pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLOUT;

        int poll_rc = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
        if (poll_rc <= 0) {
            SPDLOG_LOGGER_ERROR(log, "Connection timeout ({}ms) to {}:{}",
                                timeout.count(), config_.host, config_.port);
            close_fd();
            return std::unexpected(std::format(
                "Connection timeout after {}ms", timeout.count()));
        }

        // Check for connection error
        int so_error = 0;
        socklen_t so_len = sizeof(so_error);
        if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &so_error, &so_len) != 0) {
            SPDLOG_LOGGER_ERROR(log, "getsockopt(SO_ERROR) failed: {}",
                                strerror(errno));
            close_fd();
            return std::unexpected(std::format(
                "getsockopt(SO_ERROR) failed: {}", strerror(errno)));
        }
        if (so_error != 0) {
            SPDLOG_LOGGER_ERROR(log, "Connection failed: {}", strerror(so_error));
            close_fd();
            return std::unexpected(std::format(
                "Connection failed: {}", strerror(so_error)));
        }

        state_ = TcpState::Established;
        query_mss();
        SPDLOG_LOGGER_INFO(log, "Connected to {}:{} (mss={})",
                           config_.host, config_.port, mss_);
        return {};
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Data transfer
    // ─────────────────────────────────────────────────────────────────────────

    std::expected<size_t, std::string>
    send(const void* data, size_t len) {
        if (state_ != TcpState::Established) {
            return std::unexpected(std::format(
                "Cannot send: state={}", tcp_state_name(state_)));
        }

        const uint8_t* ptr = static_cast<const uint8_t*>(data);
        size_t remaining = len;

        while (remaining > 0) {
            ssize_t n = ::send(fd_, ptr, remaining, MSG_NOSIGNAL);
            if (n > 0) {
                ptr += n;
                remaining -= static_cast<size_t>(n);
                continue;
            }

            if (n == 0) {
                state_ = TcpState::Closed;
                return std::unexpected("Connection closed by peer");
            }

            // n < 0
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Wait for socket to become writable (short poll)
                struct pollfd pfd{};
                pfd.fd = fd_;
                pfd.events = POLLOUT;
                int rc = ::poll(&pfd, 1, 1000);
                if (rc <= 0) {
                    return std::unexpected("send() timeout waiting for writable");
                }
                continue;
            }

            SPDLOG_LOGGER_ERROR(detail::socket_logger(),
                "send() failed: {}", strerror(errno));
            state_ = TcpState::Closed;
            return std::unexpected(std::format(
                "send() failed: {}", strerror(errno)));
        }

        SPDLOG_LOGGER_TRACE(detail::socket_logger(),
            "send() completed: {} bytes", len);
        return len;
    }

    /// Poll for incoming data and deliver via callback.
    /// Non-blocking: returns 0 if no data available.
    template <typename F>
        requires std::invocable<F, const uint8_t*, uint16_t>
    std::expected<uint16_t, std::string> poll_rx(F&& data_callback) {
        if (state_ != TcpState::Established &&
            state_ != TcpState::FinWait1 &&
            state_ != TcpState::FinWait2) {
            return std::unexpected(std::format(
                "Cannot receive: state={}", tcp_state_name(state_)));
        }

        uint8_t buf[16384];
        ssize_t n = ::recv(fd_, buf, sizeof(buf), MSG_DONTWAIT);

        if (n > 0) {
            SPDLOG_LOGGER_TRACE(detail::socket_logger(),
                "recv() got {} bytes", n);
            std::invoke(std::forward<F>(data_callback),
                        static_cast<const uint8_t*>(buf),
                        static_cast<uint16_t>(n));
            return static_cast<uint16_t>(1);
        }

        if (n == 0) {
            // Peer closed connection
            SPDLOG_LOGGER_DEBUG(detail::socket_logger(),
                "Peer closed connection");
            state_ = TcpState::CloseWait;
            return std::unexpected("Connection closed by peer");
        }

        // n < 0
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return uint16_t{0}; // No data available
        }

        SPDLOG_LOGGER_ERROR(detail::socket_logger(),
            "recv() failed: {}", strerror(errno));
        state_ = TcpState::Closed;
        return std::unexpected(std::format(
            "recv() failed: {}", strerror(errno)));
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Connection close
    // ─────────────────────────────────────────────────────────────────────────

    std::expected<void, std::string> close() {
        if (state_ != TcpState::Established &&
            state_ != TcpState::CloseWait) {
            return std::unexpected(std::format(
                "Cannot close: state={}", tcp_state_name(state_)));
        }

        SPDLOG_LOGGER_DEBUG(detail::socket_logger(), "Graceful shutdown");
        ::shutdown(fd_, SHUT_WR);

        if (state_ == TcpState::Established) {
            state_ = TcpState::FinWait1;
        } else {
            state_ = TcpState::LastAck;
        }

        // Close the fd after shutdown
        close_fd();
        return {};
    }

    void reset() noexcept {
        SPDLOG_LOGGER_DEBUG(detail::socket_logger(), "Sending RST");
        // Set SO_LINGER with timeout 0 to send RST instead of FIN
        struct linger l{};
        l.l_onoff = 1;
        l.l_linger = 0;
        if (fd_ >= 0) {
            ::setsockopt(fd_, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
        }
        close_fd();
    }

    // ─────────────────────────────────────────────────────────────────────────
    // State queries
    // ─────────────────────────────────────────────────────────────────────────

    [[nodiscard]] TcpState state()       const noexcept { return state_; }
    [[nodiscard]] uint16_t mss()         const noexcept { return mss_; }
    [[nodiscard]] bool is_established()  const noexcept {
        return state_ == TcpState::Established;
    }
    [[nodiscard]] const SocketConfig& config() const noexcept {
        return config_;
    }
    [[nodiscard]] int fd() const noexcept { return fd_; }

private:
    SocketConfig config_;
    int          fd_    = -1;
    TcpState     state_ = TcpState::Closed;
    uint16_t     mss_   = 1460; // Default MSS for Ethernet

    void close_fd() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        state_ = TcpState::Closed;
    }

    void query_mss() noexcept {
        if (fd_ < 0) return;
        int mss_val = 0;
        socklen_t len = sizeof(mss_val);
        if (::getsockopt(fd_, IPPROTO_TCP, TCP_MAXSEG, &mss_val, &len) != 0) {
            SPDLOG_LOGGER_WARN(detail::socket_logger(),
                "Failed to query TCP_MAXSEG: {}, using default mss={}",
                strerror(errno), mss_);
            return;
        }
        if (mss_val > 0) {
            mss_ = static_cast<uint16_t>(mss_val);
        } else {
            SPDLOG_LOGGER_WARN(detail::socket_logger(),
                "TCP_MAXSEG returned {}, using default mss={}",
                mss_val, mss_);
        }
    }
};

static_assert(TcpTransport<SocketTransport>,
    "SocketTransport must satisfy TcpTransport concept");

} // namespace eph::net

// ─────────────────────────────────────────────────────────────────────────────
// Type aliases (require transport.hpp — include after SocketTransport is defined
// to break the circular dependency with TlsSession<SocketTransport>)
// ─────────────────────────────────────────────────────────────────────────────

#include "eph/net/transport.hpp"

namespace eph::net {

/// Standard WebSocket transport using kernel sockets.
/// 512-byte max payload, 1024-deep SPSC queues.
using SocketWssTransport = Transport<SocketTransport, 512, 1024>;

/// Large-payload variant (4KB messages, e.g. JSON market data).
using SocketWssLargeTransport = Transport<SocketTransport, 4096, 512>;

/// Convenience factory: creates a fully connected SocketWssTransport
/// from just a TransportConfig, eliminating the TcpFactory boilerplate.
///
/// Equivalent to:
///   auto factory = [&]() { /* create SocketTransport, connect */ };
///   auto transport = SocketWssTransport::create(factory, config);
///
/// Optionally accepts a SocketConfig for fine-grained TCP tuning;
/// if omitted, sensible defaults are derived from the TransportConfig.
///
/// @param config     Transport configuration (host, port, TLS, WS settings)
/// @param sock_cfg   Optional socket-level config (TCP_NODELAY, keepalive, etc.)
/// @return Connected SocketWssTransport or error string
template <size_t MaxPayload = 512, size_t QueueDepth = 1024>
[[nodiscard]] inline auto
socket_wss_connect(
    const TransportConfig& config,
    std::optional<SocketConfig> sock_cfg = std::nullopt)
    -> std::expected<std::unique_ptr<Transport<SocketTransport, MaxPayload, QueueDepth>>,
                     std::string>
{
    SocketConfig sc = sock_cfg.value_or(SocketConfig{
        .host         = config.remote_host,
        .port         = config.remote_port,
        .tcp_nodelay  = true,
    });

    // Ensure host/port match TransportConfig if not explicitly overridden
    if (!sock_cfg) {
        sc.host = config.remote_host;
        sc.port = config.remote_port;
    }

    auto tcp_timeout = config.tcp_timeout;

    auto tcp_factory = [sc, tcp_timeout]()
        -> std::expected<std::unique_ptr<SocketTransport>, std::string> {
        auto tcp = std::make_unique<SocketTransport>(sc);
        auto result = tcp->connect(tcp_timeout);
        if (!result) return std::unexpected(result.error());
        return tcp;
    };

    return Transport<SocketTransport, MaxPayload, QueueDepth>::create(
        std::move(tcp_factory), config);
}

} // namespace eph::net
