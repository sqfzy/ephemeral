#pragma once

/// @file proxy.hpp
/// HTTP CONNECT proxy configuration for `KernelTcpStream`.
///
/// Design rationale:
///
///   * **Config-driven** — a proxy is just another stage of the stream
///     bring-up, sitting between the raw TCP connect and the TLS handshake.
///     We do NOT expose a separate `connect_through_proxy()` factory;
///     callers set `StreamConfig.proxy` and the backend runs the tunnel
///     transparently.
///
///   * **HTTP CONNECT only** — SOCKS5 is explicitly out of scope. HFT colo
///     do not use proxies; this support exists purely for development,
///     testing, and restricted-network operator environments where CONNECT
///     is the lowest-common-denominator tunneling protocol.
///
///   * **Kernel-only** — the DPDK `StreamConfig` does not have a `proxy`
///     field at all (post-T3.19). Misuse is a compile-time error pointing
///     users at the kernel backend, which is the only place CONNECT is
///     supported. The previous "field exists but rejects at runtime"
///     compatibility hack was retired with the T3.19 reshape.
///
///   * **Basic auth only** — the CONNECT tunnel carries a single
///     `Proxy-Authorization: Basic base64(user:pass)` header. Digest /
///     NTLM / Kerberos auth schemes are out of scope (same reasoning as
///     SOCKS5: not used in HFT, too much code surface for a rarely-touched
///     path).
///
/// The struct is trivially constructible and validation is `noexcept` —
/// constructing an invalid config is free, and the error path returns a
/// typed `ErrorInfo` with a static-literal detail string.

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include "eph/core/error.hpp"
#include "eph/net/socket_addr.hpp"

namespace eph::net {

// ---------------------------------------------------------------------------
// ProxyConfig
// ---------------------------------------------------------------------------

/// @brief Configuration for an HTTP CONNECT proxy tunnel.
///
/// Only consumed by the kernel backend (`KernelTcpStream::create`). The
/// DPDK `StreamConfig` does not expose a `proxy` field — using a proxy
/// with a DPDK stream is a compile error rather than a runtime
/// `InvalidConfig`.
///
/// Validation semantics (see `validate()`):
///   * `host` must be non-empty
///   * `port` must be non-zero
///   * `basic_auth_user` and `basic_auth_pass` must either both be set or
///     both be unset (no "user without password" or vice versa)
///   * `timeout` must be strictly positive
struct ProxyConfig {
    /// @brief Proxy server numeric IPv4 address (e.g. "10.0.0.1").
    ///
    /// **Must be a dotted-quad IPv4 literal**, not a hostname.
    /// `KernelTcpStream::create` parses it via `Ipv4Addr::parse` and
    /// rejects with `Error::InvalidConfig` if the string fails to parse.
    /// DNS resolution for the proxy host is the caller's responsibility
    /// — invoke `eph::dpdk::dns::resolve` or another resolver yourself
    /// and pass the resulting dotted-quad here.
    ///
    /// (The pre-T3.19 doc string here advertised "hostname or IP" but
    /// the implementation has always required the literal form; this
    /// comment now reflects the implementation rather than the
    /// intent — see `eph-net-kernel/include/eph/net/kernel/tcp_stream.hpp`
    /// `KernelTcpStream::create`.)
    std::string                     host{};

    /// @brief Proxy server port. Must be non-zero. Typical values: 3128
    ///        (Squid), 8080 (many corporate proxies), 80 (legacy).
    uint16_t                        port{0};

    /// @brief Optional username for `Proxy-Authorization: Basic` auth.
    ///        If set, `basic_auth_pass` MUST also be set (and vice versa).
    std::optional<std::string>      basic_auth_user{};

    /// @brief Optional password for Basic auth. See `basic_auth_user`.
    std::optional<std::string>      basic_auth_pass{};

    /// @brief Cumulative deadline for the CONNECT handshake (TCP connect
    ///        to the proxy itself is bounded separately by
    ///        `StreamConfig.connect_timeout`). Must be > 0.
    std::chrono::milliseconds       timeout{std::chrono::seconds{10}};

    /// @brief Validate the config. `noexcept` + allocation-free.
    ///
    /// @return `{}` on success, `ErrorInfo{InvalidConfig, "<reason>"}` on
    ///         failure. The detail string is a static literal so callers may
    ///         store it indefinitely.
    [[nodiscard]] std::expected<void, ::eph::core::ErrorInfo>
    validate() const noexcept {
        if (host.empty()) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig,
                "ProxyConfig: host must be non-empty"});
        }
        // Surface the IPv4-literal contract at validate-time rather than
        // letting the caller burn a TCP setup roundtrip just to learn the
        // host string is a hostname. KernelTcpStream::create still re-parses
        // — that path stays the source of truth — but a caller that pre-
        // validates() now gets the same diagnostic without a connect attempt.
        if (auto r = Ipv4Addr::parse(host); !r) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig,
                "ProxyConfig: host must be a dotted-quad IPv4 literal "
                "(use Ipv4Addr::parse / dns::resolve to convert hostnames)"});
        }
        if (port == 0) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig,
                "ProxyConfig: port must be non-zero"});
        }
        // XOR: either both auth fields are set or neither is.
        if (basic_auth_user.has_value() != basic_auth_pass.has_value()) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig,
                "ProxyConfig: basic_auth_user and basic_auth_pass "
                "must both be set or both unset"});
        }
        // Reject empty-string credentials. has_value() distinguishes
        // "auth requested" from "no auth", but a defaulted std::string
        // sneaks through as has_value()==true with size()==0. We'd then
        // emit `Proxy-Authorization: Basic base64(":")` = `Og==`, which
        // some squid configurations silently accept and route as
        // anonymous, defeating the operator's intent. Force the caller
        // to either omit both fields or supply non-empty values.
        if (basic_auth_user.has_value() &&
            (basic_auth_user->empty() || basic_auth_pass->empty())) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig,
                "ProxyConfig: basic_auth_user / basic_auth_pass must be "
                "non-empty when auth is requested"});
        }
        if (timeout <= std::chrono::milliseconds::zero()) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig,
                "ProxyConfig: timeout must be > 0"});
        }
        return {};
    }
};

} // namespace eph::net
