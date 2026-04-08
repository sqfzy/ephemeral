/// @file framework/dpdk_setup.hpp
/// One-stop DPDK initialization for bench scenarios.
///
/// Replaces the ~50 lines of EAL split + Platform create + ARP resolve +
/// MAC parse boilerplate that was previously duplicated in every
/// `*_dpdk` scenario .cpp.
///
/// Usage:
///   auto env = bench::DpdkBenchEnv::create(argc, argv, cfg);
///   if (!env) { spdlog::error("{}", env.error()); return 1; }
///   // env->app_argc, env->app_argv  -- post-"--" CLI args
///   // env->src_mac, env->gw_mac     -- resolved Ethernet addresses
///   // env->src_ip, env->dst_ip      -- parsed local + server IPs
///   // env->platform, env->mempool() -- ready-to-use DPDK port
///
/// Only included from .cpp files compiled with EPH_USE_DPDK=1.

#pragma once

#if !defined(EPH_USE_DPDK)
#error "framework/dpdk_setup.hpp requires EPH_USE_DPDK=1"
#endif

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>
#include <rte_ethdev.h>

#include "eph/dpdk/arp.hpp"
#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/net_header.hpp"
#include "eph/dpdk/platform.hpp"
#include "eph/dpdk/udp.hpp"

#include "bench_config.hpp"

namespace bench {

/// Holds all DPDK initialization state for one bench process.
/// Move-only (owns EalGuard and Platform).
class DpdkBenchEnv {
public:
    /// Factory: split argv at "--", init EAL, create Platform, resolve gateway MAC.
    /// Returns ready-to-use environment, or an error string.
    [[nodiscard]] static std::expected<DpdkBenchEnv, std::string>
    create(int argc, char** argv, const BenchConfig& cfg) {
        // 1. Split argv at "--" before touching EAL (rte_eal_init reorders argv)
        int app_argc = 0;
        char** app_argv = nullptr;
        for (int i = 0; i < argc; ++i) {
            if (std::string_view(argv[i]) == "--") {
                app_argc = argc - i - 1;
                app_argv = argv + i + 1;
                break;
            }
        }

        // 2. Initialize EAL
        spdlog::info("Initializing DPDK EAL...");
        auto eal = eph::dpdk::EalGuard::init(argc, argv);
        if (!eal) {
            return std::unexpected("EAL init failed: " + eal.error());
        }

        // 3. Create Platform (NIC-B)
        eph::dpdk::PlatformConfig pcfg{.port_id = cfg.dpdk_port_id};
        auto platform = eph::dpdk::Platform::create(pcfg);
        if (!platform) {
            return std::unexpected("Platform create failed: " + platform.error());
        }

        // 4. Parse local + server IPs
        if (cfg.local_ip.empty() || cfg.gateway_ip.empty() || cfg.server_ip.empty()) {
            return std::unexpected(
                "DPDK mode requires --server-ip, --local-ip, --gateway-ip");
        }
        auto src_ip = eph::dpdk::net::parse_ipv4(cfg.local_ip.c_str());
        auto dst_ip = eph::dpdk::net::parse_ipv4(cfg.server_ip.c_str());
        auto gw_ip  = eph::dpdk::net::parse_ipv4(cfg.gateway_ip.c_str());

        // 5. Get local MAC
        rte_ether_addr src_mac{};
        rte_eth_macaddr_get(cfg.dpdk_port_id, &src_mac);

        // 6. Resolve gateway MAC via ARP
        spdlog::info("Resolving gateway MAC via ARP...");
        auto gw_mac = eph::dpdk::arp::resolve(
            cfg.dpdk_port_id, 0, platform->mempool(),
            src_mac, src_ip, gw_ip,
            std::chrono::milliseconds{3000});
        if (!gw_mac) {
            return std::unexpected("ARP resolve failed: " + gw_mac.error());
        }
        spdlog::info("Gateway MAC resolved: {}",
                     eph::dpdk::net::format_mac(*gw_mac).data());

        return DpdkBenchEnv{
            .eal = std::move(*eal),
            .platform = std::move(*platform),
            .src_mac = src_mac,
            .gw_mac = *gw_mac,
            .src_ip = src_ip,
            .dst_ip = dst_ip,
            .port_id = cfg.dpdk_port_id,
            .app_argc = app_argc,
            .app_argv = app_argv,
        };
    }

    /// Convenience: create a UdpSender on this DPDK port.
    /// `src_port` is the local UDP source port; `dst_port` is the remote.
    [[nodiscard]] std::expected<eph::dpdk::UdpSender, std::string>
    make_udp_sender(uint16_t src_port, uint16_t dst_port) {
        return eph::dpdk::UdpSender::create({
            .src_ip = src_ip, .dst_ip = dst_ip,
            .src_port = src_port, .dst_port = dst_port,
            .src_mac = src_mac, .dst_mac = gw_mac,
            .port_id = port_id, .tx_queue_id = 0,
            .pool = platform.mempool(),
        });
    }

    [[nodiscard]] rte_mempool* mempool() noexcept { return platform.mempool(); }

    // Move-only
    DpdkBenchEnv(DpdkBenchEnv&&) = default;
    DpdkBenchEnv& operator=(DpdkBenchEnv&&) = default;
    DpdkBenchEnv(const DpdkBenchEnv&) = delete;
    DpdkBenchEnv& operator=(const DpdkBenchEnv&) = delete;

    // Public fields (POD-style for direct access from scenarios)
    eph::dpdk::EalGuard eal;
    eph::dpdk::Platform platform;
    rte_ether_addr src_mac;
    rte_ether_addr gw_mac;
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t port_id;
    int app_argc;
    char** app_argv;
};

} // namespace bench
