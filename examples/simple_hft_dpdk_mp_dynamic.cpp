/// @file simple_hft_dpdk_mp_dynamic.cpp
///
/// DPDK single-NIC multi-process via the **autojoin** factory
/// (`Platform::join_dynamic`) — the recommended path for the
/// "two unrelated processes share one NIC" case. Run the SAME
/// binary twice in two terminals; whichever calls `eal_init`
/// first becomes primary, the other auto-attaches as secondary
/// and CAS-claims the lowest free process slot.
///
/// Compared to `simple_hft_dpdk_mp.cpp` (the declarative path
/// using `Platform::create_primary` / `create_secondary` +
/// `MpTopology`), this binary requires **zero coordination**
/// between peers:
///   - no `--role primary|secondary` flag
///   - no shared `--file-prefix` string (auto-derived from the
///     PCI BDF as `"eph_" + sanitize(pci)`)
///   - no `--self-index` argument
///   - launch order is the only "agreement"
///
/// What this binary does:
///   1. Parse `--pci` / `--nb-queues` / `--lcores` from argv.
///   2. Call `Platform::join_dynamic` — which assembles the EAL
///      argv with `--proc-type=auto`, calls `eal_init`,
///      detects the resolved role via `rte_eal_process_type()`,
///      and either creates the primary registry (slot 0) or
///      CAS-claims the lowest free secondary slot.
///   3. Print the resolved role + claimed queue/port range.
///   4. Hold for a few seconds (so a second invocation of the
///      same binary can attach as secondary while we're up).
///   5. Tear down cleanly — `~Platform` frees per-process state,
///      `eal_cleanup` releases EAL.
///
/// For the lower-level declarative path (precise self_index
/// control, asymmetric per-peer specs via `MpTopology::custom`),
/// see `examples/simple_hft_dpdk_mp.cpp`. For the full
/// real-server end-to-end demo (TLS + WS + reconnect), see
/// `examples/binance_latency.cpp`.
///
/// Required environment: NIC bound to vfio-pci, ≥ 256 hugepages
/// free. See `eph-net-dpdk/scripts/dpdk-setup.sh`.
///
/// Usage (two terminals, same host, same NIC, no shared state):
///
///   # Terminal A — first to start ⇒ primary
///   sudo ./simple_hft_dpdk_mp_dynamic --pci 0000:28:00.0 \
///                                     --nb-queues 4       \
///                                     --lcores '0,1'
///
///   # Terminal B — second to start ⇒ secondary, claims slot 1
///   sudo ./simple_hft_dpdk_mp_dynamic --pci 0000:28:00.0 \
///                                     --nb-queues 4       \
///                                     --lcores '2,3'

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/join_dynamic.hpp"
#include "eph/dpdk/platform.hpp"

namespace {

struct Args {
    std::string pci;
    uint16_t    nb_queues   = 4;
    std::string lcores      = "0,1";
    int         hold_seconds = 8;
};

void usage(const char* prog) {
    std::cerr <<
        "Usage: " << prog << " --pci <BDF> [--nb-queues N] "
                            "[--lcores 'L1,L2,...'] [--hold-seconds N]\n"
        "\n"
        "Required:\n"
        "  --pci BDF           PCI bus address of the NIC, e.g.\n"
        "                      0000:28:00.0\n"
        "Optional:\n"
        "  --nb-queues N       total RX rings (default 4); same for\n"
        "                      every peer\n"
        "  --lcores 'L1,L2'    EAL lcore set; pick disjoint sets per\n"
        "                      peer (default '0,1')\n"
        "  --hold-seconds N    keep the Platform alive for N seconds\n"
        "                      so a sibling peer can attach (default 8)\n";
}

[[nodiscard]] std::expected<Args, std::string>
parse_args(int argc, char** argv) {
    Args a{};
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        const auto need_next = [&](const char* opt) -> std::expected<std::string, std::string> {
            if (i + 1 >= argc)
                return std::unexpected(std::string{opt} + " requires a value");
            return std::string{argv[++i]};
        };
        if (arg == "--pci") {
            auto v = need_next("--pci");
            if (!v) return std::unexpected(v.error());
            a.pci = std::move(*v);
        } else if (arg == "--nb-queues") {
            auto v = need_next("--nb-queues");
            if (!v) return std::unexpected(v.error());
            a.nb_queues = static_cast<uint16_t>(std::stoul(*v));
        } else if (arg == "--lcores") {
            auto v = need_next("--lcores");
            if (!v) return std::unexpected(v.error());
            a.lcores = std::move(*v);
        } else if (arg == "--hold-seconds") {
            auto v = need_next("--hold-seconds");
            if (!v) return std::unexpected(v.error());
            a.hold_seconds = std::stoi(*v);
        } else if (arg == "--help" || arg == "-h") {
            return std::unexpected("help");
        } else {
            return std::unexpected("unknown arg: " + arg);
        }
    }
    if (a.pci.empty())
        return std::unexpected("--pci is required");
    return a;
}

} // namespace

int main(int argc, char** argv) {
    auto args_r = parse_args(argc, argv);
    if (!args_r) {
        if (args_r.error() != "help") std::cerr << args_r.error() << "\n";
        usage(argv[0]);
        return args_r.error() == "help" ? 0 : 2;
    }
    const auto& args = *args_r;

    // ── Bring up via autojoin ──────────────────────────────────────────
    eph::dpdk::JoinDynamicConfig cfg{};
    cfg.pci          = args.pci;
    cfg.nb_rx_queues = args.nb_queues;
    cfg.lcores       = {args.lcores};

    SPDLOG_INFO("calling Platform::join_dynamic (pci={}, nb_queues={}, "
                "lcores='{}')",
                args.pci, args.nb_queues, args.lcores);

    auto plat_r = eph::dpdk::Platform::join_dynamic(std::move(cfg));
    if (!plat_r) {
        SPDLOG_ERROR("join_dynamic failed: {}", plat_r.error());
        return 1;
    }
    auto platform = std::move(*plat_r);

    // ── Report what role we resolved to + what slot we got ─────────────
    const char* role = platform.is_secondary() ? "secondary" : "primary";
    const auto qr = platform.effective_rx_queue_range();
    auto pr = platform.self_port_range();

    SPDLOG_INFO("join_dynamic resolved this process to role='{}'", role);
    SPDLOG_INFO("  queue range = [{}, {})", qr.first, qr.second);
    if (pr.has_value()) {
        SPDLOG_INFO("  port  range = [{}, {})", pr->first, pr->second);
    }

    SPDLOG_INFO("holding Platform for {}s — start a second instance of "
                "this binary now to see it attach as the next role",
                args.hold_seconds);
    std::this_thread::sleep_for(std::chrono::seconds(args.hold_seconds));

    SPDLOG_INFO("tearing down");
    // ~Platform must fire BEFORE eal_cleanup (Impl::cleanup calls
    // rte_eth_dev_stop / mempool_free; both illegal post-EAL teardown).
    {
        auto _drop = std::move(platform);
    }
    (void)eph::dpdk::eal_cleanup();
    return 0;
}
