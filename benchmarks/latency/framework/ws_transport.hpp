/// @file framework/ws_transport.hpp
/// Unified WebSocket transport setup for kernel + DPDK bench paths.
///
/// Provides:
///   - BenchWsTransport: the type alias used by all WS scenarios
///                       (DirectTransport<TcpImpl, WsFramer, 4096>)
///   - make_bench_tc():  TransportConfig builder (no TLS, no pings)
///   - connect_kernel(): kernel SocketTransport factory
///   - connect_dpdk():   DPDK TcpSession factory via eph::dpdk::connect()
///
/// The compile-time selection of TcpImpl (SocketTransport vs TcpSession)
/// happens via #ifdef EPH_USE_DPDK, so each scenario .cpp gets exactly
/// one BenchWsTransport type.

#pragma once

#include <chrono>
#include <expected>
#include <memory>
#include <string>

#include "eph/transport/direct_transport.hpp"
#include "eph/transport/transport_types.hpp"

#include "bench_config.hpp"

#if defined(EPH_USE_DPDK)
  #include "eph/dpdk/connector.hpp"
  #include "eph/dpdk/tcp.hpp"
  #include "framework/dpdk_setup.hpp"
#else
  #include "eph/net/socket_transport.hpp"
  #include "eph/net/socket_config.hpp"
#endif

namespace bench {

// ── Compile-time TCP impl selection ──────────────────────────────────────

#if defined(EPH_USE_DPDK)
using BenchTcpImpl = eph::dpdk::TcpSession<>;
#else
using BenchTcpImpl = eph::net::SocketTransport;
#endif

/// The DirectTransport instantiation used by all WS bench scenarios.
using BenchWsTransport =
    eph::net::DirectTransport<BenchTcpImpl, eph::net::WsFramer, 4096>;

// ── TransportConfig builder ──────────────────────────────────────────────

/// Build a TransportConfig with bench defaults: no TLS, no pings,
/// no reconnect, UTF-8 validation off.
inline eph::net::TransportConfig make_bench_tc(const BenchConfig& cfg) {
    eph::net::TransportConfig tc;
    tc.remote_host = cfg.server_ip;
    tc.remote_port = cfg.server_port;
    tc.ws_path = "/ws";
    tc.use_tls = false;
    tc.ping_interval = std::chrono::seconds{0};
    tc.max_reconnect_attempts = 0;
    tc.skip_utf8_validation = true;
    return tc;
}

// ── Connect helpers ──────────────────────────────────────────────────────

#if !defined(EPH_USE_DPDK)

/// Kernel mode: build SocketTransport factory + DirectTransport::create().
/// Returns the connected transport (DirectTransport) on success.
inline std::expected<std::unique_ptr<BenchWsTransport>, std::string>
ws_connect_kernel(const BenchConfig& cfg) {
    auto tc = make_bench_tc(cfg);

    auto tcp_factory = [&]() -> std::expected<std::unique_ptr<BenchTcpImpl>, std::string> {
        eph::net::SocketConfig sc{
            .host = cfg.server_ip,
            .port = cfg.server_port,
            .tcp_nodelay = true,
        };
        auto tcp = std::make_unique<BenchTcpImpl>(sc);
        auto r = tcp->connect(std::chrono::milliseconds{3000});
        if (!r) return std::unexpected(r.error());
        return tcp;
    };

    auto result = BenchWsTransport::create(std::move(tcp_factory), tc);
    if (!result) {
        return std::unexpected(
            "WS transport create failed: " + result.error().message());
    }
    return std::move(*result);
}

#else // EPH_USE_DPDK

/// DPDK mode: connect using an EXISTING Platform (from DpdkBenchEnv).
/// Returns a unique_ptr to the connected DirectTransport. The caller
/// must keep the underlying Platform alive for the duration of the bench
/// (the transport references the platform's mempool).
inline std::expected<std::unique_ptr<BenchWsTransport>, std::string>
ws_connect_dpdk(DpdkBenchEnv& env, const BenchConfig& cfg) {
    auto tc = make_bench_tc(cfg);
    eph::dpdk::DpdkEndpoint ep{
        .local_ip = cfg.local_ip,
        .gateway_ip = cfg.gateway_ip,
    };
    // Pre-resolve to a uint32_t server_ip so we don't go through DNS path.
    uint32_t server_ip = eph::dpdk::net::parse_ipv4(cfg.server_ip.c_str());
    eph::dpdk::ConnectorOptions opts{
        .platform = {.port_id = cfg.dpdk_port_id},
    };
    // Reuse the gateway MAC already resolved by DpdkBenchEnv to skip ARP.
    opts.gateway_mac = env.gw_mac;

    auto conn = eph::dpdk::connect<BenchWsTransport>(
        env.platform, ep, tc, server_ip, opts);
    if (!conn) {
        return std::unexpected("DPDK WS connect failed: " + conn.error());
    }
    return std::move(*conn);
}

#endif // EPH_USE_DPDK

} // namespace bench
