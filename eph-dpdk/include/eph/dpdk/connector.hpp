#pragma once

/// @file connector.hpp
/// High-level DPDK connection helper — collapses the Platform→MAC→ARP→TCP→Transport
/// initialization sequence into a single `connect()` call.
///
/// Designed for the common case where you want a WebSocket-over-TLS connection
/// on a single DPDK port with default settings.  Advanced users who need
/// fine-grained control can still compose Platform, TcpSession, and Transport
/// directly.
///
/// Two `connect()` overloads:
///   - Hostname overload (recommended): resolves DNS from transport_cfg.remote_host
///   - IP overload: accepts pre-resolved server_ip for exclusive-NIC deployments
///
/// Usage (hostname — most users):
///   auto eal = EalGuard::init(eal_argc, eal_argv);
///   eph::dpdk::ConnectorConfig conn_cfg{ .local_ip = "10.0.0.2", .gateway_ip = "10.0.0.1" };
///   eph::net::TransportConfig  tp_cfg{ .remote_host = "example.com", .remote_port = 443 };
///   auto result = eph::dpdk::connect(conn_cfg, tp_cfg);
///
/// Usage (pre-resolved IP — exclusive NIC mode):
///   uint32_t server_ip = ...; // resolve before EAL init
///   auto result = eph::dpdk::connect(conn_cfg, tp_cfg, server_ip);

#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <string>

#include <spdlog/spdlog.h>

#include <openssl/rand.h>

#include <arpa/inet.h>
#include <netdb.h>

#include <rte_ethdev.h>

#include "eph/dpdk/arp.hpp"
#include "eph/dpdk/net_header.hpp"
#include "eph/dpdk/platform.hpp"
#include "eph/dpdk/tcp.hpp"
#include "eph/dpdk/types.hpp"

namespace eph::dpdk {

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

/// Configuration for `connect()`.  Nests `PlatformConfig` so advanced users
/// can tune queue counts, descriptor counts, and mempool size.
struct ConnectorConfig {
    PlatformConfig platform{
        .port_id         = 0,
        .nb_rx_queues    = 1,
        .nb_tx_queues    = 1,
        .nb_rx_desc      = 256,
        .nb_tx_desc      = 512,
        .mbuf_pool_size  = 4095,
        .mbuf_cache_size = 256,
    };

    std::string local_ip;                 ///< Local IPv4 on DPDK port (required)
    std::string gateway_ip;               ///< Gateway IPv4 for ARP (required)
    uint16_t    local_port  = 0;          ///< 0 = random ephemeral (49152-65535)
    uint16_t    tx_queue_id = 0;
    uint16_t    rx_queue_id = 0;

    std::chrono::milliseconds arp_timeout{3000};
    std::chrono::milliseconds connect_timeout{5000};
};

// ─────────────────────────────────────────────────────────────────────────────
// Result — exposes all intermediate products
// ─────────────────────────────────────────────────────────────────────────────

/// Result of a successful `connect()`.  Exposes the Platform, Transport,
/// and resolved MAC addresses so callers can access stats, mempool, etc.
template <typename TransportType>
struct ConnectResult {
    Platform                            platform;
    std::unique_ptr<TransportType>      transport;
    rte_ether_addr                      local_mac;
    rte_ether_addr                      gateway_mac;
};

// ─────────────────────────────────────────────────────────────────────────────
// DNS resolution
// ─────────────────────────────────────────────────────────────────────────────

/// Resolve a hostname to an IPv4 address (host byte order) via the kernel
/// network stack.  If `host` is already a dotted-decimal IPv4 string
/// (e.g. "10.0.0.1"), it is parsed directly without a DNS query.
///
/// @warning With exclusive-mode PMDs, DPDK takes full ownership of the NIC
///   and the kernel network stack becomes unavailable.  In that case this
///   function will fail if called after `EalGuard::init()`.  Either:
///   - Call `resolve_hostname()` before `EalGuard::init()`, or
///   - Use the `connect()` overload that accepts a pre-resolved `server_ip`.
///   With bifurcated drivers (mlx5, ixgbe VF) or virtual devices (net_null,
///   net_pcap), DNS remains available after EAL init.
///
/// @param host  Hostname or dotted-decimal IPv4 string
/// @return IPv4 address in host byte order, or error string
inline std::expected<uint32_t, std::string>
resolve_hostname(const std::string& host) {
    // Fast path: dotted-decimal IPv4
    uint32_t ip = net::parse_ipv4(host.c_str());
    if (ip != 0) return ip;

    // Kernel DNS resolution
    struct addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = nullptr;
    int err = getaddrinfo(host.c_str(), nullptr, &hints, &result);
    if (err != 0 || !result) {
        return std::unexpected(std::format(
            "DNS resolution failed for '{}': {}. "
            "If using an exclusive-mode PMD, resolve the hostname before "
            "EalGuard::init() and use the server_ip overload of connect().",
            host, gai_strerror(err)));
    }

    auto* addr = reinterpret_cast<struct sockaddr_in*>(result->ai_addr);
    ip = ntohl(addr->sin_addr.s_addr);
    freeaddrinfo(result);

    SPDLOG_DEBUG("DNS resolved: {} -> {}", host, net::format_ipv4(ip).data());
    return ip;
}

// ─────────────────────────────────────────────────────────────────────────────
// connect()
// ─────────────────────────────────────────────────────────────────────────────

/// One-shot connection helper: Platform → MAC → ARP → TcpSession → Transport.
///
/// DNS resolution must happen BEFORE EAL initialization (because DPDK may
/// take over the NIC, disabling the kernel network stack).  Pass the
/// pre-resolved server IPv4 as `server_ip` (host byte order).
///
/// @tparam TransportType  Transport alias — defaults to `DpdkTransport`
///                        (512-byte payload, 1024-deep queue).  Use
///                        `DpdkLargeTransport` for bulk data, etc.
/// @param cfg             Connector configuration (platform, IPs, ports)
/// @param transport_cfg   Generic transport config (TLS, WS, reconnect)
/// @param server_ip       Pre-resolved server IPv4 in host byte order
/// @return ConnectResult on success, error string on failure
template <typename TransportType = DpdkTransport>
std::expected<ConnectResult<TransportType>, std::string>
connect(const ConnectorConfig& cfg,
        const TransportConfig& transport_cfg,
        uint32_t server_ip) {

    // Validate required fields
    if (cfg.local_ip.empty()) {
        return std::unexpected("ConnectorConfig: local_ip is required");
    }
    if (cfg.gateway_ip.empty()) {
        return std::unexpected("ConnectorConfig: gateway_ip is required");
    }
    if (server_ip == 0) {
        return std::unexpected("server_ip must be a valid IPv4 address (host byte order)");
    }

    SPDLOG_DEBUG("dpdk::connect: local={}, gateway={}, server={}",
                 cfg.local_ip, cfg.gateway_ip,
                 net::format_ipv4(server_ip).data());

    // 1. Platform
    auto platform = Platform::create(cfg.platform);
    if (!platform) {
        return std::unexpected(
            std::format("Platform creation failed: {}", platform.error()));
    }

    // 2. Source MAC
    rte_ether_addr src_mac{};
    if (rte_eth_macaddr_get(cfg.platform.port_id, &src_mac) != 0) {
        return std::unexpected(
            std::format("Failed to get MAC for port {}", cfg.platform.port_id));
    }

    // 3. Parse IPs
    uint32_t local_ip   = net::parse_ipv4(cfg.local_ip.c_str());
    uint32_t gateway_ip = net::parse_ipv4(cfg.gateway_ip.c_str());
    if (local_ip == 0) {
        return std::unexpected(
            std::format("Invalid local_ip: '{}'", cfg.local_ip));
    }
    if (gateway_ip == 0) {
        return std::unexpected(
            std::format("Invalid gateway_ip: '{}'", cfg.gateway_ip));
    }

    // 4. ARP resolve gateway
    auto arp_result = arp::resolve(
        cfg.platform.port_id, cfg.rx_queue_id, platform->mempool(),
        src_mac, local_ip, gateway_ip, cfg.arp_timeout);
    if (!arp_result) {
        return std::unexpected(
            std::format("ARP resolution failed: {}", arp_result.error()));
    }
    rte_ether_addr dst_mac = *arp_result;

    // 5. Ephemeral source port
    uint16_t src_port = cfg.local_port;
    if (src_port == 0) {
        uint16_t rnd;
        RAND_bytes(reinterpret_cast<uint8_t*>(&rnd), sizeof(rnd));
        src_port = 49152 + (rnd % 16384);
        SPDLOG_DEBUG("dpdk::connect: ephemeral port {}", src_port);
    }

    // 6. TCP config + factory
    TcpConfig tcp_cfg{
        .tuple = {
            .src_ip   = local_ip,
            .dst_ip   = server_ip,
            .src_port = src_port,
            .dst_port = transport_cfg.remote_port,
        },
        .src_mac     = src_mac,
        .dst_mac     = dst_mac,
        .port_id     = cfg.platform.port_id,
        .tx_queue_id = cfg.tx_queue_id,
        .rx_queue_id = cfg.rx_queue_id,
    };

    auto* mempool = platform->mempool();
    auto connect_timeout = cfg.connect_timeout;

    auto tcp_factory = [tcp_cfg, mempool, connect_timeout]()
        -> std::expected<std::unique_ptr<TcpSession<>>, std::string> {
        auto tcp = std::make_unique<TcpSession<>>(tcp_cfg, mempool);
        auto r = tcp->connect(connect_timeout);
        if (!r) return std::unexpected(r.error());
        return tcp;
    };

    // 7. Create transport (TCP + TLS + WS handshake)
    auto transport = TransportType::create(
        std::move(tcp_factory), transport_cfg);
    if (!transport) {
        return std::unexpected(
            std::format("Transport creation failed: {}", transport.error().message()));
    }

    SPDLOG_DEBUG("dpdk::connect: connection established");

    return ConnectResult<TransportType>{
        .platform    = std::move(*platform),
        .transport   = std::move(*transport),
        .local_mac   = src_mac,
        .gateway_mac = dst_mac,
    };
}

/// Connect using an existing Platform instance (for multi-connection scenarios).
///
/// Reuses an already-initialized Platform instead of creating a new one.
/// This is useful when establishing multiple connections on the same NIC port,
/// since Platform holds the mempool and port configuration that should be
/// shared across connections.
///
/// @param platform         Already-initialized Platform (must outlive the Transport)
/// @param cfg              Connector configuration (local_ip, gateway_ip, etc.)
/// @param transport_cfg    Generic transport config (TLS, WS, reconnect)
/// @param server_ip        Pre-resolved server IPv4 in host byte order
template <typename TransportType = DpdkTransport>
std::expected<std::unique_ptr<TransportType>, std::string>
connect(Platform& platform,
        const ConnectorConfig& cfg,
        const TransportConfig& transport_cfg,
        uint32_t server_ip) {

    if (cfg.local_ip.empty()) {
        return std::unexpected("ConnectorConfig: local_ip is required");
    }
    if (cfg.gateway_ip.empty()) {
        return std::unexpected("ConnectorConfig: gateway_ip is required");
    }
    if (server_ip == 0) {
        return std::unexpected("server_ip must be a valid IPv4 address (host byte order)");
    }

    SPDLOG_DEBUG("dpdk::connect(platform&): local={}, gateway={}, server={}",
                 cfg.local_ip, cfg.gateway_ip,
                 net::format_ipv4(server_ip).data());

    // Source MAC
    rte_ether_addr src_mac{};
    if (rte_eth_macaddr_get(cfg.platform.port_id, &src_mac) != 0) {
        return std::unexpected(
            std::format("Failed to get MAC for port {}", cfg.platform.port_id));
    }

    // Parse IPs
    uint32_t local_ip   = net::parse_ipv4(cfg.local_ip.c_str());
    uint32_t gateway_ip = net::parse_ipv4(cfg.gateway_ip.c_str());
    if (local_ip == 0) {
        return std::unexpected(std::format("Invalid local_ip: '{}'", cfg.local_ip));
    }
    if (gateway_ip == 0) {
        return std::unexpected(std::format("Invalid gateway_ip: '{}'", cfg.gateway_ip));
    }

    // ARP resolve gateway
    auto arp_result = arp::resolve(
        cfg.platform.port_id, cfg.rx_queue_id, platform.mempool(),
        src_mac, local_ip, gateway_ip, cfg.arp_timeout);
    if (!arp_result) {
        return std::unexpected(
            std::format("ARP resolution failed: {}", arp_result.error()));
    }
    rte_ether_addr dst_mac = *arp_result;

    // Ephemeral source port
    uint16_t src_port = cfg.local_port;
    if (src_port == 0) {
        uint16_t rnd;
        RAND_bytes(reinterpret_cast<uint8_t*>(&rnd), sizeof(rnd));
        src_port = 49152 + (rnd % 16384);
        SPDLOG_DEBUG("dpdk::connect: ephemeral port {}", src_port);
    }

    // TCP config + factory
    TcpConfig tcp_cfg{
        .tuple = {
            .src_ip   = local_ip,
            .dst_ip   = server_ip,
            .src_port = src_port,
            .dst_port = transport_cfg.remote_port,
        },
        .src_mac     = src_mac,
        .dst_mac     = dst_mac,
        .port_id     = cfg.platform.port_id,
        .tx_queue_id = cfg.tx_queue_id,
        .rx_queue_id = cfg.rx_queue_id,
    };

    auto* mempool = platform.mempool();
    auto connect_timeout = cfg.connect_timeout;

    auto tcp_factory = [tcp_cfg, mempool, connect_timeout]()
        -> std::expected<std::unique_ptr<TcpSession<>>, std::string> {
        auto tcp = std::make_unique<TcpSession<>>(tcp_cfg, mempool);
        auto r = tcp->connect(connect_timeout);
        if (!r) return std::unexpected(r.error());
        return tcp;
    };

    // Create transport (TCP + TLS + WS handshake)
    auto transport = TransportType::create(
        std::move(tcp_factory), transport_cfg);
    if (!transport) {
        return std::unexpected(
            std::format("Transport creation failed: {}", transport.error().message()));
    }

    SPDLOG_DEBUG("dpdk::connect(platform&): connection established");
    return std::move(*transport);
}

/// Convenience overload: resolves hostname, uses existing Platform.
template <typename TransportType = DpdkTransport>
std::expected<std::unique_ptr<TransportType>, std::string>
connect(Platform& platform,
        const ConnectorConfig& cfg,
        const TransportConfig& transport_cfg) {
    if (transport_cfg.remote_host.empty()) {
        return std::unexpected(
            "TransportConfig: remote_host is required for hostname-based connect");
    }
    auto ip = resolve_hostname(transport_cfg.remote_host);
    if (!ip) return std::unexpected(ip.error());
    return connect<TransportType>(platform, cfg, transport_cfg, *ip);
}

/// Convenience overload: resolves `transport_cfg.remote_host` via DNS,
/// then delegates to the `server_ip` overload.
///
/// @warning DNS resolution uses the kernel network stack via `getaddrinfo()`.
///   See `resolve_hostname()` for caveats with exclusive-mode PMDs.
template <typename TransportType = DpdkTransport>
std::expected<ConnectResult<TransportType>, std::string>
connect(const ConnectorConfig& cfg,
        const TransportConfig& transport_cfg) {
    if (transport_cfg.remote_host.empty()) {
        return std::unexpected(
            "TransportConfig: remote_host is required for hostname-based connect");
    }

    auto ip = resolve_hostname(transport_cfg.remote_host);
    if (!ip) return std::unexpected(ip.error());

    return connect<TransportType>(cfg, transport_cfg, *ip);
}

} // namespace eph::dpdk
