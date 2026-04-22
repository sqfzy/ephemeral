/// @file core/endpoint.hpp
/// Per-scenario endpoint resolution: `endpoint = mock | wss://...`.
///
/// Every bench client binary reads one `endpoint` key from its
/// `[lat_*]` section, plus the scenario's port/ws_path. `mock` (or an
/// absent key) means "talk to the mockex binary on mock_ip:port with
/// no TLS"; a `wss://host[:port]/path` value means "connect to a real
/// server, enable TLS, override ws_path from the URL".
///
/// The helpers here do one thing each: `parse_ws_url` splits a
/// `ws://` / `wss://` URL into host/port/path, and
/// `resolve_endpoint` reads the scenario + globals config and
/// returns the final host/port/path/tls tuple the client should use
/// when it calls `KernelTcpStream::create()`.
///
/// Zero heap allocation on the happy path beyond the std::string
/// member copies. noexcept-correctness is best-effort: the stdlib
/// string ops may throw on allocator failure, but everything else
/// is arithmetic + span scans.
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "core/bench_conf.hpp"

namespace bench {

/// Parsed WebSocket URL. Scheme is always `ws` or `wss`; the
/// `is_tls` flag is the useful distillation for callers (`true` ⇔
/// `wss`).
struct WsUrl {
    std::string host;
    uint16_t    port    = 0;
    std::string path;       ///< includes the leading `/`
    bool        is_tls  = false;
};

/// Final endpoint a client should connect to. `is_real_server` is
/// the "is this the mock or the real exchange?" bit — used by the
/// `lat` wrapper to decide whether to fork a mockex child and by
/// the client itself to choose between the plain and TLS-enabled
/// `KernelTcpStream` template instantiation.
struct ResolvedEndpoint {
    std::string host;
    uint16_t    port    = 0;
    std::string ws_path;    ///< empty if the scenario isn't WS-based
    bool        is_tls  = false;
    bool        is_real_server = false;
};

/// Parse a `ws://host[:port]/path` or `wss://host[:port]/path` URL
/// into its components. Returns an error string on any structural
/// problem — we do not try to be forgiving because benchmarks must
/// fail loud when configured wrong.
///
/// Defaults applied:
///   * `ws://`  → port 80
///   * `wss://` → port 443
///   * empty path → `/`
[[nodiscard]] inline std::expected<WsUrl, std::string>
parse_ws_url(std::string_view url) noexcept {
    WsUrl out;
    std::string_view rest;
    if (url.rfind("wss://", 0) == 0) {
        out.is_tls = true;
        out.port   = 443;
        rest = url.substr(6);
    } else if (url.rfind("ws://", 0) == 0) {
        out.is_tls = false;
        out.port   = 80;
        rest = url.substr(5);
    } else {
        return std::unexpected(
            "parse_ws_url: URL must start with ws:// or wss:// — got '" +
            std::string{url} + "'");
    }

    // Split off the path (first '/' after the authority).
    const size_t slash = rest.find('/');
    std::string_view authority = (slash == std::string_view::npos)
        ? rest : rest.substr(0, slash);
    out.path = (slash == std::string_view::npos)
        ? "/"
        : std::string{rest.substr(slash)};

    // Authority is `host` or `host:port`. No credentials, no brackets.
    const size_t colon = authority.find(':');
    if (colon == std::string_view::npos) {
        out.host = std::string{authority};
    } else {
        out.host = std::string{authority.substr(0, colon)};
        auto port_str = authority.substr(colon + 1);
        if (port_str.empty()) {
            return std::unexpected("parse_ws_url: empty port after ':'");
        }
        uint32_t p = 0;
        for (char c : port_str) {
            if (c < '0' || c > '9') {
                return std::unexpected(
                    "parse_ws_url: non-numeric port '" +
                    std::string{port_str} + "'");
            }
            p = p * 10 + static_cast<uint32_t>(c - '0');
            if (p > 65535) {
                return std::unexpected("parse_ws_url: port out of range");
            }
        }
        out.port = static_cast<uint16_t>(p);
    }
    if (out.host.empty()) {
        return std::unexpected("parse_ws_url: empty host");
    }
    return out;
}

/// Combine the [networking].server_ip + scenario `port` / `ws_path`
/// with an optional `endpoint = ...` override and return the resolved
/// target. Defaults:
///   * `endpoint` absent / "mock" → server_ip:port / ws_path, plaintext
///   * `endpoint = wss://...`     → URL fields, TLS, is_real_server
///
/// A malformed URL surfaces as an error; callers should exit rather
/// than silently fall through to the mock.
[[nodiscard]] inline std::expected<ResolvedEndpoint, std::string>
resolve_endpoint(const BenchConfig& cfg,
                 const Scenario& scenario) noexcept {
    ResolvedEndpoint out;
    const auto ep = scenario.get_or<std::string>("endpoint", "mock");

    if (ep == "mock" || ep.empty()) {
        out.host = cfg.networking.server_ip.empty()
                       ? std::string{"127.0.0.1"}
                       : cfg.networking.server_ip;
        auto port_e = scenario.get<uint16_t>("port");
        if (!port_e) return std::unexpected(format_error(port_e.error()));
        out.port = *port_e;
        out.ws_path = scenario.get_or<std::string>("ws_path", "");
        out.is_tls = false;
        out.is_real_server = false;
        return out;
    }

    if (ep.rfind("ws://", 0) != 0 && ep.rfind("wss://", 0) != 0) {
        return std::unexpected(
            "resolve_endpoint: 'endpoint' must be 'mock' or a ws[s]:// "
            "URL — got '" + ep + "'");
    }

    auto url_e = parse_ws_url(ep);
    if (!url_e) return std::unexpected(url_e.error());
    out.host          = std::move(url_e->host);
    out.port          = url_e->port;
    out.ws_path       = std::move(url_e->path);
    out.is_tls        = url_e->is_tls;
    out.is_real_server = true;
    return out;
}

} // namespace bench
