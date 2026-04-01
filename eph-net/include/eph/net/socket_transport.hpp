#pragma once

/// @file socket_transport.hpp
/// POSIX socket TCP transport — satisfies TcpTransport concept for non-DPDK use.
///
/// Uses non-blocking sockets with poll() for I/O multiplexing.
/// Designed for generic WebSocket clients that don't need DPDK's
/// kernel-bypass latency but want the same Transport<> API.

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <future>
#include <optional>
#include <string>

#include <atomic>

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/net_tstamp.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "eph/net/detail/json_escape.hpp"
#include "eph/net/tcp_concept.hpp"
#include "eph/net/transport_types.hpp"
#include "eph/utils/hdr_histogram.hpp"
#include "eph/utils/time.hpp"

namespace eph::net {

// ─────────────────────────────────────────────────────────────────────────────
// Compile-time timestamp control
// ─────────────────────────────────────────────────────────────────────────────

/// Compile-time switch for SO_TIMESTAMPING support in SocketTransport.
/// Pass -DEPH_ENABLE_TIMESTAMPS=1 via the build system to enable.
/// Any non-zero value enables; 0 or undefined disables.
#ifndef EPH_ENABLE_TIMESTAMPS
#define EPH_ENABLE_TIMESTAMPS 0
#endif

inline constexpr bool kEnableSocketTimestamps = (EPH_ENABLE_TIMESTAMPS != 0);

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
    int         send_timeout_ms = 1000;  // Timeout for individual send() poll waits (ms)

    /// Multi-line formatted dump for logging/debugging.
    [[nodiscard]] std::string dump() const {
        return std::format(
            "SocketConfig:\n"
            "  target: {}:{}\n"
            "  tcp_nodelay: {}, send_timeout: {}ms\n"
            "  buffers: recv={}, send={}\n"
            "  keepalive: enabled={}, idle={}s, interval={}s, count={}",
            host, port,
            tcp_nodelay, send_timeout_ms,
            recv_buf_size == 0 ? std::string("OS default") : std::to_string(recv_buf_size),
            send_buf_size == 0 ? std::string("OS default") : std::to_string(send_buf_size),
            tcp_keepalive, keepalive_idle, keepalive_interval, keepalive_count);
    }

    /// JSON-formatted config for monitoring system integration.
    /// String fields are escaped per RFC 8259 §7 to prevent malformed output.
    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{"
            "\"host\":\"{}\",\"port\":{},"
            "\"tcp_nodelay\":{},\"recv_buf_size\":{},"
            "\"send_buf_size\":{},\"tcp_keepalive\":{},"
            "\"keepalive_idle\":{},\"keepalive_interval\":{},"
            "\"keepalive_count\":{},\"send_timeout_ms\":{}}}",
            detail::json_escape(host), port,
            tcp_nodelay ? "true" : "false", recv_buf_size,
            send_buf_size, tcp_keepalive ? "true" : "false",
            keepalive_idle, keepalive_interval,
            keepalive_count, send_timeout_ms);
    }

    /// Defaulted equality — all fields must match exactly.
    [[nodiscard]] friend bool operator==(const SocketConfig&,
                                         const SocketConfig&) = default;

    /// Parse a "host:port" or "tcp://host:port" string into a SocketConfig.
    ///
    /// Supported URL forms:
    ///   tcp://host:port         (explicit scheme)
    ///   host:port               (scheme-less, port required)
    ///   tcp://[::1]:port        (IPv6 with brackets)
    ///   [::1]:port              (IPv6 without scheme)
    ///
    /// Only sets host and port. All other config fields retain their
    /// default values and can be modified after construction.
    ///
    /// @param url  Connection target string
    /// @return SocketConfig on success, or error description
    [[nodiscard]] static std::expected<SocketConfig, std::string>
    from_url(std::string_view url) {
        // Strip leading/trailing whitespace
        while (!url.empty() && (url.front() == ' ' || url.front() == '\t'))
            url.remove_prefix(1);
        while (!url.empty() && (url.back() == ' ' || url.back() == '\t'))
            url.remove_suffix(1);

        if (url.empty()) {
            return std::unexpected(std::string("URL must not be empty"));
        }

        // Strip optional tcp:// scheme
        if (url.starts_with("tcp://")) {
            url.remove_prefix(6);
        }

        if (url.empty()) {
            return std::unexpected(std::string("URL missing host"));
        }

        SocketConfig cfg;

        // IPv6 bracket notation: [::1]:port
        if (url.front() == '[') {
            size_t bracket_end = url.find(']');
            if (bracket_end == std::string_view::npos) {
                return std::unexpected(std::string("IPv6 address missing closing ']'"));
            }
            cfg.host = std::string(url.substr(1, bracket_end - 1));
            if (cfg.host.empty()) {
                return std::unexpected(std::string("URL has empty IPv6 address"));
            }
            url.remove_prefix(bracket_end + 1);

            // Must have :port after bracket
            if (url.empty() || url.front() != ':') {
                return std::unexpected(std::string("port is required (use host:port format)"));
            }
            url.remove_prefix(1); // skip ':'
        } else {
            // Regular hostname: find the last ':' that separates host from port.
            // For IPv6 without brackets (not supported), this would break,
            // but IPv6 should always use brackets.
            size_t colon = url.rfind(':');
            if (colon == std::string_view::npos) {
                return std::unexpected(std::string("port is required (use host:port format)"));
            }
            cfg.host = std::string(url.substr(0, colon));
            if (cfg.host.empty()) {
                return std::unexpected(std::string("URL has empty host"));
            }
            url.remove_prefix(colon + 1);
        }

        // Reject control characters in hostname (cast to unsigned to avoid
        // signed-char treating bytes >= 0x80 as negative)
        for (char c : cfg.host) {
            auto uc = static_cast<unsigned char>(c);
            if (uc < 0x20 || uc == 0x7f) {
                return std::unexpected(std::string("hostname contains control characters"));
            }
        }

        // Parse port (required)
        if (url.empty()) {
            return std::unexpected(std::string("URL has empty port after ':'"));
        }

        uint32_t port32 = 0;
        auto [ptr, ec] = std::from_chars(
            url.data(), url.data() + url.size(), port32);
        if (ec != std::errc{} || ptr != url.data() + url.size()) {
            return std::unexpected(
                std::format("invalid port: '{}'", url));
        }
        if (port32 == 0 || port32 > 65535) {
            return std::unexpected(
                std::format("port out of range: {}", port32));
        }
        cfg.port = static_cast<uint16_t>(port32);

        return cfg;
    }

    /// Serialize the connection target as "tcp://host:port".
    ///
    /// Inverse of from_url(): reconstructs a URL from host and port.
    /// IPv6 addresses are bracket-enclosed per RFC 2732.
    ///
    /// @return URL string like "tcp://host:port"
    [[nodiscard]] std::string to_url() const {
        bool is_ipv6 = host.find(':') != std::string::npos;
        if (is_ipv6) {
            return std::format("tcp://[{}]:{}", host, port);
        }
        return std::format("tcp://{}:{}", host, port);
    }

    /// Validate configuration, returning an error description or empty string on success.
    /// Call before constructing SocketTransport for early, actionable error messages.
    [[nodiscard]] constexpr std::string_view validate() const noexcept {
        if (host.empty())
            return "host must not be empty";
        if (port == 0)
            return "port must be > 0";
        if (recv_buf_size < 0)
            return "recv_buf_size must be >= 0 (0 = OS default)";
        if (recv_buf_size > 0 && recv_buf_size < 1024)
            return "recv_buf_size must be at least 1024 when set (or 0 for OS default)";
        if (send_buf_size < 0)
            return "send_buf_size must be >= 0 (0 = OS default)";
        if (send_buf_size > 0 && send_buf_size < 1024)
            return "send_buf_size must be at least 1024 when set (or 0 for OS default)";
        if (send_timeout_ms < 0)
            return "send_timeout_ms must be >= 0";
        if (send_timeout_ms == 0)
            return "send_timeout_ms must be positive";
        if (keepalive_interval > 0 && keepalive_idle <= 0)
            return "keepalive_idle must be positive when keepalive_interval > 0";
        if (tcp_keepalive) {
            if (keepalive_idle <= 0)
                return "keepalive_idle must be positive when keepalive is enabled";
            if (keepalive_interval <= 0)
                return "keepalive_interval must be positive when keepalive is enabled";
            if (keepalive_count <= 0)
                return "keepalive_count must be positive when keepalive is enabled";
        }
        return {};
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Logger
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

inline const std::shared_ptr<spdlog::logger>& socket_logger() {
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
///
/// When EPH_ENABLE_TIMESTAMPS is defined to true (e.g. -DEPH_ENABLE_TIMESTAMPS=true),
/// uses SO_TIMESTAMPING to capture kernel-level RX/TX timestamps for fair
/// latency comparison with DPDK backends.
class SocketTransport {
public:
    explicit SocketTransport(const SocketConfig& config) noexcept
        : config_(config) {
        SPDLOG_LOGGER_DEBUG(detail::socket_logger(),
            "SocketTransport created: {}:{}", config.host, config.port);
    }

    ~SocketTransport() {
        close_fd();
    }

    SocketTransport(const SocketTransport&) = delete;
    SocketTransport& operator=(const SocketTransport&) = delete;

    SocketTransport(SocketTransport&& other) noexcept
        : config_(std::move(other.config_))
        , fd_(other.fd_)
        , state_(other.state_)
        , mss_(other.mss_)
        , resolved_ip_(std::move(other.resolved_ip_))
        , dns_latency_ns_(other.dns_latency_ns_)
        , connect_latency_ns_(other.connect_latency_ns_) {
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
            resolved_ip_ = std::move(other.resolved_ip_);
            dns_latency_ns_ = other.dns_latency_ns_;
            connect_latency_ns_ = other.connect_latency_ns_;
            other.fd_ = -1;
            other.state_ = TcpState::Closed;
        }
        return *this;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Connection
    // ─────────────────────────────────────────────────────────────────────────

    [[nodiscard]] std::expected<void, std::string>
    connect(std::chrono::milliseconds timeout = std::chrono::milliseconds{3000}) {
        auto log = detail::socket_logger();

        if (state_ != TcpState::Closed) {
            return std::unexpected(std::format(
                "Cannot connect: state={}", tcp_state_name(state_)));
        }

        auto connect_start = std::chrono::steady_clock::now();

        // Resolve hostname with timeout protection.
        // getaddrinfo() has no built-in timeout — it can block indefinitely
        // if the DNS server is unreachable. We run it on a detached async
        // task and wait_for() with the connect timeout to bound the delay.
        struct addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_flags = AI_ADDRCONFIG;  // Only return addresses the host can reach

        auto port_str = std::to_string(config_.port);

        // Capture host/port by value for async task safety
        auto dns_host = config_.host;
        auto dns_port = port_str;
        // Use shared_ptr so that if we time out, the async thread still
        // frees the addrinfo when it eventually completes (no leak).
        auto dns_future = std::async(std::launch::async,
            [dns_host, dns_port, hints]() mutable
                -> std::expected<std::shared_ptr<struct addrinfo>, std::string> {
                struct addrinfo* res = nullptr;
                int rc = ::getaddrinfo(dns_host.c_str(), dns_port.c_str(),
                                       &hints, &res);
                if (rc != 0) {
                    return std::unexpected(std::format(
                        "DNS resolution failed: {}", gai_strerror(rc)));
                }
                // Wrap in shared_ptr with freeaddrinfo deleter — if the caller
                // times out and abandons the future, the async thread's copy of
                // the shared_ptr will free the addrinfo when it goes out of scope.
                return std::shared_ptr<struct addrinfo>(res, ::freeaddrinfo);
            });

        auto dns_status = dns_future.wait_for(timeout);
        if (dns_status == std::future_status::timeout) {
            SPDLOG_LOGGER_ERROR(log,
                "DNS resolution timeout ({}ms) for {}:{}",
                timeout.count(), config_.host, config_.port);
            // The async thread holds a shared_ptr that will freeaddrinfo()
            // when getaddrinfo eventually returns. No leak.
            return std::unexpected(std::format(
                "DNS resolution timeout after {}ms", timeout.count()));
        }

        auto dns_result = dns_future.get();
        dns_latency_ns_ = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - connect_start).count());
        if (!dns_result) {
            SPDLOG_LOGGER_ERROR(log,
                "DNS resolution failed for {}:{}: {} (dns: {:.1f}ms)",
                config_.host, config_.port, dns_result.error(),
                static_cast<double>(dns_latency_ns_) / 1e6);
            return std::unexpected(dns_result.error());
        }
        // shared_ptr already handles freeaddrinfo via custom deleter
        auto result_ptr = *dns_result;
        struct addrinfo* result = result_ptr.get();

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

        // Enable kernel RX/TX software timestamps for latency measurement.
        // Provides netif_receive_skb timestamp (RX) and qdisc/driver timestamp (TX).
        if constexpr (kEnableSocketTimestamps) {
            int ts_flags = SOF_TIMESTAMPING_RX_SOFTWARE
                         | SOF_TIMESTAMPING_TX_SOFTWARE
                         | SOF_TIMESTAMPING_SOFTWARE
                         | SOF_TIMESTAMPING_OPT_TSONLY;
            if (::setsockopt(fd_, SOL_SOCKET, SO_TIMESTAMPING,
                             &ts_flags, sizeof(ts_flags)) != 0) {
                SPDLOG_LOGGER_WARN(log, "Failed to set SO_TIMESTAMPING: {}",
                                   strerror(errno));
            } else {
                SPDLOG_LOGGER_DEBUG(log, "SO_TIMESTAMPING enabled (RX+TX software)");
            }
        }

        // Capture resolved IP address for observability
        resolve_ip(result);

        state_ = TcpState::SynSent;
        SPDLOG_LOGGER_DEBUG(log, "Connecting to {}:{} (resolved: {})...",
                            config_.host, config_.port, resolved_ip_);

        // Non-blocking connect
        int rc = ::connect(fd_, result->ai_addr, result->ai_addrlen);
        if (rc == 0) {
            // Immediate connection (unlikely for TCP, but possible on loopback)
            state_ = TcpState::Established;
            query_mss();
            connect_latency_ns_ = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - connect_start).count());
            SPDLOG_LOGGER_INFO(log, "Connected to {} ({}:{}, dns: {:.1f}ms, total: {:.1f}ms)",
                               resolved_ip_, config_.host, config_.port,
                               static_cast<double>(dns_latency_ns_) / 1e6,
                               static_cast<double>(connect_latency_ns_) / 1e6);
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
        connect_latency_ns_ = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - connect_start).count());
        SPDLOG_LOGGER_INFO(log, "Connected to {} ({}:{}, mss={}, dns: {:.1f}ms, total: {:.1f}ms)",
                           resolved_ip_, config_.host, config_.port, mss_,
                           static_cast<double>(dns_latency_ns_) / 1e6,
                           static_cast<double>(connect_latency_ns_) / 1e6);
        return {};
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Data transfer
    // ─────────────────────────────────────────────────────────────────────────

    [[nodiscard]] std::expected<size_t, std::string>
    send(const void* data, size_t len) {
        if (state_ != TcpState::Established) {
            return std::unexpected(std::format(
                "Cannot send: state={}", tcp_state_name(state_)));
        }

        const uint8_t* ptr = static_cast<const uint8_t*>(data);
        size_t remaining = len;

        // Total deadline prevents unbounded retry accumulation when the
        // socket repeatedly returns EAGAIN across multiple partial writes.
        auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds{config_.send_timeout_ms};

        while (remaining > 0) {
            // Record send time for TX kernel stack latency (send → wire).
            // Only on the first write of each send() call to avoid
            // overwriting on partial-write retries.
            if constexpr (kEnableSocketTimestamps) {
                if (ptr == static_cast<const uint8_t*>(data)) {
                    struct timespec ts;
                    ::clock_gettime(CLOCK_REALTIME, &ts);
                    last_send_realtime_ns_.store(
                        static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec,
                        std::memory_order_relaxed);
                }
            }
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
                // Compute remaining time until deadline for this poll wait
                auto now = std::chrono::steady_clock::now();
                auto remaining_ms = std::chrono::duration_cast<
                    std::chrono::milliseconds>(deadline - now).count();
                if (remaining_ms <= 0) {
                    SPDLOG_LOGGER_WARN(detail::socket_logger(),
                        "send() total timeout exceeded: {}ms, "
                        "{}/{} bytes sent",
                        config_.send_timeout_ms, len - remaining, len);
                    return std::unexpected(std::format(
                        "send() total timeout exceeded ({}ms, {}/{} bytes sent)",
                        config_.send_timeout_ms, len - remaining, len));
                }

                // Wait for socket to become writable, capped by deadline
                struct pollfd pfd{};
                pfd.fd = fd_;
                pfd.events = POLLOUT;
                int poll_ms = static_cast<int>(
                    std::min(remaining_ms,
                             static_cast<int64_t>(config_.send_timeout_ms)));
                int rc = ::poll(&pfd, 1, poll_ms);
                if (rc <= 0) {
                    SPDLOG_LOGGER_WARN(detail::socket_logger(),
                        "send() poll timeout: {}ms, {}/{} bytes sent",
                        poll_ms, len - remaining, len);
                    return std::unexpected(std::format(
                        "send() timeout waiting for writable ({}ms)",
                        config_.send_timeout_ms));
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
    [[nodiscard]] std::expected<uint16_t, std::string> poll_rx(F&& data_callback) {
        if (state_ != TcpState::Established &&
            state_ != TcpState::FinWait1 &&
            state_ != TcpState::FinWait2) {
            return std::unexpected(std::format(
                "Cannot receive: state={}", tcp_state_name(state_)));
        }

        uint8_t buf[16384];
        ssize_t n;

        if constexpr (kEnableSocketTimestamps) {
            // Use recvmsg() to extract kernel RX timestamp from cmsg.
            struct iovec iov = { .iov_base = buf, .iov_len = sizeof(buf) };
            alignas(struct cmsghdr) uint8_t ctrl[256];
            struct msghdr msg = {};
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            msg.msg_control = ctrl;
            msg.msg_controllen = sizeof(ctrl);

            n = ::recvmsg(fd_, &msg, MSG_DONTWAIT);

            // Capture TSC right after recvmsg — earliest userspace
            // proxy for "data left kernel", before any further processing.
            if (n > 0) last_rx_burst_tsc_ = eph::utils::TSC::now();

            if (n > 0) {
                // Extract RX kernel timestamp from cmsg and record kernel stack latency.
                extract_rx_timestamp(msg);

                // Also poll error queue for any pending TX timestamps.
                poll_tx_error_queue();
            }
        } else {
            n = ::recv(fd_, buf, sizeof(buf), MSG_DONTWAIT);

            if (n > 0) last_rx_burst_tsc_ = eph::utils::TSC::now();
        }

        if (n > 0) {
            SPDLOG_LOGGER_TRACE(detail::socket_logger(),
                "recv() got {} bytes", n);
            std::invoke(std::forward<F>(data_callback),
                        static_cast<const uint8_t*>(buf),
                        static_cast<uint16_t>(n));
            return static_cast<uint16_t>(1);
        }

        if (n == 0) {
            SPDLOG_LOGGER_DEBUG(detail::socket_logger(),
                "Peer closed connection");
            state_ = TcpState::CloseWait;
            return std::unexpected("Connection closed by peer");
        }

        // n < 0
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return uint16_t{0};
        }

        SPDLOG_LOGGER_ERROR(detail::socket_logger(),
            "recv() failed: {}", strerror(errno));
        state_ = TcpState::Closed;
        return std::unexpected(std::format(
            "recv() failed: {}", strerror(errno)));
    }

    /// Poll for incoming data with a timeout.
    /// Uses poll() to wait for data availability, avoiding CPU-burning spin loops.
    /// Returns the number of poll_rx callback invocations (0 or 1), or error.
    template <typename F, typename Rep, typename Period>
        requires std::invocable<F, const uint8_t*, uint16_t>
    [[nodiscard]] std::expected<uint16_t, std::string> poll_rx_for(
            F&& data_callback,
            std::chrono::duration<Rep, Period> timeout) {
        // First try non-blocking read
        auto result = poll_rx(std::forward<F>(data_callback));
        if (!result.has_value() || *result > 0) return result;

        // No data available — wait with poll()
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
        struct pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLIN;

        int ret = ::poll(&pfd, 1, static_cast<int>(ms.count()));
        if (ret < 0) {
            if (errno == EINTR) return uint16_t{0};
            SPDLOG_LOGGER_ERROR(detail::socket_logger(),
                "poll() failed: {}", strerror(errno));
            return std::unexpected(std::format("poll() failed: {}", strerror(errno)));
        }
        if (ret == 0) return uint16_t{0}; // Timeout

        // Data ready, do the actual read
        return poll_rx(std::forward<F>(data_callback));
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Connection close
    // ─────────────────────────────────────────────────────────────────────────

    [[nodiscard]] std::expected<void, std::string> close() {
        if (state_ != TcpState::Established &&
            state_ != TcpState::CloseWait) {
            return std::unexpected(std::format(
                "Cannot close: state={}", tcp_state_name(state_)));
        }

        SPDLOG_LOGGER_DEBUG(detail::socket_logger(), "Graceful shutdown");
        if (::shutdown(fd_, SHUT_WR) != 0) {
            SPDLOG_LOGGER_WARN(detail::socket_logger(),
                "shutdown(SHUT_WR) failed: errno={} ({})",
                errno, std::strerror(errno));
        }

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
            if (::setsockopt(fd_, SOL_SOCKET, SO_LINGER, &l, sizeof(l)) != 0) {
                SPDLOG_LOGGER_WARN(detail::socket_logger(),
                    "setsockopt(SO_LINGER) for RST failed: errno={} ({})",
                    errno, std::strerror(errno));
            }
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

    /// Resolved IP address string (e.g. "10.0.0.1") from the last connect().
    /// Empty if connect() has not been called or DNS resolution failed.
    [[nodiscard]] std::string_view resolved_ip() const noexcept {
        return resolved_ip_;
    }

    /// DNS resolution latency from the last connect() call (nanoseconds).
    /// Zero if connect() has not been called.
    [[nodiscard]] uint64_t dns_latency_ns() const noexcept {
        return dns_latency_ns_;
    }

    /// Total connect latency including DNS + TCP handshake (nanoseconds).
    /// Zero if connect() has not been called or failed.
    [[nodiscard]] uint64_t connect_latency_ns() const noexcept {
        return connect_latency_ns_;
    }

    /// Kernel RX stack latency histogram (NIC driver → recv return).
    /// Only populated when kEnableSocketTimestamps=true.
    [[nodiscard]] RttStats rx_latency() const noexcept {
        return histogram_to_rtt_stats(rx_stack_histogram_);
    }

    /// Kernel TX stack latency histogram (send call → wire departure).
    /// Only populated when kEnableSocketTimestamps=true.
    [[nodiscard]] RttStats tx_latency() const noexcept {
        return histogram_to_rtt_stats(tx_stack_histogram_);
    }

    /// TSC captured right after recvmsg/recv returns data.
    /// Transport uses this as the RX arrival baseline.
    [[nodiscard]] uint64_t last_rx_burst_tsc() const noexcept {
        return last_rx_burst_tsc_;
    }

#if EPH_ENABLE_TIMESTAMPS
    /// Most recent kernel RX stack delay (ns) from the last poll_rx() call.
    /// Transport reads this to compute full-path RX latency (kernel + pipeline).
    /// Only available when compiled with -DEPH_ENABLE_TIMESTAMPS=1.
    [[nodiscard]] uint64_t last_kernel_rx_delay_ns() const noexcept {
        return last_kernel_rx_delay_ns_;
    }

    /// Most recent kernel TX stack delay (ns) from the error queue.
    /// Transport reads this to compute full-path TX latency (pipeline + kernel).
    /// Only available when compiled with -DEPH_ENABLE_TIMESTAMPS=1.
    [[nodiscard]] uint64_t last_kernel_tx_delay_ns() const noexcept {
        return last_kernel_tx_delay_ns_;
    }
#endif

    /// Local port number of the connected socket.
    /// Zero if not connected.
    [[nodiscard]] uint16_t local_port() const noexcept {
        if (fd_ < 0) return 0;
        struct sockaddr_storage addr{};
        socklen_t len = sizeof(addr);
        if (::getsockname(fd_, reinterpret_cast<struct sockaddr*>(&addr), &len) != 0)
            return 0;
        if (addr.ss_family == AF_INET)
            return ntohs(reinterpret_cast<struct sockaddr_in*>(&addr)->sin_port);
        if (addr.ss_family == AF_INET6)
            return ntohs(reinterpret_cast<struct sockaddr_in6*>(&addr)->sin6_port);
        return 0;
    }

private:
    SocketConfig config_;
    int          fd_    = -1;
    TcpState     state_ = TcpState::Closed;
    uint16_t     mss_   = 1460; // Default MSS for Ethernet
    std::string  resolved_ip_;  // Resolved IP from last connect()
    uint64_t     dns_latency_ns_ = 0;
    uint64_t     connect_latency_ns_ = 0;

    // SO_TIMESTAMPING members (only active when kEnableSocketTimestamps=true).
    // RX: kernel NIC-driver-to-recv latency (CLOCK_REALTIME domain).
    // TX: kernel send-to-wire latency (CLOCK_REALTIME domain, via error queue).
    eph::utils::HdrHistogram rx_stack_histogram_{10, 1'000'000'000ULL, 3};
    eph::utils::HdrHistogram tx_stack_histogram_{10, 1'000'000'000ULL, 3};
    // NOTE: In burst-mode TX (multiple send() calls before an error-queue read),
    // last_send_realtime_ns_ reflects the timestamp of the FIRST send in the burst.
    // Subsequent sends in the same burst share this timestamp, so error-queue
    // matching may attribute the wrong packet's kernel TX delay to later sends.
    std::atomic<int64_t> last_send_realtime_ns_{0};  // TX thread writes, RX thread reads

    // TSC captured right after recvmsg/recv returns data.
    uint64_t last_rx_burst_tsc_ = 0;

    // Per-call kernel delay values (ns). Transport reads these after poll_rx/send
    // to compute full-path latency = kernel delay + pipeline delay.
    uint64_t last_kernel_rx_delay_ns_ = 0;  // written by RX thread in poll_rx
    uint64_t last_kernel_tx_delay_ns_ = 0;  // written by RX thread in poll_tx_error_queue

    void close_fd() noexcept {
        if (fd_ >= 0) {
            int ret = ::close(fd_);
            if (ret != 0) [[unlikely]] {
                [[maybe_unused]] int saved_errno = errno;
                SPDLOG_LOGGER_DEBUG(detail::socket_logger(),
                    "close(fd={}) failed: {} (errno={})",
                    fd_, strerror(saved_errno), saved_errno);
            }
            fd_ = -1;
        }
        state_ = TcpState::Closed;
    }

    /// Extract human-readable IP from addrinfo (IPv4 and IPv6).
    void resolve_ip(const struct addrinfo* ai) noexcept {
        if (!ai) {
            resolved_ip_.clear();
            return;
        }
        char buf[INET6_ADDRSTRLEN]{};
        const char* result = nullptr;
        if (ai->ai_family == AF_INET) {
            auto* sin = reinterpret_cast<const struct sockaddr_in*>(ai->ai_addr);
            result = ::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
        } else if (ai->ai_family == AF_INET6) {
            auto* sin6 = reinterpret_cast<const struct sockaddr_in6*>(ai->ai_addr);
            result = ::inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof(buf));
        }
        resolved_ip_ = result ? buf : "";
    }

    /// Convert HdrHistogram to RttStats (reuses the same stats struct).
    static RttStats histogram_to_rtt_stats(const eph::utils::HdrHistogram& h) noexcept {
        if (h.get_total_count() == 0) return {};
        return RttStats{
            .count   = h.get_total_count(),
            .min_ns  = h.get_min_value(),
            .max_ns  = h.get_max_value(),
            .mean_ns = h.get_mean(),
            .p50_ns  = h.get_value_at_percentile(50.0),
            .p99_ns  = h.get_value_at_percentile(99.0),
            .p999_ns = h.get_value_at_percentile(99.9),
        };
    }

    /// Get current CLOCK_REALTIME in nanoseconds.
    static int64_t clock_realtime_ns() noexcept {
        struct timespec ts;
        ::clock_gettime(CLOCK_REALTIME, &ts);
        return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
    }

    /// Extract RX kernel timestamp from recvmsg cmsg and record kernel stack latency.
    void extract_rx_timestamp(const struct msghdr& msg) noexcept {
        last_kernel_rx_delay_ns_ = 0;  // reset per call
        int64_t now_ns = clock_realtime_ns();

        for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
             cmsg != nullptr;
             cmsg = CMSG_NXTHDR(const_cast<struct msghdr*>(&msg), cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET &&
                cmsg->cmsg_type == SCM_TIMESTAMPING) {
                // SCM_TIMESTAMPING returns 3 timespecs:
                //   [0] = software timestamp (SOF_TIMESTAMPING_RX_SOFTWARE)
                //   [1] = deprecated
                //   [2] = hardware timestamp
                auto* ts = reinterpret_cast<const struct timespec*>(CMSG_DATA(cmsg));
                int64_t kernel_ns = static_cast<int64_t>(ts[0].tv_sec) * 1'000'000'000LL
                                  + ts[0].tv_nsec;
                if (kernel_ns > 0 && now_ns > kernel_ns) {
                    auto delay = static_cast<uint64_t>(now_ns - kernel_ns);
                    rx_stack_histogram_.record(delay);
                    last_kernel_rx_delay_ns_ = delay;
                }
                break;
            }
        }
    }

    /// Poll the socket error queue for TX kernel timestamps.
    /// TX timestamps are delivered asynchronously after send() via MSG_ERRQUEUE.
    void poll_tx_error_queue() noexcept {
        alignas(struct cmsghdr) uint8_t ctrl[256];
        uint8_t dummy[1];
        struct iovec iov = { .iov_base = dummy, .iov_len = sizeof(dummy) };
        struct msghdr msg = {};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = ctrl;
        msg.msg_controllen = sizeof(ctrl);

        // Non-blocking read from error queue
        ssize_t n = ::recvmsg(fd_, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
        if (n < 0) return;  // EAGAIN = no TX timestamp pending

        int64_t send_ns = last_send_realtime_ns_.load(std::memory_order_relaxed);
        if (send_ns <= 0) return;

        for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
             cmsg != nullptr;
             cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET &&
                cmsg->cmsg_type == SCM_TIMESTAMPING) {
                auto* ts = reinterpret_cast<const struct timespec*>(CMSG_DATA(cmsg));
                int64_t kernel_tx_ns = static_cast<int64_t>(ts[0].tv_sec) * 1'000'000'000LL
                                     + ts[0].tv_nsec;
                if (kernel_tx_ns > send_ns) {
                    auto delay = static_cast<uint64_t>(kernel_tx_ns - send_ns);
                    tx_stack_histogram_.record(delay);
                    last_kernel_tx_delay_ns_ = delay;
                }
                break;
            }
        }
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

#include "eph/net/raw_framer.hpp"
#include "eph/net/transport.hpp"

namespace eph::net {

// ---------------------------------------------------------------------------
// Type aliases — socket backend
// ---------------------------------------------------------------------------
// Naming convention:
//   Socket{Wss|Ws}{Small|Large|Evict}Transport
//   - Wss = WebSocket over TLS (default), Ws = plain WebSocket (use_tls=false)
//   - Small = 64B payload / 256 depth (compact single-symbol)
//   - Large = 4KB payload / 512 depth (JSON market data)
//   - Evict = EvictingQueue RX (drop stale, market data streams)
//   - (none) = 512B payload / 1024 depth (default)

/// Standard WSS (TLS) WebSocket transport using kernel sockets.
/// 512-byte max payload, 1024-deep SPSC queues.
using SocketWssTransport = Transport<SocketTransport, WsFramer, 512, 1024>;

/// Small-payload WSS variant (64B messages, single-symbol feeds).
using SocketWssSmallTransport = Transport<SocketTransport, WsFramer, 64, 256>;

/// Large-payload WSS variant (4KB messages, e.g. JSON market data).
using SocketWssLargeTransport = Transport<SocketTransport, WsFramer, 4096, 512>;

/// Evicting WSS variant — drops stale messages when RX queue is full.
/// Ideal for market data streams where only the latest update matters.
using SocketWssEvictTransport = Transport<SocketTransport, WsFramer, 512, 1024,
                                          eph::containers::EvictingQueue>;

/// Standard plain WS (no TLS) transport using kernel sockets.
/// Same as SocketWssTransport but configured with use_tls=false.
/// Use for internal/test services where TLS is not needed.
using SocketWsTransport = Transport<SocketTransport, WsFramer, 512, 1024>;

/// Raw TCP transport (no WebSocket framing). For FIX or custom protocols
/// that handle their own message boundaries.
using SocketRawTransport = Transport<SocketTransport, RawFramer, 512, 1024>;

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
    -> std::expected<std::unique_ptr<Transport<SocketTransport, WsFramer, MaxPayload, QueueDepth>>,
                     ConnectionErrorInfo>
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

    // Validate SocketConfig early for actionable error messages
    if (auto err = sc.validate(); !err.empty()) {
        return std::unexpected(ConnectionErrorInfo{
            ConnectionError::kInvalidConfig,
            std::format("SocketConfig: {}", err)});
    }

    auto tcp_timeout = config.tcp_timeout;

    auto tcp_factory = [sc, tcp_timeout]()
        -> std::expected<std::unique_ptr<SocketTransport>, std::string> {
        auto tcp = std::make_unique<SocketTransport>(sc);
        auto result = tcp->connect(tcp_timeout);
        if (!result) return std::unexpected(result.error());
        return tcp;
    };

    return Transport<SocketTransport, WsFramer, MaxPayload, QueueDepth>::create(
        std::move(tcp_factory), config);
}

/// Convenience factory for plain WebSocket (ws://) connections.
///
/// Creates a Transport with use_tls=false.
///
/// @param config     Transport configuration (host, port, WS settings)
/// @param sock_cfg   Optional socket-level config (TCP_NODELAY, keepalive, etc.)
/// @return Connected plain WS Transport or error string
template <size_t MaxPayload = 512, size_t QueueDepth = 1024>
[[nodiscard]] inline auto
socket_ws_connect(
    TransportConfig config,
    std::optional<SocketConfig> sock_cfg = std::nullopt)
    -> std::expected<std::unique_ptr<Transport<SocketTransport, WsFramer, MaxPayload, QueueDepth>>,
                     ConnectionErrorInfo>
{
    config.use_tls = false;

    SocketConfig sc = sock_cfg.value_or(SocketConfig{
        .host         = config.remote_host,
        .port         = config.remote_port,
        .tcp_nodelay  = true,
    });

    if (!sock_cfg) {
        sc.host = config.remote_host;
        sc.port = config.remote_port;
    }

    // Validate SocketConfig early for actionable error messages
    if (auto err = sc.validate(); !err.empty()) {
        return std::unexpected(ConnectionErrorInfo{
            ConnectionError::kInvalidConfig,
            std::format("SocketConfig: {}", err)});
    }

    auto tcp_timeout = config.tcp_timeout;

    auto tcp_factory = [sc, tcp_timeout]()
        -> std::expected<std::unique_ptr<SocketTransport>, std::string> {
        auto tcp = std::make_unique<SocketTransport>(sc);
        auto result = tcp->connect(tcp_timeout);
        if (!result) return std::unexpected(result.error());
        return tcp;
    };

    return Transport<SocketTransport, WsFramer, MaxPayload, QueueDepth>::create(
        std::move(tcp_factory), config);
}

/// Create a socket-based WebSocket transport from a URL string.
///
/// Combines TransportConfig::from_url() with the appropriate connect
/// function (wss or ws based on scheme), eliminating boilerplate.
///
/// Usage:
///   // Minimal:
///   auto t = eph::net::connect("wss://example.com/ws");
///
///   // With config customization:
///   auto t = eph::net::connect("wss://example.com/ws", [](auto& cfg) {
///       cfg.max_reconnect_attempts = 5;
///       cfg.on_message = [](auto* data, uint16_t len, uint8_t) { ... };
///   });
///
/// @param url       WebSocket URL (ws:// or wss://)
/// @param modifier  Optional callback to customize TransportConfig before connecting
/// @param sock_cfg  Optional socket-level config (TCP_NODELAY, keepalive, etc.)
/// @return Connected transport, or ConnectionErrorInfo on failure
template <size_t MaxPayload = 512, size_t QueueDepth = 1024,
          typename ConfigModifier = std::nullptr_t>
[[nodiscard]] inline auto
connect(std::string_view url,
        ConfigModifier modifier = nullptr,
        std::optional<SocketConfig> sock_cfg = std::nullopt)
    -> std::expected<std::unique_ptr<Transport<SocketTransport, WsFramer, MaxPayload, QueueDepth>>,
                     ConnectionErrorInfo>
{
    auto cfg_result = TransportConfig::from_url(url);
    if (!cfg_result) {
        return std::unexpected(ConnectionErrorInfo{
            ConnectionError::kInvalidConfig,
            std::format("Invalid URL: {}", cfg_result.error())});
    }

    auto cfg = std::move(*cfg_result);

    // Apply user customizations if provided
    if constexpr (!std::is_null_pointer_v<ConfigModifier>) {
        modifier(cfg);
    }

    if (cfg.use_tls) {
        return socket_wss_connect<MaxPayload, QueueDepth>(cfg, sock_cfg);
    } else {
        return socket_ws_connect<MaxPayload, QueueDepth>(cfg, sock_cfg);
    }
}

} // namespace eph::net

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter specialization for SocketConfig
// ─────────────────────────────────────────────────────────────────────────────

template <>
struct std::formatter<eph::net::SocketConfig> : std::formatter<std::string> {
    auto format(const eph::net::SocketConfig& c, auto& ctx) const {
        return std::formatter<std::string>::format(
            std::format("{}:{} nodelay={} keepalive={}",
                c.host, c.port, c.tcp_nodelay, c.tcp_keepalive),
            ctx);
    }
};
