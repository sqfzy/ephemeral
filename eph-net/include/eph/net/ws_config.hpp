#pragma once

/// @file ws_config.hpp
/// @brief Common WebSocket-upgrade configuration shared by Kernel and DPDK
///        stream backends.
///
/// Extracted from the duplicated `ws_path / ws_host / ws_extra_headers /
/// ws_timeout / ws_permessage_deflate` field cluster that previously lived
/// verbatim in both `eph::net::kernel::StreamConfig` and
/// `eph::net::dpdk::StreamConfig`. Each backend now embeds a single
/// `eph::net::WsConfig ws` member.
///
/// Lifetime: `extra_headers` views must outlive the `Stream::create()` call
/// that consumes them, the same constraint as the legacy fields. The struct
/// itself owns no remote storage — it is a passive configuration value.

#include <chrono>
#include <expected>
#include <string>
#include <vector>

#include "eph/core/error.hpp"
#include "eph/net/http.hpp" // HttpHeader

namespace eph::net {

/// @brief Configuration for the optional RFC 6455 WebSocket Upgrade.
///
/// Empty `path` disables the upgrade entirely; the stream behaves as plain
/// TCP / TLS. Non-empty `path` triggers the handshake inside
/// `KernelTcpStream::create` / `DpdkTcpStream::create_and_attach`.
struct WsConfig {
    /// @brief WebSocket request-target (e.g. "/ws/btcusdt@bookTicker").
    ///        Empty = no upgrade. The struct is otherwise inert when this
    ///        is empty.
    std::string path{};

    /// @brief Value for the `Host:` header. If empty, the handshake falls
    ///        back to `tls.hostname` (TLS path) or numeric `remote.to_string()`
    ///        (plaintext path).
    std::string host{};

    /// @brief Caller-supplied headers appended AFTER the mandatory upgrade
    ///        headers (Host / Upgrade / Connection / Sec-WebSocket-Key /
    ///        Sec-WebSocket-Version). Values are non-owning views; the caller
    ///        must keep backing storage alive until `Stream::create` returns.
    std::vector<HttpHeader> extra_headers{};

    /// @brief Cumulative deadline for the WS HTTP handshake phase. Must be
    ///        > 0 when `path` is non-empty.
    std::chrono::milliseconds timeout{std::chrono::seconds{10}};

    /// @brief Offer RFC 7692 permessage-deflate during the upgrade. When
    ///        true and `path` is non-empty, the request line gets
    ///        `Sec-WebSocket-Extensions: permessage-deflate`; if the server
    ///        accepts, inbound RSV1 frames are inflated by the codec.
    ///
    /// Default true so HFT venues that may opt into compression
    /// (Binance / Bybit / OKX) get correct behaviour out of the box. Set
    /// to false to suppress the offer for venues that mis-implement the
    /// extension.
    bool permessage_deflate{true};

    /// @brief True iff the WS upgrade is disabled (path empty). Backends
    ///        use this to short-circuit handshake plumbing.
    [[nodiscard]] bool empty() const noexcept { return path.empty(); }

    /// @brief Validate the config. `noexcept` + allocation-free.
    ///
    /// @return `{}` on success. `ErrorInfo{InvalidConfig, "..."}` when:
    ///   * `timeout <= 0` and `path` non-empty.
    ///   * `path` is non-empty but does not begin with '/' — RFC 7230
    ///     request-targets in origin form MUST start with '/' (or be the
    ///     special-cased '*' for OPTIONS, which is not a valid WS target).
    ///     Without this guard a misspelled path like "ws/btcusdt" would
    ///     produce `GET ws/btcusdt HTTP/1.1\r\n` on the wire, which most
    ///     servers reject with 400 Bad Request — the failure surface
    ///     becomes a generic WS handshake error instead of a
    ///     pinpoint config-validation message.
    ///   * `path` contains CR / LF / NUL / SP / HTAB — would be rejected
    ///     by `build_http_request`'s target-token check after the connect
    ///     cost was paid; reject up front.
    ///   * `host` contains CR / LF / NUL — header-injection vector
    ///     (the value would land in the `Host:` header of the HTTP
    ///     Upgrade request). `build_http_request` catches it later via
    ///     `builder_reject_unsafe_value`, but only after TCP+TLS handshake
    ///     has already paid its full cost; pre-validating here turns a
    ///     stale `WsHandshakeFailed` (returned a few hundred ms after
    ///     `create()` was called) into a synchronous `InvalidConfig`.
    ///   * any `extra_headers` entry has an empty / non-token name OR a
    ///     value containing CR / LF / NUL — same defense-in-depth as
    ///     `host`, with the same late-failure-vs-early-failure tradeoff.
    ///
    /// Empty WsConfig (default-constructed) is always valid — it disables
    /// the upgrade. Backends only need to call this when `!empty()`.
    [[nodiscard]] std::expected<void, ::eph::core::ErrorInfo>
    validate() const noexcept {
        if (path.empty()) {
            return {}; // disabled = always valid
        }
        if (timeout <= std::chrono::milliseconds::zero()) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig,
                "WsConfig: timeout must be > 0 when path is set"});
        }
        if (path[0] != '/') {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig,
                "WsConfig: path must begin with '/' (RFC 7230 origin-form "
                "request-target)"});
        }
        // Path goes into the request-target slot of GET <target> HTTP/1.1.
        // build_http_request rejects CR/LF/NUL/SP/HTAB via
        // `builder_reject_unsafe_token` — replicate the check here so the
        // failure surfaces synchronously at config time rather than after
        // TCP + TLS handshake have already paid their cost. Defense-in-
        // depth: build_http_request still enforces the same invariant
        // downstream regardless.
        if (detail::builder_reject_unsafe_token(path)) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig,
                "WsConfig: path contains CR / LF / NUL / SP / HTAB — would "
                "be rejected by HTTP request builder downstream"});
        }
        // Host header value injection: a Host containing CR/LF would
        // smuggle a second header line into the Upgrade request,
        // potentially pinning a back-end behind a forwarding proxy or
        // poisoning shared HTTP caches. build_http_request catches this
        // via `builder_reject_unsafe_value` once the header vector is
        // assembled — that's also valid, but only fires AFTER the TCP +
        // TLS handshake has completed. Validating here turns a
        // late `WsHandshakeFailed` (where the operator has to check
        // perform_ws_handshake's sub-error to see "invalid header value")
        // into a synchronous `InvalidConfig`. Empty `host` is fine — it
        // falls back to `tls.hostname` / `remote.to_string()` downstream.
        if (!host.empty() && detail::builder_reject_unsafe_value(host)) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig,
                "WsConfig: host contains CR / LF / NUL — would inject "
                "extra headers into the Upgrade request"});
        }
        // Same defense for caller-supplied extras. Mirror the exact
        // checks `validate_builder_headers` runs (token name + safe
        // value) so a config that passes here is guaranteed to pass
        // build_http_request later.
        for (const auto& h : extra_headers) {
            if (h.name.empty() || !detail::is_valid_token(h.name)) {
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::InvalidConfig,
                    "WsConfig: extra_headers entry has empty or non-token "
                    "name (RFC 7230 token: ALPHA / DIGIT / a few specials)"});
            }
            if (detail::builder_reject_unsafe_value(h.value)) {
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::InvalidConfig,
                    "WsConfig: extra_headers entry value contains CR / LF "
                    "/ NUL — header injection vector"});
            }
        }
        return {};
    }
};

} // namespace eph::net
