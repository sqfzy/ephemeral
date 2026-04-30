/// @file lat_multi_dpdk.cpp
/// Multi-scenario parallel runner. Brings up a single Platform sharing
/// NIC_B across N concurrent EAL worker lcores, each running one
/// lat scenario from `[parallel].runs[]` of bench config.toml.
///
/// Modeled on `examples/simple_hft_dpdk_rss.cpp` (single-process N
/// queue / N worker lcore / RSS-pinned src_port) but each worker
/// runs a different `run_lat_<sc>_loop<EnableTls>(BenchCtx&)`.
///
/// Usage (drives this from `lat all --dpdk` after NIC → vfio-pci and
/// per-scenario mockex fork):
///
///   sudo ./lat_multi_dpdk --config /path/to/config.toml
///
/// Exit codes:
///   0   all workers exited cleanly
///   1   config parse / validation
///   2   Platform / Poller setup
///   3   Stream::create_and_attach in some worker
///   != 0 any worker's rc

#define EPH_USE_DPDK 1

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_lcore.h>

#include <spdlog/spdlog.h>

#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/lcore_pin.hpp"
#include "eph/dpdk/platform.hpp"
#include "eph/net/dpdk/flow_steering.hpp"
#include "eph/net/dpdk/poller.hpp"
#include "eph/utils/cpu.hpp"

#include "core/bench_conf.hpp"
#include "core/bench_ctx.hpp"
#include "core/dpdk_env.hpp"
#include "core/measurement.hpp"

#include "scenarios/lat_tcp_loop.hpp"
#include "scenarios/lat_udp_loop.hpp"
#include "scenarios/lat_ws_loop.hpp"
#include "scenarios/lat_ex_market_loop.hpp"
#include "scenarios/lat_ex_market_2p_loop.hpp"
#include "scenarios/lat_ex_order_loop.hpp"
#include "scenarios/lat_ex_md_udp_loop.hpp"

namespace ed = eph::net::dpdk;
namespace edpdk = eph::dpdk;
using namespace std::chrono_literals;

namespace {

std::atomic<bool> g_running{true};
void on_signal(int) { g_running.store(false, std::memory_order_release); }

constexpr const char* kDefaultConfigPath = "benchmarks/latency/bench.conf";

const char* parse_config_path(int argc, char** argv) noexcept {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0) return argv[i + 1];
    }
    if (const char* env = std::getenv("BENCH_CONFIG"); env && *env) return env;
    return kDefaultConfigPath;
}

enum class ScenarioId {
    kTcp, kUdp, kWs, kExMarket, kExMarket2P, kExOrder, kExMdUdp, kUnknown
};

ScenarioId scenario_id_from_name(std::string_view s) noexcept {
    if (s == "lat_tcp")          return ScenarioId::kTcp;
    if (s == "lat_udp")          return ScenarioId::kUdp;
    if (s == "lat_ws")           return ScenarioId::kWs;
    if (s == "lat_ex_market")    return ScenarioId::kExMarket;
    if (s == "lat_ex_market_2p") return ScenarioId::kExMarket2P;
    if (s == "lat_ex_order")     return ScenarioId::kExOrder;
    if (s == "lat_ex_md_udp")    return ScenarioId::kExMdUdp;
    return ScenarioId::kUnknown;
}

/// Per-worker context handed to rte_eal_remote_launch.
struct WorkerCtx {
    bench::BenchCtx     bench_ctx;
    ScenarioId          scenario_id;
    bool                use_tls;
    int                 result_rc = 0;
};

/// EAL worker entrypoint (signature `int(*)(void*)`).
int worker_main(void* arg) noexcept {
    auto* ctx = static_cast<WorkerCtx*>(arg);
    const unsigned lcore = rte_lcore_id();
    const unsigned cpu   = rte_lcore_to_cpu_id(static_cast<int>(lcore));
    spdlog::info("lat_multi_dpdk: [slot={}] worker lcore={} cpu={} scenario={}",
                 ctx->bench_ctx.slot_index, lcore, cpu,
                 ctx->bench_ctx.scenario_cfg
                     ? ctx->bench_ctx.scenario_cfg->name()
                     : "?");

    int rc = 0;
    switch (ctx->scenario_id) {
        case ScenarioId::kTcp:
            rc = ctx->use_tls
                    ? bench::scenarios::run_lat_tcp_loop<true>(ctx->bench_ctx)
                    : bench::scenarios::run_lat_tcp_loop<false>(ctx->bench_ctx);
            break;
        case ScenarioId::kUdp:
            rc = bench::scenarios::run_lat_udp_loop(ctx->bench_ctx);
            break;
        case ScenarioId::kWs:
            rc = ctx->use_tls
                    ? bench::scenarios::run_lat_ws_loop<true>(ctx->bench_ctx)
                    : bench::scenarios::run_lat_ws_loop<false>(ctx->bench_ctx);
            break;
        case ScenarioId::kExMarket:
            rc = ctx->use_tls
                    ? bench::scenarios::run_lat_ex_market_loop<true>(ctx->bench_ctx)
                    : bench::scenarios::run_lat_ex_market_loop<false>(ctx->bench_ctx);
            break;
        case ScenarioId::kExMarket2P:
            rc = ctx->use_tls
                    ? bench::scenarios::run_lat_ex_market_2p_loop<true>(ctx->bench_ctx)
                    : bench::scenarios::run_lat_ex_market_2p_loop<false>(ctx->bench_ctx);
            break;
        case ScenarioId::kExOrder:
            rc = ctx->use_tls
                    ? bench::scenarios::run_lat_ex_order_loop<true>(ctx->bench_ctx)
                    : bench::scenarios::run_lat_ex_order_loop<false>(ctx->bench_ctx);
            break;
        case ScenarioId::kExMdUdp:
            rc = bench::scenarios::run_lat_ex_md_udp_loop(ctx->bench_ctx);
            break;
        case ScenarioId::kUnknown:
            spdlog::error("lat_multi_dpdk: [slot={}] unknown scenario",
                          ctx->bench_ctx.slot_index);
            rc = 1;
            break;
    }
    ctx->result_rc = rc;
    spdlog::info("lat_multi_dpdk: [slot={}] worker exited rc={}",
                 ctx->bench_ctx.slot_index, rc);
    return rc;
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    spdlog::set_level(spdlog::level::info);

    const char* conf_path = parse_config_path(argc, argv);
    auto cfg_r = bench::load_bench_conf(conf_path);
    if (!cfg_r) {
        spdlog::error("lat_multi_dpdk: load_bench_conf: {}",
                      bench::format_error(cfg_r.error()));
        return 1;
    }
    const bench::BenchConfig& bench_cfg = *cfg_r;

    if (bench_cfg.parallel.runs.empty()) {
        spdlog::error("lat_multi_dpdk: [parallel].runs is empty (or [parallel] absent)");
        return 1;
    }

    // ── Validate runs[]: scenario known, lcore/cpu/queue uniqueness ──
    uint16_t max_queue = 0;
    {
        std::vector<uint16_t> seen_lcores;
        std::vector<int>      seen_cpus;
        std::vector<uint16_t> seen_queues;
        for (size_t i = 0; i < bench_cfg.parallel.runs.size(); ++i) {
            const auto& r = bench_cfg.parallel.runs[i];
            if (scenario_id_from_name(r.scenario) == ScenarioId::kUnknown) {
                spdlog::error("lat_multi_dpdk: runs[{}].scenario='{}' not in 7 known names",
                              i, r.scenario);
                return 1;
            }
            for (auto v : seen_lcores) if (v == r.lcore) {
                spdlog::error("lat_multi_dpdk: runs[{}].lcore={} duplicated", i, r.lcore);
                return 1;
            }
            for (auto v : seen_cpus) if (v == r.cpu) {
                spdlog::error("lat_multi_dpdk: runs[{}].cpu={} duplicated", i, r.cpu);
                return 1;
            }
            for (auto v : seen_queues) if (v == r.queue) {
                spdlog::error("lat_multi_dpdk: runs[{}].queue={} duplicated", i, r.queue);
                return 1;
            }
            seen_lcores.push_back(r.lcore);
            seen_cpus.push_back(r.cpu);
            seen_queues.push_back(r.queue);
            if (r.queue > max_queue) max_queue = r.queue;
        }
    }
    const uint16_t nb_queues = max_queue + 1;
    spdlog::info("lat_multi_dpdk: {} runs, nb_rx_queues={}",
                 bench_cfg.parallel.runs.size(), nb_queues);

    // ── Build LcorePin[] and EAL config ──────────────────────────────
    std::vector<edpdk::LcorePin> pins;
    pins.reserve(bench_cfg.parallel.runs.size());
    for (const auto& r : bench_cfg.parallel.runs) {
        pins.push_back(edpdk::LcorePin{
            r.lcore, r.cpu,
            std::string{"bench-"} + r.scenario});
    }

    const std::string& dpdk_pci = bench_cfg.networking.nic_b_pci;
    if (dpdk_pci.empty()) {
        spdlog::error("lat_multi_dpdk: networking.nic_b_pci is required");
        return 1;
    }

    edpdk::PlatformConfig pcfg{};
    pcfg.port_id      = 0;
    pcfg.nb_rx_queues = nb_queues;
    pcfg.nb_tx_queues = nb_queues;

    edpdk::EalConfig eal_cfg{};
    eal_cfg.program_name = "lat_multi_dpdk";
    eal_cfg.allowed_devs = {dpdk_pci};
    eal_cfg.extra_args   = {std::string{"--proc-type=auto"},
                            std::string{"--log-level=lib.eal:warning"}};

    eph::utils::CpuPinPolicy pin_policy{
        .require_isolcpus            = false,
        .require_no_sibling_conflict = false,
        .require_same_numa           = false,
        .warn_irq_overlap            = false,
    };

    auto env_r = eph::dpdk::test::DpdkBenchEnv::create(
        std::move(pcfg), std::move(eal_cfg),
        std::span<edpdk::LcorePin const>{pins}, pin_policy,
        bench_cfg.networking.server_ip,
        bench_cfg.networking.client_ip,
        bench_cfg.networking.gateway_ip);
    if (!env_r) {
        spdlog::error("lat_multi_dpdk: DpdkBenchEnv::create: {}", env_r.error());
        return 2;
    }
    auto env = std::move(*env_r);
    bench::print_dpdk_config_echo(env);
    bench::DpdkBenchView view = bench::make_view(env);

    // ── One Poller per queue, registered with Platform ───────────────
    std::vector<std::unique_ptr<ed::DpdkPoller<>>> pollers;
    pollers.reserve(nb_queues);
    for (uint16_t q = 0; q < nb_queues; ++q) {
        ed::PollerConfig pc{};
        pc.port_id     = env.port_id;
        pc.rx_queue_id = q;
        auto pr = ed::DpdkPoller<>::create(pc);
        if (!pr) {
            spdlog::error("lat_multi_dpdk: Poller::create(q={}): {}",
                          q, pr.error().detail);
            return 2;
        }
        if (auto rr = env.platform.register_poller(q, pr->get()); !rr) {
            spdlog::error("lat_multi_dpdk: register_poller(q={}): {}",
                          q, rr.error().detail);
            return 2;
        }
        pollers.emplace_back(std::move(*pr));
    }

    // ── Build per-run BenchCtx + WorkerCtx ───────────────────────────
    std::vector<WorkerCtx> ctxs;
    ctxs.reserve(bench_cfg.parallel.runs.size());
    for (size_t i = 0; i < bench_cfg.parallel.runs.size(); ++i) {
        const auto& r = bench_cfg.parallel.runs[i];
        const bench::Scenario* sc = bench_cfg.scenario(r.scenario);
        if (sc == nullptr) {
            spdlog::error("lat_multi_dpdk: [scenarios.{}] not found", r.scenario);
            return 1;
        }
        const bool use_tls = sc->get_or<bool>("use_tls", false);

        WorkerCtx wctx{};
        wctx.bench_ctx.bench_cfg    = &bench_cfg;
        wctx.bench_ctx.scenario_cfg = sc;
        wctx.bench_ctx.view         = &view;
        wctx.bench_ctx.poller       = pollers[r.queue].get();
        wctx.bench_ctx.queue_id     = r.queue;
        wctx.bench_ctx.cpu_id       = static_cast<uint16_t>(r.cpu);
        wctx.bench_ctx.lcore_id     = r.lcore;
        wctx.bench_ctx.slot_index   = static_cast<int>(i);
        wctx.bench_ctx.running      = &g_running;
        wctx.scenario_id            = scenario_id_from_name(r.scenario);
        wctx.use_tls                = use_tls;
        ctxs.push_back(std::move(wctx));
    }

    // ── Dispatch: workers on lcores 1..N-1, main lcore runs slot 0 ──
    // Map runs[i].lcore → ctxs[i] for rte_eal_remote_launch.
    spdlog::info("lat_multi_dpdk: launching {} worker(s)", ctxs.size());
    int slot0_rc = 0;
    bool slot0_dispatched_to_main = false;
    const unsigned main_lcore = rte_lcore_id();
    for (size_t i = 0; i < ctxs.size(); ++i) {
        const auto& r = bench_cfg.parallel.runs[i];
        if (r.lcore == main_lcore) {
            // Main lcore runs slot i directly — but defer until after
            // we've launched the others, so DPDK doesn't go single-
            // threaded waiting for our return.
            slot0_dispatched_to_main = true;
            continue;
        }
        int rc = rte_eal_remote_launch(worker_main, &ctxs[i], r.lcore);
        if (rc != 0) {
            spdlog::error("lat_multi_dpdk: rte_eal_remote_launch(lcore={}, slot={}) failed: rc={}",
                          r.lcore, i, rc);
            g_running.store(false, std::memory_order_release);
            return 2;
        }
    }
    // Now main lcore runs whichever slot was assigned to it (typically slot 0).
    for (size_t i = 0; i < ctxs.size(); ++i) {
        const auto& r = bench_cfg.parallel.runs[i];
        if (r.lcore == main_lcore) {
            slot0_rc = worker_main(&ctxs[i]);
            (void)slot0_rc;
            break;
        }
    }
    if (!slot0_dispatched_to_main) {
        spdlog::warn("lat_multi_dpdk: main lcore {} not in runs[]; main thread will idle "
                     "until workers finish", main_lcore);
    }

    // ── Wait all workers ─────────────────────────────────────────────
    for (size_t i = 0; i < ctxs.size(); ++i) {
        const auto& r = bench_cfg.parallel.runs[i];
        if (r.lcore == main_lcore) continue;
        const int rc = rte_eal_wait_lcore(r.lcore);
        spdlog::info("lat_multi_dpdk: lcore={} (slot={}) joined rc={}",
                     r.lcore, i, rc);
    }

    // ── Summary ──────────────────────────────────────────────────────
    int worst_rc = 0;
    std::printf("\n=== lat_multi_dpdk summary ===\n");
    for (size_t i = 0; i < ctxs.size(); ++i) {
        const auto& r = bench_cfg.parallel.runs[i];
        std::printf("  [slot=%zu] scenario=%s lcore=%u cpu=%d queue=%u rc=%d\n",
                    i, r.scenario.c_str(),
                    static_cast<unsigned>(r.lcore), r.cpu,
                    static_cast<unsigned>(r.queue),
                    ctxs[i].result_rc);
        if (ctxs[i].result_rc != 0 && worst_rc == 0) {
            worst_rc = ctxs[i].result_rc;
        }
    }
    std::fflush(stdout);

    return worst_rc;
}
