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

#include "core/config.hpp"

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

/// Read DPDK bootstrap keys from the pre-section (global) part of
/// bench.conf and construct a fully-initialized `DpdkBenchEnv`.
///
/// Required global keys (lowercase checked first, uppercase fallback):
///   - mock_ip      / SERVER_IP     — destination (kernel mock on NIC_A)
///   - client_ip    / LOCAL_IP      — DPDK client source IP on NIC_B
///   - gateway_ip   / GATEWAY_IP    — default GW for ARP resolve
///   - dpdk_pci     / NIC_B_PCI     — NIC_B PCI BDF (e.g. "0000:28:00.0")
/// Optional global key (default "0,1" with WARN):
///   - eal_cores    / EAL_CORES     — comma-separated lcore list
///
/// `mock_ip`/`client_ip`/`gateway_ip` are REQUIRED (no default). Unlike
/// the kernel bench path which falls back to 127.0.0.1 for loopback smoke
/// tests, DPDK over loopback is meaningless per
/// memory/feedback_bench_no_loopback.md, so missing these keys is a hard
/// error.
///
/// Failure modes (all surface as `std::unexpected(std::string)`):
///   - "load_dpdk_env: missing global key '<k>'"
///   - "load_dpdk_env: create_full: <reason>"
[[nodiscard]] inline std::expected<eph::dpdk::test::DpdkBenchEnv, std::string>
load_dpdk_env(const bench::ScenarioConfig& globals,
              uint16_t dpdk_port_id = 0) noexcept try {
    // `ScenarioConfig::load_globals` deliberately preserves trailing inline
    // comments verbatim (see the comment at load_globals' implementation) —
    // the legacy uppercase keys in bench.conf all have `# DPDK EAL lcores`-
    // style trailers that would otherwise land inside `value`. Strip the
    // first unquoted `#` and any surrounding whitespace here, which is the
    // behaviour the legacy `BenchConfig` parser used for the same keys.
    auto strip_inline_comment = [](std::string s) -> std::string {
        if (auto hash = s.find('#'); hash != std::string::npos) {
            s.erase(hash);
        }
        // rtrim + ltrim (whitespace-only span)
        auto ws = [](unsigned char c) { return std::isspace(c) != 0; };
        while (!s.empty() && ws(static_cast<unsigned char>(s.back()))) s.pop_back();
        std::size_t first = 0;
        while (first < s.size() && ws(static_cast<unsigned char>(s[first]))) ++first;
        if (first > 0) s.erase(0, first);
        return s;
    };

    // ── Required networking keys ─────────────────────────────────────────
    auto read_required = [&](std::string_view lower, std::string_view upper)
        -> std::expected<std::string, std::string> {
        std::string v = strip_inline_comment(std::string{globals.get_string(lower, "")});
        if (v.empty()) v = strip_inline_comment(std::string{globals.get_string(upper, "")});
        if (v.empty()) {
            return std::unexpected(
                "load_dpdk_env: missing global key '" + std::string{lower} +
                "' (also checked uppercase '" + std::string{upper} + "')");
        }
        return v;
    };

    auto mock_ip_r = read_required("mock_ip", "SERVER_IP");
    if (!mock_ip_r) return std::unexpected(mock_ip_r.error());
    auto client_ip_r = read_required("client_ip", "LOCAL_IP");
    if (!client_ip_r) return std::unexpected(client_ip_r.error());
    auto gateway_ip_r = read_required("gateway_ip", "GATEWAY_IP");
    if (!gateway_ip_r) return std::unexpected(gateway_ip_r.error());
    auto dpdk_pci_r = read_required("dpdk_pci", "NIC_B_PCI");
    if (!dpdk_pci_r) return std::unexpected(dpdk_pci_r.error());

    // ── Optional cores with warn-on-default ──────────────────────────────
    std::string eal_cores =
        strip_inline_comment(std::string{globals.get_string("eal_cores", "")});
    if (eal_cores.empty()) {
        eal_cores = strip_inline_comment(
            std::string{globals.get_string("EAL_CORES", "")});
    }
    if (eal_cores.empty()) {
        spdlog::warn("load_dpdk_env: eal_cores not set; defaulting to \"0,1\"");
        eal_cores = "0,1";
    }

    // ── Synthesize EAL argv and keep backing storage alive ───────────────
    auto argv_storage = synthesize_eal_argv(eal_cores, *dpdk_pci_r);
    std::vector<char*> argv;
    argv.reserve(argv_storage.size());
    for (auto& s : argv_storage) {
        argv.push_back(s.data());
    }
    const int argc = static_cast<int>(argv.size());

    spdlog::info(
        "load_dpdk_env: EAL argv synthesized: {} cores={} pci={}",
        argc, eal_cores, *dpdk_pci_r);

    // ── Optional RSS / multi-queue from globals ──────────────────────────
    // bench.conf knobs:
    //   NB_RX_QUEUES=4
    //   ENABLE_RSS=true
    // Defaults (1 / false) keep every existing scenario behaviourally
    // identical to the pre-stage-5 path.
    eph::dpdk::PlatformConfig pcfg{};
    pcfg.port_id = dpdk_port_id;
    if (auto v = globals.get_u32("nb_rx_queues", 1); v) {
        pcfg.nb_rx_queues = static_cast<uint16_t>(*v);
        pcfg.nb_tx_queues = std::max<uint16_t>(pcfg.nb_rx_queues, 1);
    }
    {
        std::string rss_str = strip_inline_comment(
            std::string{globals.get_string("enable_rss", "false")});
        for (auto& c : rss_str)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        pcfg.enable_rss = (rss_str == "true" || rss_str == "1" ||
                           rss_str == "yes" || rss_str == "on");
    }
    if (pcfg.enable_rss || pcfg.nb_rx_queues > 1) {
        spdlog::info(
            "load_dpdk_env: RSS configured nb_rx_queues={} enable_rss={}",
            pcfg.nb_rx_queues, pcfg.enable_rss ? "true" : "false");
    }

    // ── Delegate to the shared EAL+Platform+ARP bring-up ─────────────────
    auto env_r = eph::dpdk::test::DpdkBenchEnv::create_full(
        argc, argv.data(),
        *mock_ip_r,     // server_ip
        *client_ip_r,   // local_ip
        *gateway_ip_r,  // gateway_ip
        pcfg);
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
