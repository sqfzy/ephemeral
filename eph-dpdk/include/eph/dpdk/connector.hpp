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
/// Multiple `connect()` overloads, from simplest to most configurable:
///
/// Usage (simplest — hostname + required DPDK endpoint):
///   auto eal = EalGuard::init(eal_argc, eal_argv);
///   auto result = eph::dpdk::connect("example.com", {"10.0.0.2", "10.0.0.1"});
///
/// Usage (with options):
///   auto result = eph::dpdk::connect("example.com", {"10.0.0.2", "10.0.0.1"},
///                                    {.local_port = 5000});
///
/// Usage (full control — custom TransportConfig):
///   DpdkEndpoint ep{"10.0.0.2", "10.0.0.1"};
///   eph::net::TransportConfig tp_cfg{.remote_host = "example.com", .remote_port = 8443};
///   auto result = eph::dpdk::connect(ep, tp_cfg);
///
/// Usage (pre-resolved IP — exclusive NIC mode):
///   uint32_t server_ip = ...; // resolve before EAL init
///   auto result = eph::dpdk::connect(ep, tp_cfg, server_ip);

#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <string>

#include <spdlog/spdlog.h>

#include <openssl/rand.h>

#include <arpa/inet.h>
#include <netdb.h>

#include <rte_ethdev.h>

#include "eph/dpdk/arp.hpp"
#include "eph/dpdk/dns.hpp"
#include "eph/dpdk/net_header.hpp"
#include "eph/dpdk/platform.hpp"
#include "eph/dpdk/tcp.hpp"
#include "eph/dpdk/types.hpp"

namespace eph::dpdk {

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────

/// IANA ephemeral port range (RFC 6335 §6): 49152–65535.
inline constexpr uint16_t kEphemeralPortMin   = 49152;
inline constexpr uint16_t kEphemeralPortRange = 16384; // 65535 - 49152 + 1

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

/// Required DPDK network identity — no defaults, must be fully specified.
/// Omitting either field is a compile error (aggregate with no defaults).
struct DpdkEndpoint {
    std::string local_ip;    ///< Local IPv4 on DPDK port
    std::string gateway_ip;  ///< Gateway IPv4 for ARP resolution
};

/// Optional connection settings — all fields have sensible defaults.
/// Nests `PlatformConfig` so advanced users can tune queue counts,
/// descriptor counts, and mempool size.
struct ConnectorOptions {
    PlatformConfig platform{
        .port_id         = 0,
        .nb_rx_queues    = 1,
        .nb_tx_queues    = 1,
        .nb_rx_desc      = 256,
        .nb_tx_desc      = 512,
        .mbuf_pool_size  = 4095,
        .mbuf_cache_size = 256,
    };
    uint16_t local_port  = 0;  ///< 0 = random ephemeral (49152-65535)
    uint16_t tx_queue_id = 0;
    uint16_t rx_queue_id = 0;
    std::chrono::milliseconds arp_timeout{3000};
    std::chrono::milliseconds connect_timeout{5000};
    dns::DnsConfig dns{};  ///< DNS config for DPDK DNS fallback (default: 8.8.8.8)
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
    SPDLOG_DEBUG("Resolving hostname: '{}'", host);

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
// Internal: shared connection setup logic
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

/// Intermediate products from prepare_connection(), consumed by connect()
/// to create the Transport and (optionally) return MAC addresses.
struct ConnectionSetup {
    rte_ether_addr src_mac;
    rte_ether_addr dst_mac;
    std::function<std::expected<std::unique_ptr<TcpSession<>>, std::string>()> tcp_factory;
};

/// Shared logic: validate → get MAC → parse IPs → ARP → ephemeral port
/// → build TcpConfig + factory.  Used by all connect() overloads.
inline std::expected<ConnectionSetup, std::string>
prepare_connection(const DpdkEndpoint& ep,
                   const ConnectorOptions& opts,
                   const TransportConfig& transport_cfg,
                   uint32_t server_ip,
                   rte_mempool* mempool) {
    // Validate required fields
    if (ep.local_ip.empty()) {
        return std::unexpected("DpdkEndpoint: local_ip is required");
    }
    if (ep.gateway_ip.empty()) {
        return std::unexpected("DpdkEndpoint: gateway_ip is required");
    }
    if (server_ip == 0) {
        return std::unexpected("server_ip must be a valid IPv4 address (host byte order)");
    }

    // Source MAC
    rte_ether_addr src_mac{};
    if (rte_eth_macaddr_get(opts.platform.port_id, &src_mac) != 0) {
        return std::unexpected(
            std::format("Failed to get MAC for port {}", opts.platform.port_id));
    }

    // Parse IPs
    uint32_t local_ip   = net::parse_ipv4(ep.local_ip.c_str());
    uint32_t gateway_ip = net::parse_ipv4(ep.gateway_ip.c_str());
    if (local_ip == 0) {
        return std::unexpected(std::format("Invalid local_ip: '{}'", ep.local_ip));
    }
    if (gateway_ip == 0) {
        return std::unexpected(std::format("Invalid gateway_ip: '{}'", ep.gateway_ip));
    }

    // ARP resolve gateway
    auto arp_result = arp::resolve(
        opts.platform.port_id, opts.rx_queue_id, mempool,
        src_mac, local_ip, gateway_ip, opts.arp_timeout);
    if (!arp_result) {
        return std::unexpected(
            std::format("ARP resolution failed: {}", arp_result.error()));
    }
    rte_ether_addr dst_mac = *arp_result;

    // Ephemeral source port
    uint16_t src_port = opts.local_port;
    if (src_port == 0) {
        uint16_t rnd;
        if (RAND_bytes(reinterpret_cast<uint8_t*>(&rnd), sizeof(rnd)) != 1) {
            return std::unexpected("CSPRNG failure: RAND_bytes failed for ephemeral port");
        }
        src_port = kEphemeralPortMin + (rnd % kEphemeralPortRange);
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
        .port_id     = opts.platform.port_id,
        .tx_queue_id = opts.tx_queue_id,
        .rx_queue_id = opts.rx_queue_id,
    };

    auto connect_timeout = opts.connect_timeout;

    auto tcp_factory = [tcp_cfg, mempool, connect_timeout]()
        -> std::expected<std::unique_ptr<TcpSession<>>, std::string> {
        auto tcp = std::make_unique<TcpSession<>>(tcp_cfg, mempool);
        auto r = tcp->connect(connect_timeout);
        if (!r) return std::unexpected(r.error());
        return tcp;
    };

    return ConnectionSetup{
        .src_mac     = src_mac,
        .dst_mac     = dst_mac,
        .tcp_factory = std::move(tcp_factory),
    };
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// connect()
// ─────────────────────────────────────────────────────────────────────────────

/// Full-control connect: pre-resolved server IP + custom TransportConfig.
///
/// @tparam TransportType  Transport alias — defaults to `DpdkTransport`
/// @param ep              Required DPDK endpoint (local_ip, gateway_ip)
/// @param transport_cfg   Generic transport config (TLS, WS, reconnect)
/// @param server_ip       Pre-resolved server IPv4 in host byte order
/// @param opts            Optional settings (platform, ports, timeouts)
template <typename TransportType = DpdkTransport>
std::expected<ConnectResult<TransportType>, std::string>
connect(const DpdkEndpoint& ep,
        const TransportConfig& transport_cfg,
        uint32_t server_ip,
        const ConnectorOptions& opts = {}) {

    SPDLOG_DEBUG("dpdk::connect: local={}, gateway={}, server={}",
                 ep.local_ip, ep.gateway_ip,
                 net::format_ipv4(server_ip).data());

    auto platform = Platform::create(opts.platform);
    if (!platform) {
        return std::unexpected(
            std::format("Platform creation failed: {}", platform.error()));
    }

    auto setup = detail::prepare_connection(
        ep, opts, transport_cfg, server_ip, platform->mempool());
    if (!setup) return std::unexpected(setup.error());

    auto transport = TransportType::create(
        std::move(setup->tcp_factory), transport_cfg);
    if (!transport) {
        return std::unexpected(
            std::format("Transport creation failed: {}", transport.error().message()));
    }

    SPDLOG_DEBUG("dpdk::connect: connection established");

    return ConnectResult<TransportType>{
        .platform    = std::move(*platform),
        .transport   = std::move(*transport),
        .local_mac   = setup->src_mac,
        .gateway_mac = setup->dst_mac,
    };
}

/// DNS-resolving connect: resolves `transport_cfg.remote_host` automatically.
///
/// Resolution strategy:
///   1. Try kernel DNS (getaddrinfo) — works with bifurcated drivers
///   2. Fall back to DPDK DNS — creates Platform, does ARP for gateway,
///      sends UDP DNS query through the NIC
///
/// The DPDK DNS fallback uses 8.8.8.8 as the default nameserver.
/// Configure via `ConnectorOptions::dns`.
template <typename TransportType = DpdkTransport>
std::expected<ConnectResult<TransportType>, std::string>
connect(const DpdkEndpoint& ep,
        const TransportConfig& transport_cfg,
        const ConnectorOptions& opts = {}) {
    if (transport_cfg.remote_host.empty()) {
        return std::unexpected(
            "TransportConfig: remote_host is required for hostname-based connect");
    }

    // Try kernel DNS first (zero cost if driver allows it)
    auto ip = resolve_hostname(transport_cfg.remote_host);
    if (ip) {
        return connect<TransportType>(ep, transport_cfg, *ip, opts);
    }

    // Kernel DNS failed — fall back to DPDK DNS.
    // Validate config before creating Platform to fail fast on bad input.
    SPDLOG_DEBUG("dpdk::connect: kernel DNS failed ({}), trying DPDK DNS",
                 ip.error());

    uint32_t local_ip   = net::parse_ipv4(ep.local_ip.c_str());
    uint32_t gateway_ip = net::parse_ipv4(ep.gateway_ip.c_str());
    if (local_ip == 0) {
        return std::unexpected(std::format("Invalid local_ip: '{}'", ep.local_ip));
    }
    if (gateway_ip == 0) {
        return std::unexpected(std::format("Invalid gateway_ip: '{}'", ep.gateway_ip));
    }

    // Need Platform + ARP before we can send UDP DNS queries.
    auto platform = Platform::create(opts.platform);
    if (!platform) {
        return std::unexpected(
            std::format("Platform creation failed: {}", platform.error()));
    }

    rte_ether_addr src_mac{};
    if (rte_eth_macaddr_get(opts.platform.port_id, &src_mac) != 0) {
        return std::unexpected(
            std::format("Failed to get MAC for port {}", opts.platform.port_id));
    }

    // ARP for gateway MAC (needed to route DNS UDP packets)
    auto gw_mac = arp::resolve(
        opts.platform.port_id, opts.rx_queue_id, platform->mempool(),
        src_mac, local_ip, gateway_ip, opts.arp_timeout);
    if (!gw_mac) {
        return std::unexpected(std::format(
            "DNS fallback: ARP for gateway {} failed: {}",
            ep.gateway_ip, gw_mac.error()));
    }

    // DNS resolve over DPDK
    auto dpdk_ip = dns::resolve(
        opts.platform.port_id, opts.rx_queue_id, platform->mempool(),
        src_mac, *gw_mac, local_ip,
        transport_cfg.remote_host, opts.dns);
    if (!dpdk_ip) {
        return std::unexpected(std::format(
            "DNS resolution failed (both kernel and DPDK): kernel={}, dpdk={}",
            ip.error(), dpdk_ip.error()));
    }

    // Build TCP connection reusing the gateway MAC we already resolved.
    // prepare_connection() would ARP again internally, so we construct
    // the TcpConfig directly to avoid the redundant ARP.
    uint16_t src_port = opts.local_port;
    if (src_port == 0) {
        uint16_t rnd;
        if (RAND_bytes(reinterpret_cast<uint8_t*>(&rnd), sizeof(rnd)) != 1) {
            return std::unexpected("CSPRNG failure: RAND_bytes failed for ephemeral port");
        }
        src_port = kEphemeralPortMin + (rnd % kEphemeralPortRange);
    }

    TcpConfig tcp_cfg{
        .tuple = {
            .src_ip   = local_ip,
            .dst_ip   = *dpdk_ip,
            .src_port = src_port,
            .dst_port = transport_cfg.remote_port,
        },
        .src_mac     = src_mac,
        .dst_mac     = *gw_mac,
        .port_id     = opts.platform.port_id,
        .tx_queue_id = opts.tx_queue_id,
        .rx_queue_id = opts.rx_queue_id,
    };

    auto connect_timeout = opts.connect_timeout;
    auto tcp_factory = [tcp_cfg, mempool = platform->mempool(), connect_timeout]()
        -> std::expected<std::unique_ptr<TcpSession<>>, std::string> {
        auto tcp = std::make_unique<TcpSession<>>(tcp_cfg, mempool);
        auto r = tcp->connect(connect_timeout);
        if (!r) return std::unexpected(r.error());
        return tcp;
    };

    auto transport = TransportType::create(
        std::move(tcp_factory), transport_cfg);
    if (!transport) {
        return std::unexpected(
            std::format("Transport creation failed: {}", transport.error().message()));
    }

    SPDLOG_DEBUG("dpdk::connect: connection established (via DPDK DNS)");

    return ConnectResult<TransportType>{
        .platform    = std::move(*platform),
        .transport   = std::move(*transport),
        .local_mac   = src_mac,
        .gateway_mac = *gw_mac,
    };
}

/// Simplest connect: hostname + required endpoint + optional settings.
///
/// Builds a default `TransportConfig` (port 443, TLS on) from the hostname.
/// Required fields (local_ip, gateway_ip) are compile-time enforced —
/// DpdkEndpoint has no defaults.
///
/// @code
///   auto r = eph::dpdk::connect("example.com", {"10.0.0.2", "10.0.0.1"});
///   auto r = eph::dpdk::connect("example.com", {"10.0.0.2", "10.0.0.1"},
///                               {.local_port = 5000});
/// @endcode
template <typename TransportType = DpdkTransport>
std::expected<ConnectResult<TransportType>, std::string>
connect(std::string_view host, const DpdkEndpoint& ep,
        const ConnectorOptions& opts = {}) {
    TransportConfig transport_cfg{
        .remote_host = std::string(host),
    };
    return connect<TransportType>(ep, transport_cfg, opts);
}

/// Connect with existing Platform + pre-resolved IP.
template <typename TransportType = DpdkTransport>
std::expected<std::unique_ptr<TransportType>, std::string>
connect(Platform& platform,
        const DpdkEndpoint& ep,
        const TransportConfig& transport_cfg,
        uint32_t server_ip,
        const ConnectorOptions& opts = {}) {

    SPDLOG_DEBUG("dpdk::connect(platform&): local={}, gateway={}, server={}",
                 ep.local_ip, ep.gateway_ip,
                 net::format_ipv4(server_ip).data());

    auto setup = detail::prepare_connection(
        ep, opts, transport_cfg, server_ip, platform.mempool());
    if (!setup) return std::unexpected(setup.error());

    auto transport = TransportType::create(
        std::move(setup->tcp_factory), transport_cfg);
    if (!transport) {
        return std::unexpected(
            std::format("Transport creation failed: {}", transport.error().message()));
    }

    SPDLOG_DEBUG("dpdk::connect(platform&): connection established");
    return std::move(*transport);
}

/// Connect with existing Platform + DNS resolution (kernel, DPDK fallback).
template <typename TransportType = DpdkTransport>
std::expected<std::unique_ptr<TransportType>, std::string>
connect(Platform& platform,
        const DpdkEndpoint& ep,
        const TransportConfig& transport_cfg,
        const ConnectorOptions& opts = {}) {
    if (transport_cfg.remote_host.empty()) {
        return std::unexpected(
            "TransportConfig: remote_host is required for hostname-based connect");
    }

    // Try kernel DNS first
    auto ip = resolve_hostname(transport_cfg.remote_host);
    if (ip) {
        return connect<TransportType>(platform, ep, transport_cfg, *ip, opts);
    }

    // Fall back to DPDK DNS
    SPDLOG_DEBUG("dpdk::connect(platform&): kernel DNS failed, trying DPDK DNS");

    uint32_t local_ip   = net::parse_ipv4(ep.local_ip.c_str());
    uint32_t gateway_ip = net::parse_ipv4(ep.gateway_ip.c_str());
    if (local_ip == 0 || gateway_ip == 0) {
        return std::unexpected("Invalid local_ip or gateway_ip for DPDK DNS fallback");
    }

    rte_ether_addr src_mac{};
    if (rte_eth_macaddr_get(opts.platform.port_id, &src_mac) != 0) {
        return std::unexpected("Failed to get MAC for DPDK DNS fallback");
    }

    auto gw_mac = arp::resolve(
        opts.platform.port_id, opts.rx_queue_id, platform.mempool(),
        src_mac, local_ip, gateway_ip, opts.arp_timeout);
    if (!gw_mac) {
        return std::unexpected(std::format(
            "DNS fallback: ARP for gateway failed: {}", gw_mac.error()));
    }

    auto dpdk_ip = dns::resolve(
        opts.platform.port_id, opts.rx_queue_id, platform.mempool(),
        src_mac, *gw_mac, local_ip,
        transport_cfg.remote_host, opts.dns);
    if (!dpdk_ip) {
        return std::unexpected(std::format(
            "DNS failed (kernel: {}, DPDK: {})", ip.error(), dpdk_ip.error()));
    }

    return connect<TransportType>(platform, ep, transport_cfg, *dpdk_ip, opts);
}

/// Simplest connect with existing Platform.
template <typename TransportType = DpdkTransport>
std::expected<std::unique_ptr<TransportType>, std::string>
connect(Platform& platform, std::string_view host, const DpdkEndpoint& ep,
        const ConnectorOptions& opts = {}) {
    TransportConfig transport_cfg{
        .remote_host = std::string(host),
    };
    return connect<TransportType>(platform, ep, transport_cfg, opts);
}

} // namespace eph::dpdk
