/// @file dpdk_rss_demo.cpp
///
/// Single-process DPDK example: one `Platform` with `enable_rss=true` and
/// `nb_rx_queues > 1`, multiple `DpdkUdpSocket`s spawned via the turnkey
/// `create_and_attach` factory and spread across the NIC's RX queues. The
/// point is the **RSS data plane in single-process mode** — the missing
/// piece between `simple_hft.cpp` (single-queue HFT client) and
/// `dpdk_mp_demo.cpp` (multi-process, where RSS is a side effect of
/// queue partitioning).
///
/// What's demonstrated:
///
///   * `Platform::create_primary` with `enable_rss=true` + `nb_rx_queues=N`,
///     hard-failing on RSS bring-up (no silent collapse to queue 0 — see
///     `eph-net-dpdk/CHANGELOG.md`).
///   * The three diagnostic getters that tell you what RSS path resolved:
///         `dispatch_mode()` /
///         `rss_using_probed_key()` / `effective_rx_queue_range()`.
///     The probed-key path lights up on ENA where the PMD rejects
///     `rte_eth_dev_rss_hash_update`; we read back the NIC's own key via
///     `rte_eth_dev_rss_hash_conf_get` and use that for `predict_rss_queue`.
///   * **One `DpdkPoller` per RX queue, polled by a dedicated EAL lcore**
///     (`rte_eal_remote_launch` for workers; lcore 0 / main thread polls
///     queue 0). Each worker lcore was pinned to its cpu inside
///     `rte_eal_init` via `EalGuard::init_with_pins(pins)` — the typed
///     entry already routes through `pin_lcore` and the registry.
///     This is the multi-lcore production pattern; the previous version
///     of this example used main-thread round-robin and is now replaced.
///   * `DpdkUdpSocket::create_and_attach` with `pin_to_queue = q` for
///     each connection — the helper reverse-picks an ephemeral
///     `src_port` so the inbound 5-tuple Toeplitz-hashes back to `q`.
///     The library logs the picked port at INFO; that's what makes the
///     RSS pinning visible.
///   * Smoke-boot tolerance: `create_and_attach` may legitimately fail
///     (RSS port-search exhausted, ARP not resolved, MACs unset) — we
///     warn and continue so the cleanup branches still get exercised.
///
/// What's deliberately out of scope:
///
///   * No actual traffic. UDP has no handshake, so once the sockets are
///     created and the pollers attached, the demo just drives `poll()`
///     for `--seconds` and exits. Swap in a real `OnDatagram` callback +
///     send loop to drive production traffic.
///   * No ARP/DNS resolution. `dst_mac` is left zero — bytes won't egress
///     a real router, but that's not what this example exists to show.
///     For a real-server probe with full control plane, see
///     `examples/binance_latency.cpp`.
///   * Multi-process mode. See `examples/dpdk_mp_demo.cpp` and
///     `eph-net-dpdk/docs/dpdk-multiprocess.md` for that surface.
///   * `RxDispatchMode::FlowDirector`. The same `create_and_attach` API
///     handles FD via rte_flow rule install when the NIC supports it;
///     selected automatically by `Platform::detect_rx_dispatch_mode()`.
///     This example forces `enable_rss=true` to make the RssPartitioned
///     path the focus.
///
/// Required environment: NIC bound to vfio-pci, ≥ 256 hugepages free.
/// See `eph-net-dpdk/scripts/dpdk-setup.sh`.
///
/// **Required**: the number of `--pin` lcores must equal `--nb-queues`
/// (one lcore per RSS RX queue). The mapping is positional:
/// the i-th `--pin` becomes lcore i, polling queue i. Mismatch is rejected
/// at startup. `--lcores '<raw EAL spec>'` (escape hatch) is only valid
/// when its lcore-count happens to match `--nb-queues`; if you use the raw
/// path you're on the hook to make the layout match.
///
/// Usage:
///
///   sudo ./dpdk_rss_demo --
///        --pci 0000:28:00.0
///        --pin 0=4 --pin 1=5 --pin 2=6 --pin 3=7
///        --nb-queues 4 --connections 4
///        --src-ip 0x0A000010 --dst-ip 0x0A000020 --dst-port 30000
///
/// `--src-ip` / `--dst-ip` take a **packed-uint32 hex literal** (host
/// byte order), not a dotted quad — they are passed through `strtoul(...,
/// 16)`. `0x0A000010` corresponds to `10.0.0.16` (`0x0A.0x00.0x00.0x10`).
/// A dotted quad would silently truncate at the first dot and resolve to
/// the wrong /32 — see the AppArgs default values below for the
/// canonical encoding.
///
/// Additional flags (all optional; full list at parse_args, line 173):
///   --lcores '<spec>'     raw EAL escape hatch, mutually exclusive
///                         with --pin (e.g. `(0-3)@(4-7)`).
///   --port-id <id>        DPDK port enumeration index (default 0;
///                         only relevant when more than one --pci
///                         is bound).
///   --seconds <n>         demo run duration (default 5).
///   --per-lcore-pools <k> NUMA-aware mempool hint. `0` (default)
///                         keeps the single-pool shape; `k > 0`
///                         creates k pools and asks `create_and_attach`
///                         for `pool_lcore_hint = (i % k)` per
///                         connection so each socket draws from a
///                         pool local to its lcore (saves 50-100 ns
///                         per mbuf alloc on multi-socket hosts).
///
/// Recommended log level for this demo is `info`: the library's
/// "create_and_attach: RSS pin → src_port=… hashes to queue=…" log line
/// is the only place the helper's reverse-pick is surfaced.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_lcore.h>

#include <spdlog/spdlog.h>

#include "eph/codec/raw_datagram_codec.hpp"
#include "eph/dpdk/cli.hpp"
#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/lcore_pin.hpp"   // LcorePin / EalGuard::init_with_pins
#include "eph/dpdk/platform.hpp"
#include "eph/net/dpdk/flow_steering.hpp"  // RxDispatchMode / rx_dispatch_mode_name
#include "eph/net/dpdk/poller.hpp"
#include "eph/net/dpdk/udp_socket.hpp"
#include "eph/utils/cpu.hpp"        // CpuPinPolicy

namespace edpdk = eph::net::dpdk;
namespace ed    = eph::dpdk;
namespace ec    = eph::codec;
using namespace std::chrono_literals;

static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running.store(false, std::memory_order_release); }

namespace {

struct AppArgs {
    /// EAL flags — --pci / --pin / --lcores / --port-id (or --dpdk-port).
    ed::cli::EalCliConfig        eal{};
    uint16_t                nb_rx_queues = 4;
    uint16_t                connections  = 4;    // # of UDP sockets to spawn
    uint32_t                src_ip       = 0x0A000010;  // 10.0.0.16
    uint32_t                dst_ip       = 0x0A000020;  // 10.0.0.32
    uint16_t                dst_port     = 30000;
    std::chrono::seconds    run_seconds  = 5s;
    /// @brief Per-lcore / NUMA-aware mempools (T2.9). When > 0, Platform
    /// creates that many additional pools (one per lcore id, allocated on
    /// the lcore's local NUMA socket). `create_and_attach` then honours
    /// `UdpConfig::pool_lcore_hint = (i % per_lcore_pools)` per
    /// connection so each socket draws from a pool local to its lcore —
    /// the +50–100 ns cross-NUMA mbuf alloc penalty disappears on multi-
    /// socket hosts. `0` (default) preserves the byte-for-byte single-pool
    /// shape this example uses unless opted in.
    uint16_t                per_lcore_pools = 0;
};

int split_app_args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--") return i;
    }
    return argc;
}

AppArgs parse_args(int argc, char** argv) {
    AppArgs out;
    const int sep = split_app_args(argc, argv);
    for (int i = sep + 1; i < argc; ++i) {
        std::string_view a = argv[i];
        char const* next = (i + 1 < argc) ? argv[i + 1] : nullptr;

        // EAL flags first.
        auto consumed = ed::cli::consume_one(out.eal, a, next);
        if (!consumed) {
            spdlog::error("dpdk_rss_demo: {}", consumed.error());
            std::exit(1);
        }
        if (*consumed > 0) { i += *consumed - 1; continue; }

        if      (a == "--nb-queues"       && next) { out.nb_rx_queues = static_cast<uint16_t>(std::atoi(next)); ++i; }
        else if (a == "--connections"     && next) { out.connections  = static_cast<uint16_t>(std::atoi(next)); ++i; }
        else if (a == "--src-ip"          && next) { out.src_ip       = static_cast<uint32_t>(std::strtoul(next, nullptr, 16)); ++i; }
        else if (a == "--dst-ip"          && next) { out.dst_ip       = static_cast<uint32_t>(std::strtoul(next, nullptr, 16)); ++i; }
        else if (a == "--dst-port"        && next) { out.dst_port     = static_cast<uint16_t>(std::atoi(next)); ++i; }
        else if (a == "--seconds"         && next) { out.run_seconds  = std::chrono::seconds(std::atoi(next));  ++i; }
        else if (a == "--per-lcore-pools" && next) { out.per_lcore_pools = static_cast<uint16_t>(std::atoi(next)); ++i; }
    }
    return out;
}

/// Per-worker context handed to `rte_eal_remote_launch`. Shared across
/// all workers (no per-worker mutable state besides the ctor-time bound
/// `poller`). Lifetime: outlives every worker — both `running` and
/// `pollers[]` storage are owned by `main`'s scope, and the workers are
/// joined via `rte_eal_wait_lcore` before main returns.
struct WorkerCtx {
    edpdk::DpdkPoller<>*       poller;        ///< this worker's queue Poller
    uint16_t                   queue_id;      ///< for log lines only
    std::atomic<bool>*         running;       ///< shared stop flag
};

/// Entry point for every worker lcore (`rte_eal_remote_launch` requires
/// `int (*)(void*)`). Tight burst-poll loop until `*ctx->running` clears
/// — no allocations, no syscalls (other than what the PMD does inside
/// `rte_eth_rx_burst`). Returns 0 unconditionally; the lcore goes back
/// to `eal_thread_loop()` to wait for the next launch (or for cleanup).
int worker_main(void* arg) noexcept {
    auto* ctx = static_cast<WorkerCtx*>(arg);
    const unsigned lcore = rte_lcore_id();
    const unsigned cpu   = rte_lcore_to_cpu_id(static_cast<int>(lcore));
    spdlog::info("dpdk_rss_demo: worker lcore={} cpu={} queue={} entering poll loop",
                 lcore, cpu, ctx->queue_id);
    while (ctx->running->load(std::memory_order_acquire)) {
        (void)ctx->poller->poll();
    }
    spdlog::info("dpdk_rss_demo: worker lcore={} queue={} exited",
                 lcore, ctx->queue_id);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    spdlog::set_level(spdlog::level::info);

    const AppArgs args = parse_args(argc, argv);
    if (args.nb_rx_queues < 2) {
        spdlog::error("dpdk_rss_demo: --nb-queues must be >= 2 "
                      "(RSS demo needs multi-queue dispatch)");
        return 1;
    }
    if (args.connections == 0) {
        spdlog::error("dpdk_rss_demo: --connections must be >= 1");
        return 1;
    }

    // ── Validate --pin / --lcores exclusivity (require at least one) ─────
    if (auto v = ed::cli::validate(args.eal); !v) {
        spdlog::error("dpdk_rss_demo: {}", v.error());
        return 1;
    }
    if (args.eal.pins.empty() && args.eal.lcores_raw.empty()) {
        spdlog::error("dpdk_rss_demo: provide either --pin lcore=cpu[:role] "
                      "(typed) or --lcores '<raw EAL spec>' (escape hatch)");
        return 1;
    }
    // Multi-lcore launch is positional: --pin[i] runs queue[i]. Mismatch
    // between #pins and #queues would either leave queues unpolled or
    // leave lcores idle in eal_thread_loop. Reject early. Raw --lcores
    // bypasses this check (we have no way to count its lcores without
    // re-parsing DPDK's set-of-sets syntax).
    if (!args.eal.pins.empty() && args.eal.pins.size() != args.nb_rx_queues) {
        spdlog::error("dpdk_rss_demo: --pin count ({}) must equal "
                      "--nb-queues ({}) — one lcore per RSS queue",
                      args.eal.pins.size(), args.nb_rx_queues);
        return 1;
    }

    // ── 1) EAL + Platform via the unified launch factory ────────
    // Single binary, single peer ⇒ single-process bring-up. Platform
    // owns the EAL session and runs eal_cleanup atomically on
    // destruction; no separate EalGuard needed.
    ed::PlatformConfig pcfg{};
    pcfg.port_id          = args.eal.port_id;
    pcfg.nb_rx_queues     = args.nb_rx_queues;
    pcfg.nb_tx_queues     = args.nb_rx_queues;
    pcfg.per_lcore_pools  = args.per_lcore_pools;
    // max_procs default 1 = single-process RSS multi-queue.

    if (!args.eal.pins.empty()) {
        spdlog::info("dpdk_rss_demo: bring-up via launch "
                     "(typed pins, {} pin(s))", args.eal.pins.size());
    } else {
        spdlog::info("dpdk_rss_demo: bring-up via launch "
                     "(raw lcores='{}')", args.eal.lcores_raw);
    }

    auto plat_r = ed::Platform::launch(
        std::move(pcfg),
        ed::cli::to_eal_config(args.eal, "dpdk_rss_demo"),
        std::span<ed::LcorePin const>{args.eal.pins},
        eph::utils::CpuPinPolicy{});
    if (!plat_r) {
        spdlog::error("dpdk_rss_demo: Platform::launch failed: {}",
                      plat_r.error());
        return 2;
    }
    auto platform = std::move(*plat_r);

    // ── 3) RSS diagnostics — what path actually resolved? ────────────────
    // `rss_using_probed_key()` lights up when `configure_rss` is rejected
    // by the PMD (notably ENA on AWS) and we fell back to reading the
    // NIC's own hash key for `predict_rss_queue`.
    const auto qr = platform.effective_rx_queue_range();
    spdlog::info(
        "dpdk_rss_demo: Platform up — port={}, nb_rx_queues={}, "
        "dispatch_mode={}, is_rss_active={}, rss_using_probed_key={}, "
        "rx_queue_range=[{},{})",
        platform.port_id(), platform.nb_rx_queues(),
        edpdk::rx_dispatch_mode_name(platform.dispatch_mode()),
        platform.dispatch_mode() == edpdk::RxDispatchMode::RssPartitioned,
        platform.rss_using_probed_key(),
        qr.first, qr.second);

    if (platform.dispatch_mode() != edpdk::RxDispatchMode::RssPartitioned) {
        spdlog::warn("dpdk_rss_demo: NIC did not resolve to "
                     "RssPartitioned mode — RSS-pinned src_port path will "
                     "not engage. The example continues so cleanup branches "
                     "are exercised, but this is not what the demo is for.");
    }

    // ── 4) One Poller per owned RX queue, registered with Platform ───────
    // Each Poller is constructed and registered from the main thread; the
    // `poll()` calls themselves are dispatched per-lcore in step 6 via
    // `rte_eal_remote_launch`. Pollers are NOT thread-local — they're
    // owned here in `pollers[]`, and each worker just receives a raw
    // pointer through its WorkerCtx. The PMD requires one polling thread
    // per RX queue, which the queue-id ↔ lcore-id mapping enforces.
    std::vector<std::unique_ptr<edpdk::DpdkPoller<>>> pollers;
    pollers.reserve(qr.second - qr.first);
    for (uint16_t q = qr.first; q < qr.second; ++q) {
        edpdk::PollerConfig poller_cfg{};
        poller_cfg.port_id     = platform.port_id();
        poller_cfg.rx_queue_id = q;
        auto pr = edpdk::DpdkPoller<>::create(poller_cfg);
        if (!pr) {
            spdlog::error("dpdk_rss_demo: DpdkPoller::create(queue={}) "
                          "failed: {}", q, pr.error().detail);
            return 4;
        }
        if (auto r = platform.register_poller(q, pr->get()); !r) {
            spdlog::error("dpdk_rss_demo: register_poller(queue={}) "
                          "failed: {}", q, r.error().detail);
            return 5;
        }
        pollers.emplace_back(std::move(*pr));
        spdlog::info("dpdk_rss_demo: poller registered for queue={}", q);
    }

    // ── 5) Spawn UDP sockets, each pinned to a distinct RX queue ─────────
    // For each connection i: ask `create_and_attach` to land the inbound
    // 5-tuple on `queue = qr.first + (i % nb_owned_queues)`. The helper
    // walks `[32768, 60999]` looking for an ephemeral src_port whose
    // Toeplitz hash matches that queue under the NIC's RSS key, rebinds
    // `cfg.dpdk.wire.src_port` to the picked value, and
    // constructs the sender. The library logs the picked port at INFO —
    // watch for "RSS pin → src_port=… hashes to queue=…".
    using UdpSock = edpdk::DpdkUdpSocket<ec::RawDatagramCodec>;
    std::vector<std::unique_ptr<UdpSock>> sockets;
    sockets.reserve(args.connections);

    const uint16_t nb_owned = static_cast<uint16_t>(qr.second - qr.first);
    for (uint16_t i = 0; i < args.connections; ++i) {
        const uint16_t want_q = qr.first + (i % nb_owned);

        edpdk::UdpConfig ucfg{};
        ucfg.dpdk.wire.src_ip   = args.src_ip;
        ucfg.dpdk.wire.dst_ip   = args.dst_ip;
        // src_port populated as a placeholder: validate() requires non-zero.
        // The RSS-pin path overwrites it with the reverse-picked ephemeral.
        ucfg.dpdk.wire.src_port = static_cast<uint16_t>(32768 + i);
        ucfg.dpdk.wire.dst_port = args.dst_port;
        ucfg.dpdk.wire.port_id  = platform.port_id();
        ucfg.dpdk.wire.pool     = platform.mempool();
        // dst_mac left zero — would need ARP resolution to drive real
        // traffic. See eph::dpdk::arp::resolve / examples/binance_latency.cpp.

        ucfg.dpdk.pin_to_queue = want_q;

        // Per-lcore mempool hint (T2.9, NUMA-aware alloc). When
        // `per_lcore_pools > 0`, each socket gets a pool local to its
        // own lcore — Platform clamps `hint` against the pool count, so
        // `i % per_lcore_pools` is safe.  When `per_lcore_pools == 0`,
        // leave the hint at -1 so the helper uses `wire.pool`
        // as set (the single shared pool, byte-for-byte the pre-T2.9 path).
        if (args.per_lcore_pools > 0) {
            ucfg.dpdk.pool_lcore_hint =
                static_cast<int>(i % args.per_lcore_pools);
        }

        auto sr = UdpSock::create_and_attach(std::move(ucfg), platform);
        if (!sr) {
            spdlog::warn(
                "dpdk_rss_demo: create_and_attach[{}] (pin queue={}) "
                "failed: {} -- continuing so RAII cleanup is exercised",
                i, want_q, sr.error().detail);
            continue;
        }
        spdlog::info("dpdk_rss_demo: socket[{}] attached, pinned to queue={}",
                     i, want_q);
        sockets.emplace_back(std::move(*sr));
    }
    spdlog::info("dpdk_rss_demo: {}/{} sockets attached",
                 sockets.size(), args.connections);

    // ── 6) Multi-lcore launch — one EAL lcore per RX queue ───────────────
    // Mapping: queue[i] is polled by lcore[i]. Lcore 0 (the main lcore =
    // *this* thread) polls queue 0; lcores 1..N-1 are workers launched
    // via `rte_eal_remote_launch(lcore_id, worker_main, &ctxs[i])`.
    //
    // EAL has already pinned every worker thread to its declared cpu
    // inside `rte_eal_init` (driven by `--pin` → `init_with_pins` →
    // `--lcores=...` argv). That setaffinity is invisible from this
    // call site; what we control here is which work each lcore runs.
    //
    // `worker_main` returns when the shared `g_running` flag clears.
    // `rte_eal_wait_lcore` joins each worker before main returns so the
    // RAII teardown of `pollers[]` doesn't race with a still-polling
    // worker (use-after-free hazard).
    const uint16_t nb_pollers = static_cast<uint16_t>(pollers.size());
    if (nb_pollers == 0) {
        spdlog::error("dpdk_rss_demo: no pollers registered — bailing");
        return 6;
    }
    std::vector<WorkerCtx> ctxs;
    ctxs.reserve(nb_pollers);
    for (uint16_t i = 0; i < nb_pollers; ++i) {
        ctxs.push_back(WorkerCtx{
            .poller   = pollers[i].get(),
            .queue_id = static_cast<uint16_t>(qr.first + i),
            .running  = &g_running,
        });
    }

    // Spawn workers for lcore 1..N-1. RTE_LCORE_FOREACH_WORKER walks
    // every EAL lcore that is NOT the main lcore. The `i` index into
    // pollers[] starts at 1 (lcore 0 keeps queue 0 for itself).
    uint16_t worker_idx = 1;
    unsigned lcore_id;
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (worker_idx >= nb_pollers) {
            spdlog::error(
                "dpdk_rss_demo: more EAL worker lcores than queues "
                "({} > {}); pin/queue layout must be 1:1",
                static_cast<unsigned>(worker_idx) + 1, nb_pollers);
            g_running.store(false, std::memory_order_release);
            break;
        }
        // rte_eal_remote_launch returns 0 on success, -EBUSY if the
        // target lcore is already running a function (shouldn't happen
        // — this is a fresh init), or -EINVAL on a bad lcore id.
        if (int rc = rte_eal_remote_launch(worker_main,
                                           &ctxs[worker_idx],
                                           lcore_id);
            rc != 0) {
            spdlog::error("dpdk_rss_demo: rte_eal_remote_launch(lcore={}, "
                          "queue={}) failed: rc={}",
                          lcore_id, ctxs[worker_idx].queue_id, rc);
            g_running.store(false, std::memory_order_release);
            break;
        }
        spdlog::info("dpdk_rss_demo: launched worker on lcore={} "
                     "for queue={}", lcore_id, ctxs[worker_idx].queue_id);
        ++worker_idx;
    }

    // ── 7) Main lcore polls queue 0 until SIGINT or deadline ─────────────
    spdlog::info("dpdk_rss_demo: main lcore={} cpu={} polling queue={} "
                 "for {}s (workers polling queues 1..{})",
                 rte_lcore_id(),
                 rte_lcore_to_cpu_id(static_cast<int>(rte_lcore_id())),
                 ctxs[0].queue_id, args.run_seconds.count(),
                 nb_pollers - 1);
    const auto deadline = std::chrono::steady_clock::now() + args.run_seconds;
    while (g_running.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        (void)ctxs[0].poller->poll();
    }

    // ── 8) Stop workers and join ─────────────────────────────────────────
    // Once g_running is false, every worker_main loop will exit on its
    // next iteration. rte_eal_wait_lcore blocks until the worker returns
    // and then re-arms the lcore for a future launch (we don't reuse it).
    // This MUST run before pollers / sockets / platform are destroyed —
    // otherwise a worker still inside poll() would crash on freed memory.
    spdlog::info("dpdk_rss_demo: signalling workers to stop");
    g_running.store(false, std::memory_order_release);
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        const int rc = rte_eal_wait_lcore(lcore_id);
        spdlog::info("dpdk_rss_demo: lcore={} joined (rc={})",
                     lcore_id, rc);
    }

    spdlog::info("dpdk_rss_demo: shutting down — RAII teardown");
    // RAII: ~UdpSock (each socket unregisters from its Poller) →
    //       ~DpdkPoller (each poller unregisters from Platform) →
    //       ~Platform (dev_stop / dev_close / mempool_free / pin guards
    //                  released / eal_cleanup all in one atomic chain).
    return 0;
}
