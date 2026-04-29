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
        return {};
    }
};

} // namespace eph::net
