#pragma once

/// @file socket_config.hpp
/// Configuration struct for POSIX socket TCP transport.
///
/// Defines SocketConfig with URL parsing, validation, JSON/dump
/// serialization, and the kEnableSocketTimestamps compile-time switch.

#include <charconv>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

#include "eph/core/detail/json_escape.hpp"

namespace eph::net {

// Bring json_escape into eph::net::detail so existing to_json() code compiles.
namespace detail { using eph::core::detail::json_escape; }

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
