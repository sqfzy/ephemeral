/// @file core/dpdk_env.hpp
/// Bridge from bench.conf + ScenarioConfig globals to a
/// fully-initialized `eph::dpdk::test::DpdkBenchEnv` (EAL init + Platform +
/// ARP resolve) so the six `lat_*_dpdk` scenario binaries can share one
/// bring-up sequence.
///
/// Per .artifacts/plan-phase-11-dpdk-measurement-20260411-082123.md D-1/D-2:
/// we deliberately keep `DpdkBenchEnv` in `eph::dpdk::test::` (not in
/// `eph::net::dpdk::`) because test fixtures and benchmarks share the same
/// bring-up; and EAL parameters are synthesized from bench.conf so the user
/// CLI stays `sudo lat tcp --dpdk` without passing `-l 0,1 -a 0000:28:00.0`.
///
/// This header compiles to nothing when `EPH_USE_DPDK` is not defined so
/// kernel-only bench builds keep working.

#pragma once

#ifdef EPH_USE_DPDK

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "core/bench_conf.hpp"

#include "eph/dpdk/test/dpdk_env.hpp"

namespace bench {

/// Synthesize the EAL argv that `DpdkBenchEnv::create_full` expects.
///
/// Given `cores_csv="0,1"` and `pci_bdf="0000:28:00.0"` the result is:
///
///   ["lat_bench",
///    "-l", "0,1",
///    "-a", "0000:28:00.0",
///    "--proc-type=auto",
///    "--log-level=lib.eal:warning",
///    "--"]
///
/// If `pci_bdf` is empty the `-a <pci>` pair is omitted — the caller is
/// responsible for deciding whether that is a hard error (typical bench
/// run) or a soft-fallback (single-NIC smoke test).
///
/// Pure function: no I/O, no EAL calls, no env lookups. Exists as a
/// standalone helper so `tests/unit/bench/test_dpdk_env_argv.cpp` can
/// unit-test the argv shape without booting DPDK.
[[nodiscard]] inline std::vector<std::string>
synthesize_eal_argv(std::string_view cores_csv,
                    std::string_view pci_bdf) {
    std::vector<std::string> argv;
    argv.reserve(10);
    // argv[0] must be a program name string — EAL logs it on init but
    // otherwise ignores its value. Use a stable literal so log scrapes
    // don't depend on the scenario binary name.
    argv.emplace_back("lat_bench");
    argv.emplace_back("-l");
    argv.emplace_back(std::string{cores_csv});
    if (!pci_bdf.empty()) {
        argv.emplace_back("-a");
        argv.emplace_back(std::string{pci_bdf});
    }
    // --proc-type=auto lets EAL detect primary/secondary; for bench this
    // is always primary. Hardcoded per plan §Encoding-规范 (not exposed
    // to bench.conf to avoid config sprawl).
    argv.emplace_back("--proc-type=auto");
    // Silence non-warning EAL chatter so the bench report stays readable.
    argv.emplace_back("--log-level=lib.eal:warning");
    // The `--` separator is required by `DpdkBenchEnv::create_full` to
    // split EAL args from (empty) scenario args.
    argv.emplace_back("--");
    return argv;
}

/// Pick a random ephemeral source port in the [49152, 65535] range.
///
/// Rationale (plan D-3): the DPDK `TcpSession` 4-tuple bypasses the kernel
/// TIME_WAIT table, but the kernel mock (on the other side of the wire)
/// keeps TIME_WAIT for ~60 s. Reusing a fixed src_port between back-to-back
/// bench runs trips the mock's accept path. Fresh port per `main()` is the
/// zero-cost mitigation.
///
/// Uses `std::random_device` for seeding — this is not a hot path and the
/// single call happens once per scenario binary startup.
[[nodiscard]] inline uint16_t random_src_port() noexcept {
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<uint16_t> dist{49152, 65535};
    return dist(rng);
}

/// Read DPDK bootstrap fields from config.toml-backed `BenchConfig` and
/// construct a fully-initialized `DpdkBenchEnv`.
///
/// Required fields (validated by `load_bench_conf()`):
///   - networking.server_ip   — destination (kernel mock on NIC_A)
///   - networking.client_ip   — DPDK client source IP on NIC_B
///   - networking.gateway_ip  — default GW for ARP resolve
///   - networking.nic_b_pci   — NIC_B PCI BDF (required for DPDK bring-up)
/// Optional:
///   - cpu.eal_cores          — comma-separated lcore list (default "0,1")
///   - dpdk.rss.nb_rx_queues  — default 1
///   - dpdk.rss.enable_rss    — default false
///
/// Failure modes (all surface as `std::unexpected(std::string)`):
///   - "load_dpdk_env: networking.nic_b_pci is required ..."
///   - "load_dpdk_env: create_full: <reason>"
[[nodiscard]] inline std::expected<eph::dpdk::test::DpdkBenchEnv, std::string>
load_dpdk_env(const BenchConfig& cfg,
              uint16_t dpdk_port_id = 0) noexcept try {
    const std::string& mock_ip    = cfg.networking.server_ip;
    const std::string& client_ip  = cfg.networking.client_ip;
    const std::string& gateway_ip = cfg.networking.gateway_ip;
    const std::string& dpdk_pci   = cfg.networking.nic_b_pci;

    if (dpdk_pci.empty()) {
        return std::unexpected(
            "load_dpdk_env: networking.nic_b_pci is required in config.toml "
            "for DPDK bring-up");
    }

    std::string eal_cores = cfg.cpu.eal_cores;
    if (eal_cores.empty()) {
        spdlog::warn("load_dpdk_env: cpu.eal_cores not set; defaulting to \"0,1\"");
        eal_cores = "0,1";
    }

    auto argv_storage = synthesize_eal_argv(eal_cores, dpdk_pci);
    std::vector<char*> argv;
    argv.reserve(argv_storage.size());
    for (auto& s : argv_storage) argv.push_back(s.data());
    const int argc = static_cast<int>(argv.size());

    spdlog::info(
        "load_dpdk_env: EAL argv synthesized: {} cores={} pci={}",
        argc, eal_cores, dpdk_pci);

    eph::dpdk::PlatformConfig pcfg{};
    pcfg.port_id      = dpdk_port_id;
    pcfg.nb_rx_queues = cfg.dpdk.nb_rx_queues;
    pcfg.nb_tx_queues = std::max<uint16_t>(pcfg.nb_rx_queues, 1);
    pcfg.enable_rss   = cfg.dpdk.enable_rss;

    if (pcfg.enable_rss || pcfg.nb_rx_queues > 1) {
        spdlog::info(
            "load_dpdk_env: RSS configured nb_rx_queues={} enable_rss={}",
            pcfg.nb_rx_queues, pcfg.enable_rss ? "true" : "false");
    }

    auto env_r = eph::dpdk::test::DpdkBenchEnv::create_full(
        argc, argv.data(), mock_ip, client_ip, gateway_ip, pcfg);
    if (!env_r) {
        return std::unexpected("load_dpdk_env: create_full: " + env_r.error());
    }
    return std::move(*env_r);
} catch (const std::exception& e) {
    return std::unexpected(std::string{"load_dpdk_env: exception: "} + e.what());
} catch (...) {
    return std::unexpected("load_dpdk_env: unknown exception");
}

/// Print a one-line DPDK configuration echo so the bench report is
/// self-describing (port_id, MAC addresses, resolved IPs in host byte
/// order). Invoked once per scenario, right before the measurement loop.
inline void print_dpdk_config_echo(const eph::dpdk::test::DpdkBenchEnv& env) noexcept {
    std::printf(
        "dpdk_config: port_id=%u "
        "src_mac=%02x:%02x:%02x:%02x:%02x:%02x "
        "gw_mac=%02x:%02x:%02x:%02x:%02x:%02x "
        "src_ip=0x%08x dst_ip=0x%08x gw_ip=0x%08x\n",
        static_cast<unsigned>(env.port_id),
        env.src_mac.addr_bytes[0], env.src_mac.addr_bytes[1],
        env.src_mac.addr_bytes[2], env.src_mac.addr_bytes[3],
        env.src_mac.addr_bytes[4], env.src_mac.addr_bytes[5],
        env.gw_mac.addr_bytes[0],  env.gw_mac.addr_bytes[1],
        env.gw_mac.addr_bytes[2],  env.gw_mac.addr_bytes[3],
        env.gw_mac.addr_bytes[4],  env.gw_mac.addr_bytes[5],
        env.src_ip, env.dst_ip, env.gw_ip);
    std::fflush(stdout);
}

} // namespace bench

#endif // EPH_USE_DPDK
