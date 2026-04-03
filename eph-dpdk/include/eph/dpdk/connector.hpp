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
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <string>

#include <optional>

#include <eph/core/detail/json_escape.hpp>
#include <eph/core/detail/string_checks.hpp>

#include <spdlog/sinks/stdout_color_sinks.h>
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

namespace detail {
inline spdlog::logger* connector_logger() {
    static auto l = [] {
        auto lg = spdlog::get("dpdk.connector");
        if (!lg) lg = spdlog::stdout_color_mt("dpdk.connector");
        return lg;
    }();
    return l.get();
}
} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────

/// IANA ephemeral port range (RFC 6335 §6): 49152–65535.
inline constexpr uint16_t kEphemeralPortMin   = 49152;
inline constexpr uint16_t kEphemeralPortRange = 16384; // 65535 - 49152 + 1

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Required DPDK network identity for connection setup.
///
/// Both fields are mandatory — omitting either prevents connect() from
/// building valid Ethernet/IP headers. No defaults are provided to force
/// explicit specification at the call site.
struct DpdkEndpoint {
    std::string local_ip;    ///< Local IPv4 on DPDK port
    std::string gateway_ip;  ///< Gateway IPv4 for ARP resolution

    /// Validate endpoint, returning error description or empty on success.
    ///
    /// @return empty string_view if valid; otherwise a diagnostic pointing to
    ///         a string literal (safe to store — all returned views have static
    ///         storage duration).
    [[nodiscard]] constexpr std::string_view validate() const noexcept {
        if (local_ip.empty())   return "local_ip must not be empty";
        if (gateway_ip.empty()) return "gateway_ip must not be empty";
        // Reject control characters to prevent log injection.
        if (core::detail::contains_control_chars(local_ip))
            return "local_ip contains control characters";
        if (core::detail::contains_control_chars(gateway_ip))
            return "gateway_ip contains control characters";
        return {};
    }

    [[nodiscard]] friend bool operator==(const DpdkEndpoint&,
                                          const DpdkEndpoint&) = default;

    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{\"local_ip\":\"{}\",\"gateway_ip\":\"{}\"}}",
            core::detail::json_escape(local_ip),
            core::detail::json_escape(gateway_ip));
    }

    [[nodiscard]] std::string dump() const {
        return std::format("DpdkEndpoint(local={}, gw={})",
                           local_ip, gateway_ip);
    }

    /// Check for non-fatal contradictions or likely misconfigurations.
    /// Returns a list of warning messages (empty if no issues).
    /// Unlike validate() which blocks operation, these are advisory.
    [[nodiscard]] std::vector<std::string> warnings() const {
        std::vector<std::string> w;
        // Loopback addresses are not reachable via DPDK (bypasses kernel)
        if (local_ip.starts_with("127."))
            w.emplace_back("local_ip starts with 127.x (loopback) -- "
                           "DPDK bypasses the kernel, loopback traffic "
                           "will not reach the NIC");
        if (gateway_ip.starts_with("127."))
            w.emplace_back("gateway_ip starts with 127.x (loopback) -- "
                           "DPDK bypasses the kernel, ARP for loopback "
                           "gateway will fail");
        // Same IP for local and gateway is unusual
        if (!local_ip.empty() && local_ip == gateway_ip)
            w.emplace_back(std::format(
                "local_ip == gateway_ip ({}) -- self-gateway is unusual "
                "and may indicate a configuration error", local_ip));
        return w;
    }
};

/// @brief Optional connection settings — all fields have sensible defaults.
///
/// Nests PlatformConfig so advanced users can tune NIC queue counts,
/// descriptor ring sizes, and mempool size. Also controls ARP/DNS timeouts,
/// TCP connect timeout, and optional pre-resolved gateway MAC.
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
    /// Pre-resolved gateway MAC. When set, ARP resolution is skipped.
    /// Use this for multi-connection scenarios where the first connection
    /// resolves the gateway MAC and subsequent connections reuse it.
    std::optional<rte_ether_addr> gateway_mac{};
    std::chrono::milliseconds arp_timeout{3000};
    std::chrono::milliseconds connect_timeout{5000};
    dns::DnsConfig dns{};  ///< DNS config for DPDK DNS fallback (default: 8.8.8.8)

    /// Validate options, returning error description or empty on success.
    ///
    /// @return empty string_view if valid; otherwise a diagnostic pointing to
    ///         a string literal (safe to store — all returned views have static
    ///         storage duration).
    [[nodiscard]] constexpr std::string_view validate() const noexcept {
        if (auto err = validate_config(platform); !err.empty()) return err;
        if (tx_queue_id >= platform.nb_tx_queues)
            return "tx_queue_id must be < platform.nb_tx_queues";
        if (rx_queue_id >= platform.nb_rx_queues)
            return "rx_queue_id must be < platform.nb_rx_queues";
        if (arp_timeout.count() <= 0)
            return "arp_timeout must be positive";
        if (connect_timeout.count() <= 0)
            return "connect_timeout must be positive";
        if (auto err = dns.validate(); !err.empty()) return err;
        return {};
    }

    [[nodiscard]] friend bool operator==(const ConnectorOptions& a,
                                          const ConnectorOptions& b) noexcept {
        // rte_ether_addr is a C struct without operator==, so we compare
        // the optional<rte_ether_addr> manually via memcmp.
        auto mac_eq = [](const std::optional<rte_ether_addr>& x,
                         const std::optional<rte_ether_addr>& y) -> bool {
            if (x.has_value() != y.has_value()) return false;
            if (!x.has_value()) return true;
            return std::memcmp(&*x, &*y, sizeof(rte_ether_addr)) == 0;
        };
        return a.platform == b.platform
            && a.local_port == b.local_port
            && a.tx_queue_id == b.tx_queue_id
            && a.rx_queue_id == b.rx_queue_id
            && mac_eq(a.gateway_mac, b.gateway_mac)
            && a.arp_timeout == b.arp_timeout
            && a.connect_timeout == b.connect_timeout
            && a.dns == b.dns;
    }

    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{\"platform\":{},\"local_port\":{},\"tx_queue_id\":{},"
            "\"rx_queue_id\":{},\"arp_timeout_ms\":{},\"connect_timeout_ms\":{},"
            "\"dns\":{}}}",
            platform.to_json(), local_port, tx_queue_id,
            rx_queue_id, arp_timeout.count(), connect_timeout.count(),
            dns.to_json());
    }

    [[nodiscard]] std::string dump() const {
        return std::format(
            "ConnectorOptions(port={}, queues=tx{}/rx{}, "
            "arp_timeout={}ms, connect_timeout={}ms, dns={})\n  {}",
            local_port, tx_queue_id, rx_queue_id,
            arp_timeout.count(), connect_timeout.count(),
            net::format_ipv4(dns.nameserver_ip).data(),
            platform.dump());
    }

    /// Check for non-fatal contradictions or likely misconfigurations.
    /// Returns a list of warning messages (empty if no issues).
    /// Unlike validate() which blocks construction, these are advisory.
    [[nodiscard]] std::vector<std::string> warnings() const {
        std::vector<std::string> w;
        // Short ARP timeout may fail on loaded switches
        if (arp_timeout.count() < 500)
            w.emplace_back(std::format(
                "arp_timeout={}ms is very short -- ARP resolution may "
                "fail on loaded switches", arp_timeout.count()));
        // Short connect timeout may fail on high-latency links
        if (connect_timeout.count() < 1000)
            w.emplace_back(std::format(
                "connect_timeout={}ms is very short -- TCP handshake may "
                "fail on high-latency or congested links",
                connect_timeout.count()));
        // Connect timeout shorter than ARP timeout is suspicious
        if (connect_timeout < arp_timeout)
            w.emplace_back(std::format(
                "connect_timeout ({}ms) < arp_timeout ({}ms) -- "
                "connect may time out before ARP completes",
                connect_timeout.count(), arp_timeout.count()));
        // Well-known ports (< 1024) as local port is unusual for DPDK
        if (local_port > 0 && local_port < 1024)
            w.emplace_back(std::format(
                "local_port={} is in the well-known range (< 1024) -- "
                "unusual for a client connection", local_port));
        // Small mempool may exhaust during connection setup
        if (platform.mbuf_pool_size < 1023)
            w.emplace_back(std::format(
                "mbuf_pool_size={} is small -- may exhaust during "
                "concurrent ARP/DNS/TCP operations",
                platform.mbuf_pool_size));
        return w;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Result — exposes all intermediate products
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Result of a successful connect() call.
///
/// Exposes all intermediate products (Platform, Transport, MAC addresses)
/// so callers can access NIC stats, mempool, and reuse the gateway MAC
/// for subsequent connections on the same port.
///
/// @tparam TransportType  The transport type created by connect()
template <typename TransportType>
struct ConnectResult {
    Platform                            platform;   ///< Initialized DPDK platform (owns port + mempool)
    std::unique_ptr<TransportType>      transport;  ///< Connected transport (owns TCP session)
    rte_ether_addr                      local_mac;  ///< NIC's MAC address
    rte_ether_addr                      gateway_mac; ///< Resolved gateway MAC (from ARP or pre-configured)
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
[[nodiscard]] inline std::expected<uint32_t, std::string>
resolve_hostname(const std::string& host) {
    [[maybe_unused]] auto log = detail::connector_logger();
    SPDLOG_LOGGER_DEBUG(log, "Resolving hostname: '{}'", host);

    if (host.empty()) {
        SPDLOG_LOGGER_DEBUG(log, "resolve_hostname: empty hostname");
        return std::unexpected("Hostname is empty");
    }

    // RFC 1035 §3.1: total name length must not exceed 253 characters.
    if (host.size() > 253) {
        return std::unexpected(
            std::format("Hostname too long ({} > 253 chars)", host.size()));
    }

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

    if (!result->ai_addr) {
        freeaddrinfo(result);
        return std::unexpected(std::format(
            "DNS resolution for '{}' returned null ai_addr", host));
    }
    auto* addr = reinterpret_cast<struct sockaddr_in*>(result->ai_addr);
    ip = ntohl(addr->sin_addr.s_addr);
    freeaddrinfo(result);

    SPDLOG_LOGGER_DEBUG(log, "DNS resolved: {} -> {}", host, net::format_ipv4(ip).data());
    return ip;
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal: shared connection setup logic
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

/// Generate a random ephemeral port in [49152, 65535].
/// Returns 0 on CSPRNG failure.
///
/// Uses RAND_bytes so the port is unpredictable, reducing the chance of
/// mid-air collisions when multiple connections are established rapidly.
inline uint16_t random_ephemeral_port() noexcept {
    // kEphemeralPortRange must be a power of 2 so the modulo is unbiased
    // (no off-by-one over-representation of the low values).
    static_assert((kEphemeralPortRange & (kEphemeralPortRange - 1)) == 0,
                  "kEphemeralPortRange must be a power of 2 for unbiased modulo");
    uint16_t rnd;
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&rnd), sizeof(rnd)) != 1)
        return 0;
    return kEphemeralPortMin + (rnd % kEphemeralPortRange);
}

/// Intermediate products from prepare_connection(), consumed by connect()
/// to create the Transport and (optionally) return MAC addresses.
struct ConnectionSetup {
    rte_ether_addr src_mac;
    rte_ether_addr dst_mac;
    std::function<std::expected<std::unique_ptr<TcpSession<>>, std::string>()> tcp_factory;
};

/// Shared logic: validate → get MAC → parse IPs → ARP → ephemeral port
/// → build TcpConfig + factory.  Used by all connect() overloads.
[[nodiscard]] inline std::expected<ConnectionSetup, std::string>
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

    // ARP resolve gateway (skip if pre-resolved MAC provided)
    rte_ether_addr dst_mac{};
    if (opts.gateway_mac) {
        dst_mac = *opts.gateway_mac;
    } else {
        auto arp_result = arp::resolve(
            opts.platform.port_id, opts.rx_queue_id, mempool,
            src_mac, local_ip, gateway_ip, opts.arp_timeout);
        if (!arp_result) {
            return std::unexpected(
                std::format("ARP resolution failed: {}", arp_result.error()));
        }
        dst_mac = *arp_result;
    }

    // Ephemeral source port
    uint16_t src_port = opts.local_port;
    if (src_port == 0) {
        src_port = detail::random_ephemeral_port();
        if (src_port == 0) {
            return std::unexpected("CSPRNG failure: RAND_bytes failed for ephemeral port");
        }
        SPDLOG_LOGGER_DEBUG(connector_logger(), "dpdk::connect: ephemeral port {}", src_port);
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
[[nodiscard]] std::expected<ConnectResult<TransportType>, std::string>
connect(const DpdkEndpoint& ep,
        const TransportConfig& transport_cfg,
        uint32_t server_ip,
        const ConnectorOptions& opts = {}) {

    // Validate all configs before Platform::create() to fail fast on bad config
    // (Platform init is expensive and single-port NICs only allow one)
    if (auto err = ep.validate(); !err.empty()) {
        return std::unexpected(std::format("DpdkEndpoint: {}", err));
    }
    if (auto err = opts.validate(); !err.empty()) {
        return std::unexpected(std::format("ConnectorOptions: {}", err));
    }
    if (server_ip == 0) {
        return std::unexpected("server_ip must be a valid IPv4 address (host byte order)");
    }

    [[maybe_unused]] auto log = detail::connector_logger();
    SPDLOG_LOGGER_DEBUG(log, "dpdk::connect: local={}, gateway={}, server={}",
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

    SPDLOG_LOGGER_DEBUG(log, "dpdk::connect: connection established");

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
[[nodiscard]] std::expected<ConnectResult<TransportType>, std::string>
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
    [[maybe_unused]] auto log = detail::connector_logger();
    SPDLOG_LOGGER_DEBUG(log, "dpdk::connect: kernel DNS failed ({}), trying DPDK DNS",
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
        return std::unexpected(std::format(
            "DNS fallback: Platform creation failed: {}", platform.error()));
    }

    rte_ether_addr src_mac{};
    if (rte_eth_macaddr_get(opts.platform.port_id, &src_mac) != 0) {
        return std::unexpected(std::format(
            "DNS fallback: failed to get MAC for port {}", opts.platform.port_id));
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

    // Reuse the already-created Platform for the full connection setup.
    // Pass the resolved gateway MAC via opts so prepare_connection() skips
    // ARP — we already have the MAC from the DNS-fallback ARP above.
    ConnectorOptions opts_with_mac = opts;
    opts_with_mac.gateway_mac = *gw_mac;

    auto transport_result = connect<TransportType>(
        *platform, ep, transport_cfg, *dpdk_ip, opts_with_mac);
    if (!transport_result) {
        return std::unexpected(transport_result.error());
    }

    SPDLOG_LOGGER_DEBUG(log, "dpdk::connect: connection established (via DPDK DNS)");

    return ConnectResult<TransportType>{
        .platform    = std::move(*platform),
        .transport   = std::move(*transport_result),
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
[[nodiscard]] std::expected<ConnectResult<TransportType>, std::string>
connect(std::string_view host, const DpdkEndpoint& ep,
        const ConnectorOptions& opts = {}) {
    TransportConfig transport_cfg{
        .remote_host = std::string(host),
    };
    return connect<TransportType>(ep, transport_cfg, opts);
}

/// @brief Connect using an existing Platform with a pre-resolved server IP.
///
/// Reuses an already-initialized Platform (port + mempool) instead of creating
/// a new one. Useful for multi-connection setups on the same NIC port.
///
/// @tparam TransportType  Transport alias (default: DpdkTransport)
/// @param platform      Initialized Platform to reuse
/// @param ep            DPDK endpoint (local_ip, gateway_ip)
/// @param transport_cfg Transport configuration (TLS, WS, reconnect settings)
/// @param server_ip     Pre-resolved server IPv4 in host byte order
/// @param opts          Optional connection settings (timeouts, queue IDs)
/// @return Unique pointer to the connected Transport, or error string
template <typename TransportType = DpdkTransport>
[[nodiscard]] std::expected<std::unique_ptr<TransportType>, std::string>
connect(Platform& platform,
        const DpdkEndpoint& ep,
        const TransportConfig& transport_cfg,
        uint32_t server_ip,
        const ConnectorOptions& opts = {}) {

    [[maybe_unused]] auto log = detail::connector_logger();
    SPDLOG_LOGGER_DEBUG(log, "dpdk::connect(platform&): local={}, gateway={}, server={}",
                 ep.local_ip, ep.gateway_ip,
                 net::format_ipv4(server_ip).data());

    auto setup = detail::prepare_connection(
        ep, opts, transport_cfg, server_ip, platform.mempool());
    if (!setup) {
        SPDLOG_LOGGER_DEBUG(log, "dpdk::connect(platform&): prepare_connection failed: {}",
                     setup.error());
        return std::unexpected(setup.error());
    }

    auto transport = TransportType::create(
        std::move(setup->tcp_factory), transport_cfg);
    if (!transport) {
        SPDLOG_LOGGER_DEBUG(log, "dpdk::connect(platform&): Transport creation failed: {}",
                     transport.error().message());
        return std::unexpected(
            std::format("Transport creation failed: {}", transport.error().message()));
    }

    SPDLOG_LOGGER_DEBUG(log, "dpdk::connect(platform&): connection established");
    return std::move(*transport);
}

/// @brief Connect using an existing Platform with DNS resolution (kernel, then DPDK fallback).
///
/// @tparam TransportType  Transport alias (default: DpdkTransport)
/// @param platform      Initialized Platform to reuse
/// @param ep            DPDK endpoint (local_ip, gateway_ip)
/// @param transport_cfg Transport configuration (remote_host must be set)
/// @param opts          Optional connection settings
/// @return Unique pointer to the connected Transport, or error string
template <typename TransportType = DpdkTransport>
[[nodiscard]] std::expected<std::unique_ptr<TransportType>, std::string>
connect(Platform& platform,
        const DpdkEndpoint& ep,
        const TransportConfig& transport_cfg,
        const ConnectorOptions& opts = {}) {
    [[maybe_unused]] auto log = detail::connector_logger();
    if (transport_cfg.remote_host.empty()) {
        SPDLOG_LOGGER_DEBUG(log, "dpdk::connect(platform&): remote_host is empty");
        return std::unexpected(
            "TransportConfig: remote_host is required for hostname-based connect");
    }

    // Try kernel DNS first
    auto ip = resolve_hostname(transport_cfg.remote_host);
    if (ip) {
        return connect<TransportType>(platform, ep, transport_cfg, *ip, opts);
    }

    // Fall back to DPDK DNS
    SPDLOG_LOGGER_DEBUG(log, "dpdk::connect(platform&): kernel DNS failed ({}), trying DPDK DNS",
                 ip.error());

    uint32_t local_ip   = net::parse_ipv4(ep.local_ip.c_str());
    uint32_t gateway_ip = net::parse_ipv4(ep.gateway_ip.c_str());
    if (local_ip == 0) {
        SPDLOG_LOGGER_DEBUG(log, "dpdk::connect(platform&): local_ip parse failed: '{}'",
                     ep.local_ip);
        return std::unexpected(
            std::format("Invalid local_ip: '{}' failed to parse", ep.local_ip));
    }
    if (gateway_ip == 0) {
        SPDLOG_LOGGER_DEBUG(log, "dpdk::connect(platform&): gateway_ip parse failed: '{}'",
                     ep.gateway_ip);
        return std::unexpected(
            std::format("Invalid gateway_ip: '{}' failed to parse", ep.gateway_ip));
    }

    rte_ether_addr src_mac{};
    if (rte_eth_macaddr_get(opts.platform.port_id, &src_mac) != 0) {
        SPDLOG_LOGGER_DEBUG(log, "dpdk::connect(platform&): MAC get failed for port {}",
                     opts.platform.port_id);
        return std::unexpected(std::format(
            "Failed to get MAC for DPDK DNS fallback (port={})",
            opts.platform.port_id));
    }

    auto gw_mac = arp::resolve(
        opts.platform.port_id, opts.rx_queue_id, platform.mempool(),
        src_mac, local_ip, gateway_ip, opts.arp_timeout);
    if (!gw_mac) {
        SPDLOG_LOGGER_DEBUG(log, "dpdk::connect(platform&): ARP for gateway {} failed: {}",
                     ep.gateway_ip, gw_mac.error());
        return std::unexpected(std::format(
            "DNS fallback: ARP for gateway failed: {}", gw_mac.error()));
    }

    auto dpdk_ip = dns::resolve(
        opts.platform.port_id, opts.rx_queue_id, platform.mempool(),
        src_mac, *gw_mac, local_ip,
        transport_cfg.remote_host, opts.dns);
    if (!dpdk_ip) {
        SPDLOG_LOGGER_DEBUG(log, "dpdk::connect(platform&): DPDK DNS failed: {}", dpdk_ip.error());
        return std::unexpected(std::format(
            "DNS failed (kernel: {}, DPDK: {})", ip.error(), dpdk_ip.error()));
    }

    return connect<TransportType>(platform, ep, transport_cfg, *dpdk_ip, opts);
}

/// @brief Simplest connect with an existing Platform: hostname + endpoint.
///
/// Builds a default TransportConfig (port 443, TLS on) from the hostname
/// and delegates to the DNS-resolving overload.
///
/// @tparam TransportType  Transport alias (default: DpdkTransport)
/// @param platform  Initialized Platform to reuse
/// @param host      Hostname or dotted-decimal IPv4 to connect to
/// @param ep        DPDK endpoint (local_ip, gateway_ip)
/// @param opts      Optional connection settings
/// @return Unique pointer to the connected Transport, or error string
template <typename TransportType = DpdkTransport>
[[nodiscard]] std::expected<std::unique_ptr<TransportType>, std::string>
connect(Platform& platform, std::string_view host, const DpdkEndpoint& ep,
        const ConnectorOptions& opts = {}) {
    TransportConfig transport_cfg{
        .remote_host = std::string(host),
    };
    return connect<TransportType>(platform, ep, transport_cfg, opts);
}

} // namespace eph::dpdk

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter specializations
// ─────────────────────────────────────────────────────────────────────────────

template <>
struct std::formatter<eph::dpdk::DpdkEndpoint> : std::formatter<std::string> {
    auto format(const eph::dpdk::DpdkEndpoint& ep, auto& ctx) const {
        return std::formatter<std::string>::format(ep.dump(), ctx);
    }
};

template <>
struct std::formatter<eph::dpdk::ConnectorOptions> : std::formatter<std::string> {
    auto format(const eph::dpdk::ConnectorOptions& opts, auto& ctx) const {
        return std::formatter<std::string>::format(opts.dump(), ctx);
    }
};
