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
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "core/bench_conf.hpp"

#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/lcore_pin.hpp"
#include "eph/dpdk/test/dpdk_env.hpp"
#include "eph/utils/cpu.hpp"

namespace bench {

/// Synthesize the legacy bench EAL argv shape (kept for the
/// `test_dpdk_env_argv.cpp` unit test only). Production bench
/// bring-up now goes through `DpdkBenchEnv::create` (which builds
/// EAL argv internally via `build_eal_argv`); this function is no
/// longer on the live bring-up path.
///
/// Given `cores_csv="0,1"` and `pci_bdf="0000:28:00.0"` the result is:
///
///   ["lat_bench", "-l", "0,1", "-a", "0000:28:00.0",
///    "--proc-type=auto", "--log-level=lib.eal:warning", "--"]
///
/// Pure function: no I/O, no EAL calls, no env lookups.
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
    // The `--` separator is the legacy bench bring-up convention
    // (split EAL args from scenario args). Preserved here so the
    // argv-shape unit test continues to validate the historical
    // contract; the new DpdkBenchEnv::create path does not consume
    // it.
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

/// Parse a comma-separated CPU list (e.g. `"0,1"` or `"0, 4, 5"`) into a
/// vector of `LcorePin` with sequential `lcore_id`s 0..N-1 and role
/// `"bench-eal"`. Whitespace around each token is trimmed; empty tokens
/// (`",,"`) are skipped. Negative values surface as `std::unexpected`.
///
/// This is the typed replacement for the legacy `-l <csv>` argv path:
/// CSV `"0,4"` lowers to `[LcorePin{0,0,"bench-eal"}, LcorePin{1,4,"bench-eal"}]`
/// which builds `--lcores=0@0,1@4` — byte-equivalent to the EAL semantics
/// of `-l 0,4` (lcore 0 on cpu 0, lcore 1 on cpu 4) but participates in
/// the process-wide pin registry.
[[nodiscard]] inline std::expected<std::vector<eph::dpdk::LcorePin>, std::string>
parse_eal_cores_csv(std::string_view csv) {
    std::vector<eph::dpdk::LcorePin> pins;
    std::uint16_t next_lcore = 0;
    std::size_t pos = 0;
    while (pos <= csv.size()) {
        std::size_t comma = csv.find(',', pos);
        std::string_view tok = (comma == std::string_view::npos)
                                   ? csv.substr(pos)
                                   : csv.substr(pos, comma - pos);
        // Strip surrounding whitespace.
        while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t'))
            tok.remove_prefix(1);
        while (!tok.empty() && (tok.back()  == ' ' || tok.back()  == '\t'))
            tok.remove_suffix(1);
        if (!tok.empty()) {
            // strtol over std::atoi: atoi swallows non-digits silently
            // ("abc" → 0), which would have pinned this lcore to cpu 0
            // and stacked it with another lcore — directly violating the
            // "every bench thread on a fixed cpu" project rule. strtol
            // tells us where parsing stopped via `end`, so we can reject
            // any trailing garbage.
            const std::string tok_z{tok};
            char* end = nullptr;
            errno = 0;
            const long parsed = std::strtol(tok_z.c_str(), &end, 10);
            if (end == tok_z.c_str() || end == nullptr || *end != '\0' ||
                errno == ERANGE) {
                return std::unexpected(std::string{
                    "parse_eal_cores_csv: malformed cpu token '"}
                    + std::string{tok} + "' (expected non-negative decimal)");
            }
            if (parsed < 0) {
                return std::unexpected(std::string{
                    "parse_eal_cores_csv: negative cpu in token '"}
                    + std::string{tok} + "'");
            }
            // Cap at INT_MAX defensively. Real cpu ids are 0..NCPU-1
            // (a few hundred at most); anything larger is a typo.
            if (parsed > std::numeric_limits<int>::max()) {
                return std::unexpected(std::string{
                    "parse_eal_cores_csv: cpu out of int range in token '"}
                    + std::string{tok} + "'");
            }
            const int cpu = static_cast<int>(parsed);
            pins.push_back(eph::dpdk::LcorePin{
                next_lcore++, cpu, std::string{"bench-eal"}});
        }
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
    return pins;
}

/// Read DPDK bootstrap fields from config.toml-backed `BenchConfig` and
/// construct a fully-initialized `DpdkBenchEnv`.
///
/// EAL init goes through `EalGuard::init_with_pins`: the CSV
/// `cpu.eal_cores` is parsed into typed `LcorePin` specs, validated against
/// the process-wide pin registry (relaxed policy for shared dev hosts —
/// `cpu_client` collisions surface loudly), then lowered to
/// `--lcores=<lcore>@<cpu>,...` argv. The legacy `synthesize_eal_argv`
/// (`-l <csv>`) helper remains for unit-test coverage but is no longer
/// the bench bring-up path.
///
/// Required fields (validated by `load_bench_conf()`):
///   - networking.server_ip   — destination (kernel mock on NIC_A)
///   - networking.client_ip   — DPDK client source IP on NIC_B
///   - networking.gateway_ip  — default GW for ARP resolve
///   - networking.nic_b_pci   — NIC_B PCI BDF (required for DPDK bring-up)
/// Optional:
///   - cpu.eal_cores          — comma-separated cpu list (default "0,1")
///   - dpdk.rss.nb_rx_queues  — default 1; > 1 auto-engages RSS/FD bring-up
///
/// Failure modes (all surface as `std::unexpected(std::string)`):
///   - "load_dpdk_env: networking.nic_b_pci is required ..."
///   - "load_dpdk_env: parse_eal_cores_csv: <reason>"
///   - "load_dpdk_env: DpdkBenchEnv::create: <reason>"
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
        SPDLOG_WARN("load_dpdk_env: cpu.eal_cores not set; defaulting to \"0,1\"");
        eal_cores = "0,1";
    }

    auto pins_r = parse_eal_cores_csv(eal_cores);
    if (!pins_r) {
        return std::unexpected("load_dpdk_env: " + pins_r.error());
    }
    auto pins = std::move(*pins_r);

    eph::dpdk::EalConfig eal_cfg{};
    eal_cfg.program_name = "lat_bench";
    eal_cfg.allowed_devs = {dpdk_pci};
    // --proc-type=auto and --log-level filter routed through extra_args.
    // create_with_eal (via DpdkBenchEnv::create) appends its own
    // `--lcores=...` token from the typed pins on top of these.
    eal_cfg.extra_args   = {std::string{"--proc-type=auto"},
                            std::string{"--log-level=lib.eal:warning"}};

    SPDLOG_INFO(
        "load_dpdk_env: EAL+Platform via DpdkBenchEnv::create: "
        "pins={} cpus_csv={} pci={} nb_rx_queues={}",
        pins.size(), eal_cores, dpdk_pci, cfg.dpdk.nb_rx_queues);

    eph::dpdk::PlatformConfig pcfg{};
    pcfg.port_id      = dpdk_port_id;
    pcfg.nb_rx_queues = cfg.dpdk.nb_rx_queues;
    pcfg.nb_tx_queues = std::max<uint16_t>(pcfg.nb_rx_queues, 1);

    if (pcfg.nb_rx_queues > 1) {
        SPDLOG_INFO(
            "load_dpdk_env: RSS auto-engaged via nb_rx_queues={}",
            pcfg.nb_rx_queues);
    }

    // Relaxed CpuPinPolicy: bench runs on shared dev hosts where
    // isolcpus / NUMA / SMT-sibling enforcement would gate the run
    // on cluster topology rather than what we care about (every
    // measurement thread on a fixed cpu).
    eph::utils::CpuPinPolicy pin_policy{
        .require_isolcpus            = false,
        .require_no_sibling_conflict = false,
        .require_same_numa           = false,
        .warn_irq_overlap            = false,
    };

    // ── EPH_LAT_AUTOJOIN_* envvar diversion (multi-process bench mode) ──
    // When EPH_LAT_AUTOJOIN_MAX_PROCS is set, use create_via_autojoin so
    // lat_*_dpdk binaries can run as either primary or secondary within one
    // NIC session. Originally diagnostic; now also the production path for
    // 7-process parallel bench acceptance.
    if (const char* max_procs_s = std::getenv("EPH_LAT_AUTOJOIN_MAX_PROCS");
        max_procs_s && *max_procs_s) {
        // Strict parse: reject non-numeric / out-of-range / 0/1 envvars
        // up front instead of silently coercing via atoi() (Vuln A from
        // /pax --review). atoi("bad")=0 and atoi("-1")→cast→4294967295
        // would have surfaced as opaque "no free slot" or topology
        // parse errors with no pointer back at the typo.
        char* end = nullptr;
        const unsigned long mp_raw = std::strtoul(max_procs_s, &end, 10);
        if (end == max_procs_s || *end != '\0' || mp_raw < 2 || mp_raw > 64) {
            return std::unexpected(
                std::string{"load_dpdk_env: EPH_LAT_AUTOJOIN_MAX_PROCS='"}
                + max_procs_s + "' invalid (expect integer in [2, 64])");
        }
        const uint32_t max_procs = static_cast<uint32_t>(mp_raw);
        const char* lcores_s     = std::getenv("EPH_LAT_AUTOJOIN_LCORES");
        const char* gw_mac_file  = std::getenv("EPH_LAT_AUTOJOIN_GW_MAC_FILE");
        // Treat empty env-var values the same as unset — without this, an
        // orchestrator that does `export EPH_LAT_AUTOJOIN_LCORES="$VAR"`
        // with `$VAR` accidentally empty would bypass the bench.conf
        // fallback and surface as a hard error from create_via_autojoin's
        // empty-lcores guard. The hard error is correct downstream
        // (mask=0 silently disables MpRegistry v2), but at the bench
        // boundary an unset/empty env-var should fall back to eal_cores
        // (the bench.conf value) so the user-facing contract matches the
        // doc claim "Optional. Falls back to bench.conf eal_cores".
        std::string lcores_str   =
            (lcores_s && *lcores_s) ? lcores_s : eal_cores;
        std::string gw_mac_path  =
            (gw_mac_file && *gw_mac_file) ? gw_mac_file : "";
        SPDLOG_INFO(
            "load_dpdk_env: EPH_LAT_AUTOJOIN_MAX_PROCS={} lcores={} gw_mac_file={}",
            max_procs, lcores_str, gw_mac_path);
        auto env_r = eph::dpdk::test::DpdkBenchEnv::create_via_autojoin(
            dpdk_pci, max_procs, lcores_str,
            mock_ip, client_ip, gateway_ip, gw_mac_path);
        if (!env_r) {
            return std::unexpected(
                "load_dpdk_env: DpdkBenchEnv::create_via_autojoin: " + env_r.error());
        }
        return std::move(*env_r);
    }

    auto env_r = eph::dpdk::test::DpdkBenchEnv::create(
        std::move(pcfg),
        std::move(eal_cfg),
        std::span<eph::dpdk::LcorePin const>{pins},
        pin_policy,
        mock_ip, client_ip, gateway_ip);
    if (!env_r) {
        return std::unexpected("load_dpdk_env: DpdkBenchEnv::create: " + env_r.error());
    }
    return std::move(*env_r);
} catch (const std::exception& e) {
    return std::unexpected(std::string{"load_dpdk_env: exception: "} + e.what());
} catch (...) {
    return std::unexpected("load_dpdk_env: unknown exception");
}

/// Non-owning view of the DPDK bench environment. Holds a `Platform&`
/// plus the resolved IPs / MACs / port_id / pool from `DpdkBenchEnv`.
/// Mirror of `eph::dpdk::test::DpdkBenchEnv`'s helpers
/// (`make_tcp_config` / `make_udp_config`) so per-scenario
/// `run_lat_<sc>_loop` functions can produce wire-level configs
/// without depending on the owning DpdkBenchEnv type.
struct DpdkBenchView {
    eph::dpdk::Platform&  platform;
    uint32_t              src_ip;
    uint32_t              dst_ip;
    uint32_t              gw_ip;
    rte_ether_addr        src_mac;
    rte_ether_addr        gw_mac;
    uint16_t              port_id;
    rte_mempool*          pool;

    /// Build a TcpConfig with this view's tuple+MAC+port. Mirrors
    /// `DpdkBenchEnv::make_tcp_config`. tx/rx_queue_id are left at 0;
    /// caller (or `Stream::create_and_attach`'s pin_to_queue path)
    /// overrides them to the appropriate queue.
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

    /// Build a UdpConfig (the legacy wire-level fields). Caller fills
    /// in `src_port` / `dst_port` and any pool override. Mirrors the
    /// UdpConfig population in `DpdkBenchEnv::make_udp_sender`.
    [[nodiscard]] eph::dpdk::UdpConfig
    make_udp_config(uint16_t local_port, uint16_t remote_port) const {
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
        return ucfg;
    }
};

/// Construct a `DpdkBenchView` referencing `env`'s Platform and
/// snapshotting the IP / MAC / port / pool fields. Used by single-
/// binary main()s to bridge DpdkBenchEnv → DpdkBenchView → BenchCtx.
[[nodiscard]] inline DpdkBenchView
make_view(eph::dpdk::test::DpdkBenchEnv& env) noexcept {
    return DpdkBenchView{
        env.platform,
        env.src_ip, env.dst_ip, env.gw_ip,
        env.src_mac, env.gw_mac,
        env.port_id, env.pool,
    };
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
