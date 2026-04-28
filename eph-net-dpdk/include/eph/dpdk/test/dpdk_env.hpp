#pragma once

/// @file test/dpdk_env.hpp
/// Shared DPDK bootstrap helper for tests and benchmarks.
///
/// `DpdkBenchEnv::create_full(argc, argv, ips..., port_id)` performs:
///   1. Split argv at "--" (EAL args before, scenario args after)
///   2. EalGuard::init()
///   3. Platform::create() (port, mempool, queues)
///   4. Parse local/server/gateway IPs to host byte order
///   5. Get local MAC via rte_eth_macaddr_get()
///   6. ARP-resolve gateway MAC
///
/// The result is a move-only struct containing all the resources needed
/// to create UdpSender, TcpSession, or WS transports over DPDK.
///
/// Originally lived under benchmarks/latency/core/dpdk_env.hpp; promoted
/// to eph-dpdk's test namespace so any test fixture or benchmark can
/// reuse this bring-up sequence without reverse-including the bench
/// tree.  Type name retained as `DpdkBenchEnv` for source compatibility.

#ifdef EPH_USE_DPDK

#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <rte_ethdev.h>

#include <spdlog/spdlog.h>

#include "eph/dpdk/arp.hpp"
#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/net_header.hpp"
#include "eph/dpdk/platform.hpp"
#include "eph/dpdk/tcp.hpp"
#include "eph/dpdk/udp.hpp"

namespace eph::dpdk::test {

/// Move-only bundle of all DPDK resources needed to drive a real-NIC
/// scenario: EAL guard, Platform (port + mempool + queues), resolved
/// src/dst/gw IPs (host byte order), local MAC, gateway MAC
/// (ARP-resolved at startup), and the port/pool handles used by
/// sender factory methods.
///
/// Construct via `create_full()`.  The resulting object owns its EAL
/// and must outlive any UdpSender / TcpSession built from it.
struct DpdkBenchEnv {
    eph::dpdk::EalGuard  eal;
    eph::dpdk::Platform  platform;
    uint32_t             src_ip{};      ///< local IP (host byte order)
    uint32_t             dst_ip{};      ///< server IP (host byte order)
    uint32_t             gw_ip{};       ///< gateway IP (host byte order)
    rte_ether_addr       src_mac{};
    rte_ether_addr       gw_mac{};
    uint16_t             port_id{};
    rte_mempool*         pool{nullptr};

    DpdkBenchEnv(eph::dpdk::EalGuard e, eph::dpdk::Platform p,
                 uint32_t si, uint32_t di, uint32_t gi,
                 rte_ether_addr sm, rte_ether_addr gm,
                 uint16_t pid, rte_mempool* pl)
        : eal(std::move(e)), platform(std::move(p)),
          src_ip(si), dst_ip(di), gw_ip(gi),
          src_mac(sm), gw_mac(gm), port_id(pid), pool(pl) {}

    DpdkBenchEnv(DpdkBenchEnv&&) = default;
    DpdkBenchEnv& operator=(DpdkBenchEnv&&) = default;

    /// Full factory with explicit PlatformConfig — the override path that
    /// lets the bench helpers turn on RSS / multi-queue (`nb_rx_queues`,
    /// `enable_rss`) without touching the legacy 6-arg call sites.
    /// `pcfg.port_id` is honoured; all other fields land verbatim on the
    /// constructed Platform.
    [[nodiscard]] static std::expected<DpdkBenchEnv, std::string>
    create_full(int argc, char** argv,
                const std::string& server_ip_str,
                const std::string& local_ip_str,
                const std::string& gateway_ip_str,
                const eph::dpdk::PlatformConfig& pcfg_template) {
        // ── 1. Split argv at "--" ──────────────────────────────────
        int eal_argc = argc;
        char** eal_argv = argv;
        for (int i = 0; i < argc; ++i) {
            if (std::string_view{argv[i]} == "--") {
                eal_argc = i;
                break;
            }
        }

        // ── 2. EAL init ───────────────────────────────────────────
        auto eal = eph::dpdk::EalGuard::init(eal_argc, eal_argv);
        if (!eal) return std::unexpected("EAL init: " + eal.error());

        // ── 3. Platform (caller-supplied config) ──────────────────
        auto plat = eph::dpdk::Platform::create(pcfg_template);
        if (!plat) return std::unexpected("Platform: " + plat.error());
        const uint16_t dpdk_port_id = pcfg_template.port_id;

        uint16_t port_id = plat->port_id();
        rte_mempool* pool = plat->mempool();

        // ── 4. Parse IPs (host byte order) ────────────────────────
        auto parse_ip = [](const std::string& s) -> std::expected<uint32_t, std::string> {
            in_addr addr{};
            if (inet_pton(AF_INET, s.c_str(), &addr) != 1) {
                return std::unexpected("invalid IP: " + s);
            }
            return ntohl(addr.s_addr);
        };
        auto src_ip = parse_ip(local_ip_str);
        if (!src_ip) return std::unexpected(src_ip.error());
        auto dst_ip = parse_ip(server_ip_str);
        if (!dst_ip) return std::unexpected(dst_ip.error());
        auto gw_ip = parse_ip(gateway_ip_str);
        if (!gw_ip) return std::unexpected(gw_ip.error());

        // ── 5. Local MAC ──────────────────────────────────────────
        rte_ether_addr src_mac{};
        if (int rc = rte_eth_macaddr_get(port_id, &src_mac); rc != 0) {
            return std::unexpected("rte_eth_macaddr_get failed: " +
                                   std::to_string(rc));
        }
        spdlog::info("dpdk_env: local MAC {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
                     src_mac.addr_bytes[0], src_mac.addr_bytes[1],
                     src_mac.addr_bytes[2], src_mac.addr_bytes[3],
                     src_mac.addr_bytes[4], src_mac.addr_bytes[5]);

        // ── 6. ARP resolve gateway MAC ────────────────────────────
        auto gw_mac_result = eph::dpdk::arp::resolve(
            port_id, pool,
            src_mac, *src_ip, *gw_ip,
            std::chrono::seconds{3});
        if (!gw_mac_result) {
            return std::unexpected("ARP resolve gateway: " + gw_mac_result.error());
        }
        spdlog::info("dpdk_env: gateway MAC {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
                     gw_mac_result->addr_bytes[0], gw_mac_result->addr_bytes[1],
                     gw_mac_result->addr_bytes[2], gw_mac_result->addr_bytes[3],
                     gw_mac_result->addr_bytes[4], gw_mac_result->addr_bytes[5]);

        return DpdkBenchEnv{
            std::move(*eal), std::move(*plat),
            *src_ip, *dst_ip, *gw_ip,
            src_mac, *gw_mac_result,
            port_id, pool
        };
    }

    /// Backwards-compatible overload — defaults to single-queue / RSS-off.
    /// New code that wants RSS should use the PlatformConfig overload.
    [[nodiscard]] static std::expected<DpdkBenchEnv, std::string>
    create_full(int argc, char** argv,
                const std::string& server_ip_str,
                const std::string& local_ip_str,
                const std::string& gateway_ip_str,
                uint16_t dpdk_port_id) {
        eph::dpdk::PlatformConfig pcfg{};
        pcfg.port_id = dpdk_port_id;
        return create_full(argc, argv, server_ip_str, local_ip_str,
                           gateway_ip_str, pcfg);
    }

    /// Create a UdpSender configured for this environment.
    [[nodiscard]] std::expected<eph::dpdk::UdpSender, std::string>
    make_udp_sender(uint16_t local_port, uint16_t remote_port) const {
        eph::dpdk::UdpConfig ucfg{};
        ucfg.src_ip = src_ip;
        ucfg.dst_ip = dst_ip;
        ucfg.src_port = local_port;
        ucfg.dst_port = remote_port;
        ucfg.src_mac = src_mac;
        ucfg.dst_mac = gw_mac;
        ucfg.port_id = port_id;
        ucfg.tx_queue_id = 0;
        ucfg.pool = pool;
        return eph::dpdk::UdpSender::create(ucfg);
    }

    /// Create a TcpConfig for TcpSession.
    [[nodiscard]] eph::dpdk::TcpConfig
    make_tcp_config(uint16_t local_port, uint16_t remote_port) const {
        eph::dpdk::TcpConfig tcfg{};
        tcfg.tuple = {src_ip, dst_ip, local_port, remote_port};
        tcfg.src_mac = src_mac;
        tcfg.dst_mac = gw_mac;
        tcfg.port_id = port_id;
        tcfg.tx_queue_id = 0;
        tcfg.rx_queue_id = 0;
        return tcfg;
    }
};

} // namespace eph::dpdk::test

#endif // EPH_USE_DPDK
