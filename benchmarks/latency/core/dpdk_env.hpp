/// @file core/dpdk_env.hpp
/// Bridge from bench.conf + ScenarioConfig globals to a
/// fully-initialized `eph::dpdk::test::DpdkBenchEnv` (Platform secondary
/// attach + ARP resolve) so the six `lat_*_dpdk` scenario binaries can
/// share one bring-up sequence.
///
/// Per .artifacts/plan-phase-11-dpdk-measurement-20260411-082123.md D-1/D-2:
/// we deliberately keep `DpdkBenchEnv` in `eph::dpdk::test::` (not in
/// `eph::net::dpdk::`) because test fixtures and benchmarks share the same
/// bring-up; and EAL parameters are synthesized from bench.conf so the user
/// CLI stays `sudo lat tcp --dpdk` without passing `-l 0,1 -a 0000:28:00.0`.
///
/// Daemon-reshape (post-b4fc8969): `DpdkBenchEnv::create` now goes through
/// `Platform::create(PlatformConfig)` (secondary attach to a daemon-managed
/// NIC). An `eph-nicd@<pci>` daemon must already be running for
/// `cfg.networking.nic_b_pci` — the `lat` wrapper script handles the
/// vfio-pci binding (via `eph-net-dpdk/tools/dpdk-setup.sh`) but does
/// NOT spawn the daemon; the operator is expected to start it (e.g.
/// `sudo systemctl start eph-nicd@<bdf>` or, in dev settings,
/// `eph::dpdk::dev::ensure_local_daemon` from `eph/dpdk/dev_helpers.hpp`).
/// Without a running daemon, `Platform::create` fails with a typed error
/// pointing at the systemctl command. The previous `Platform::launch` /
/// `create_via_autojoin` paths (and the `EPH_LAT_AUTOJOIN_*` envvar
/// diversion) are gone — multi-process bench runs simply launch N
/// scenario binaries, each one a secondary; the daemon arbitrates queue
/// allocation.
///
/// JSON output naming under MP: now that the legacy
/// `EPH_LAT_AUTOJOIN_MAX_PROCS` gate is no longer set by the
/// orchestrator, `bench::mp_output_suffix` (in `core/bench_ctx.hpp`)
/// instead defaults to "always emit `_pid<N>`" under `EPH_USE_DPDK`
/// builds — so multiple secondaries flushing concurrently no longer
/// silently overwrite each other's results. Tests / debug runs may
/// override via `EPH_LAT_FORCE_PID=0`.
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
    // --proc-type=auto lets EAL detect primary/secondary at runtime;
    // for the daemon-led bench shape that resolves to secondary
    // (eph-nicd is primary). Hardcoded per plan §Encoding-规范 (not
    // exposed to bench.conf to avoid config sprawl). Argv is only
    // consumed by the unit test today; the live bring-up path uses
    // build_eal_argv inside DpdkBenchEnv::create.
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
/// Bench bring-up under the daemon-led model:
///   1. Operator (or `lat` wrapper) starts `eph-nicd@<pci>.service`
///      against `cfg.networking.nic_b_pci`. The daemon owns NIC physical
///      state — descriptor depth, RSS key, mempool size live in
///      `NicServiceConfig` on the daemon side, NOT in this fixture.
///   2. `DpdkBenchEnv::create` calls `Platform::create(PlatformConfig)`
///      with the bench's per-process EAL knobs (typed pins, allowed_devs)
///      and claims `cfg.dpdk.nb_rx_queues` queue pairs from the daemon's
///      pool.
///   3. ARP-resolves the gateway and packages everything into a move-only
///      struct.
///
/// Required fields (validated by `load_bench_conf()`):
///   - networking.server_ip   — destination (kernel mock on NIC_A)
///   - networking.client_ip   — DPDK client source IP on NIC_B
///   - networking.gateway_ip  — default GW for ARP resolve
///   - networking.nic_b_pci   — NIC_B PCI BDF (required for DPDK bring-up)
/// Optional:
///   - cpu.eal_cores          — comma-separated cpu list (default "0,1")
///   - dpdk.rss.nb_rx_queues  — default 1; > 1 asks daemon for a multi-queue
///                              claim (the daemon must be configured with
///                              total_queues >= the requested count)
///
/// Failure modes (all surface as `std::unexpected(std::string)`):
///   - "load_dpdk_env: networking.nic_b_pci is required ..."
///   - "load_dpdk_env: parse_eal_cores_csv: <reason>"
///   - "load_dpdk_env: DpdkBenchEnv::create: <reason>" (often "is
///      eph-nicd@<pci>.service running?")
[[nodiscard]] inline std::expected<eph::dpdk::test::DpdkBenchEnv, std::string>
load_dpdk_env(const BenchConfig& cfg,
              uint16_t dpdk_port_id = 0) noexcept try {
    (void)dpdk_port_id;  // port_id is no longer settable per-process — the
                         // daemon owns NIC bring-up. Kept in the signature
                         // for ABI compat with kernel-only callers.

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

    SPDLOG_INFO(
        "load_dpdk_env: Platform::create via DpdkBenchEnv::create: "
        "pins={} cpus_csv={} pci={} queues={}",
        pins.size(), eal_cores, dpdk_pci, cfg.dpdk.nb_rx_queues);

    // ── Build the new lean PlatformConfig ─────────────────────────
    // Under the daemon-led model the per-process config carries only:
    //   - pci          — identifies which daemon to attach to
    //   - queues       — bidirectional queue pairs to claim from the
    //                    pool (nb_rx_queues from bench.conf maps here)
    //   - pins/lcores  — per-process EAL lcore layout
    //   - pin_policy   — how strict the typed-pin validator should be
    //   - extra_eal_args — log-level / proc-type passthrough
    //   - program_name — argv[0] for EAL log identification
    //
    // NIC physical state (rss key, descriptors, mempool size,
    // promiscuous, …) lives on the daemon's NicServiceConfig and is
    // not tunable from the bench client.
    eph::dpdk::PlatformConfig pcfg{};
    pcfg.pci          = dpdk_pci;
    pcfg.queues       = std::max<uint16_t>(cfg.dpdk.nb_rx_queues, 1);
    pcfg.pins         = std::span<eph::dpdk::LcorePin const>{pins};
    // Relaxed CpuPinPolicy: bench runs on shared dev hosts where
    // isolcpus / NUMA / SMT-sibling enforcement would gate the run
    // on cluster topology rather than what we care about (every
    // measurement thread on a fixed cpu).
    pcfg.pin_policy   = eph::utils::CpuPinPolicy{
        .require_isolcpus            = false,
        .require_no_sibling_conflict = false,
        .require_same_numa           = false,
        .warn_irq_overlap            = false,
    };
    // Bench keeps its EAL log channel quiet — the proc_type, file_prefix,
    // and -a allowlist are all derived inside Platform::create from
    // pcfg.pci, so we only have to forward log-level here.
    pcfg.extra_eal_args = {std::string{"--log-level=lib.eal:warning"}};
    pcfg.program_name   = "lat_bench";

    if (pcfg.queues > 1) {
        SPDLOG_INFO(
            "load_dpdk_env: claiming queues={} (RSS multi-queue requires "
            "the daemon to be configured with total_queues >= {})",
            pcfg.queues, pcfg.queues);
    }

    // Multi-process bench mode (formerly gated on
    // EPH_LAT_AUTOJOIN_MAX_PROCS) is now the default — every
    // lat_*_dpdk binary attaches as a secondary, the daemon arbitrates.
    // The orchestrator just spawns N binaries instead of setting the
    // envvar.
    //
    // FUTURE: S5's QueueAllocator + sub-range partitioning has landed
    // (commit 7984ad28, 2026-04-30) but the daemon does NOT yet expose
    // a "peek-free-count" probe API (it would have to be a separate
    // `eph_queue_peek` IPC since QueueAllocator state lives in the
    // hugepage memzone behind a pthread_mutex). Without that, this
    // fixture cannot preflight-validate `pcfg.queues` against the
    // daemon's unclaimed pool — the only signal is the typed
    // `Error::QueuePoolExhausted` returned by `Platform::create`
    // itself, which is propagated as a clean error string below. Add
    // the preflight only if/when a probe API materialises; until then
    // the ergonomics of "try and fail with a clear error" are fine
    // for the bench fixture.

    auto env_r = eph::dpdk::test::DpdkBenchEnv::create(
        std::move(pcfg),
        mock_ip, client_ip, gateway_ip);
    if (!env_r) {
        return std::unexpected(
            "load_dpdk_env: DpdkBenchEnv::create: " + env_r.error()
            + " (is `eph-nicd@" + dpdk_pci + ".service` running?)");
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

    /// Build a wire::UdpConfig. Caller fills in `src_port` / `dst_port`
    /// and any pool override. Mirrors the population in
    /// `DpdkBenchEnv::make_udp_sender`.
    [[nodiscard]] eph::dpdk::wire::UdpConfig
    make_udp_config(uint16_t local_port, uint16_t remote_port) const {
        eph::dpdk::wire::UdpConfig ucfg{};
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
