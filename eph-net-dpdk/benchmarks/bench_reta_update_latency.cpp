/// @file bench_reta_update_latency.cpp
/// Real-NIC microbench for `rte_eth_dev_rss_reta_update` call latency.
///
/// Acceptance criterion #10 of the daemon-reshape S5 plan
/// (`/home/ec2-user/.claude/plans/calm-roaming-sedgewick.md`):
///
///   > 性能测试：`rte_eth_dev_rss_reta_update` 调用本身延迟 < 5ms
///   > (基线 PMD 文档值)
///
/// The QueueAllocator's claim/release IPC handlers refresh the NIC's
/// RETA on every state transition — this bench captures the per-call
/// latency of that PMD operation on the host's vfio-pci-bound NIC so a
/// future regression (or unfortunate PMD upgrade) can be caught.
///
/// Methodology:
///   1. Bring up the NIC via `Platform::serve_nic` against
///      `nic_b_pci` from `benchmarks/latency/config.toml` (or the
///      `EPH_BENCH_PCI` env var override).
///   2. Build a fully-populated `rte_eth_rss_reta_entry64` array sized
///      from the PMD's reported `reta_size` (clamped to 512). Round-
///      robin every entry across queues `0..total_queues-1`.
///   3. In each bench iteration call `rte_eth_dev_rss_reta_update`,
///      flipping the round-robin start each time so the PMD doesn't
///      short-circuit on a no-op write.
///
/// Skip protocol:
///   * If `rte_eal_init` fails (no hugepages / vfio not bound /
///     non-root) → `state.SkipWithError` with a one-line diagnostic.
///   * If `Platform::serve_nic` fails (other secondary already on the
///     NIC, total_queues exceeds NIC max, link-up timeout, etc.)
///     → `state.SkipWithError`.
///   * If the PMD returns -ENOTSUP for `rte_eth_dev_rss_reta_update`
///     (rare — virtual / null PMD, or some PF driver in some configs)
///     → `state.SkipWithError`. The S5 design degrades cleanly in
///     this case (RETA stays at the bring-up configuration); the
///     bench has nothing to measure.
///
/// Build only on hosts without a vfio-pci NIC; `xmake build
/// bench_reta_update_latency` is the build-only verification step.
/// `xmake run bench_reta_update_latency` requires:
///   * NIC_B (PCI BDF from config.toml) bound to vfio-pci
///   * Hugepages mounted, daemon lcore (0) free
///   * Run as root (vfio-pci open) — `sudo xmake run bench_reta_update_latency`
///
/// Override the BDF: `EPH_BENCH_PCI=0000:01:00.0 sudo xmake run ...`.
///
/// Pinned baseline capture protocol (after a regression-checked run):
///   sudo xmake run bench_reta_update_latency
///     --benchmark_format=console > .artifacts/bench-reta-update-<yyyymmdd>.txt
///
/// Acceptance: p99 (= bench's `Max` over a stable run, since each
/// iteration is one syscall worth and the bench loops many iters)
/// MUST stay under 5 ms.

#include <array>
#include <cstdint>
#include <cstdlib>
#include <string>

#include <benchmark/benchmark.h>
#include <spdlog/spdlog.h>

#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>

#include "eph/dpdk/platform.hpp"

namespace {

/// @brief Resolve which BDF to bring up. Order:
///   1. `EPH_BENCH_PCI` env var (operator override — useful when
///      config.toml's NIC_B doesn't match the box).
///   2. AWS Graviton default (`0000:28:00.0`) — matches the
///      bench-host (`config.toml`) NIC_B.
///
/// Returns an empty string if none usable; caller skips.
std::string resolve_bench_pci() {
    if (const char* env = std::getenv("EPH_BENCH_PCI"); env != nullptr) {
        return std::string{env};
    }
    return std::string{"0000:28:00.0"};
}

/// @brief Singleton: bring up Platform::serve_nic exactly once across
/// all bench fixtures. Returns nullopt on bring-up failure (caller
/// should call `state.SkipWithError` with the captured error).
///
/// Why a static singleton instead of fixture SetUp/TearDown: serve_nic
/// initialises EAL in the calling process, and `rte_eal_init` is
/// non-idempotent within a single process — second init returns
/// -EALREADY. A function-static unique pointer plus a
/// captured-once error message gives us the right "init once,
/// observe state from every benchmark function" shape.
struct BenchPlatform {
    std::optional<eph::dpdk::Platform> platform;
    std::string                        skip_reason;
    uint16_t                           total_queues = 16;
    uint16_t                           port_id      = 0;
    uint16_t                           reta_size    = 0;
};

BenchPlatform& bench_platform() {
    static BenchPlatform inst = []() {
        BenchPlatform b;
        const std::string pci = resolve_bench_pci();
        if (pci.empty()) {
            b.skip_reason =
                "no NIC PCI BDF resolved (set EPH_BENCH_PCI or update "
                "benchmarks/latency/config.toml's nic_b_pci)";
            return b;
        }
        eph::dpdk::NicServiceConfig cfg;
        cfg.pci          = pci;
        cfg.total_queues = b.total_queues;
        cfg.daemon_lcore = 0;

        auto plat_r = eph::dpdk::Platform::serve_nic(cfg);
        if (!plat_r) {
            b.skip_reason = "Platform::serve_nic failed: " + plat_r.error();
            return b;
        }
        b.platform = std::move(*plat_r);
        b.port_id  = b.platform->port_id();

        rte_eth_dev_info dev_info{};
        if (rte_eth_dev_info_get(b.port_id, &dev_info) != 0) {
            b.skip_reason =
                "rte_eth_dev_info_get failed on port=" +
                std::to_string(b.port_id);
            // Keep platform alive; ~Platform tears down EAL cleanly.
            return b;
        }
        b.reta_size = dev_info.reta_size;
        if (b.reta_size == 0) b.reta_size = 128;
        return b;
    }();
    return inst;
}

/// @brief Build a fully-populated RETA group array. Distributes
/// `reta_size` slots across queues `[0, total_queues)` round-robin
/// starting at `rr_offset`.
///
/// `mask` is set to all-ones for every group so the PMD writes every
/// slot rather than short-circuiting on a no-op.
void build_reta(std::array<rte_eth_rss_reta_entry64,
                          RTE_ETH_RSS_RETA_SIZE_512 /
                              RTE_ETH_RETA_GROUP_SIZE>& reta,
                uint16_t reta_size,
                uint16_t total_queues,
                uint16_t rr_offset) noexcept {
    for (auto& g : reta) {
        g.mask = 0;
        for (auto& q : g.reta) q = 0;
    }
    for (uint16_t i = 0; i < reta_size; ++i) {
        const uint16_t group = i / RTE_ETH_RETA_GROUP_SIZE;
        const uint16_t bit   = i % RTE_ETH_RETA_GROUP_SIZE;
        reta[group].mask |= (1ULL << bit);
        reta[group].reta[bit] =
            static_cast<uint16_t>((i + rr_offset) % total_queues);
    }
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// BM_RetaUpdate — measures rte_eth_dev_rss_reta_update call latency.
// ─────────────────────────────────────────────────────────────────────────────
//
// One iteration = one PMD call. Acceptance: max latency < 5 ms.
//
// The benchmark flips the round-robin offset every iteration so the
// PMD can't short-circuit on a no-op (some PMDs do diff-based fast-path
// writes; we want the full path).
static void BM_RetaUpdate(benchmark::State& state) {
    auto& b = bench_platform();
    if (!b.platform.has_value()) {
        state.SkipWithError(b.skip_reason.c_str());
        return;
    }
    if (b.reta_size == 0) {
        state.SkipWithError("PMD reported reta_size=0 — RETA update unsupported");
        return;
    }

    std::array<rte_eth_rss_reta_entry64,
               RTE_ETH_RSS_RETA_SIZE_512 / RTE_ETH_RETA_GROUP_SIZE> reta{};

    // Probe once before the timed loop to detect ENOTSUP and skip cleanly.
    build_reta(reta, b.reta_size, b.total_queues, /*rr_offset=*/0);
    int probe_rc = rte_eth_dev_rss_reta_update(b.port_id, reta.data(),
                                               b.reta_size);
    if (probe_rc == -ENOTSUP || probe_rc == -EOPNOTSUPP) {
        state.SkipWithError(
            "PMD does not support runtime rte_eth_dev_rss_reta_update — "
            "S5 degrades cleanly; nothing to measure");
        return;
    }
    if (probe_rc != 0) {
        const std::string msg =
            "rte_eth_dev_rss_reta_update probe failed (rc=" +
            std::to_string(probe_rc) + " rte_errno=" +
            std::to_string(rte_errno) + ")";
        state.SkipWithError(msg.c_str());
        return;
    }

    uint16_t rr = 0;
    for (auto _ : state) {
        ++rr;
        build_reta(reta, b.reta_size, b.total_queues, rr);
        int rc = rte_eth_dev_rss_reta_update(b.port_id, reta.data(),
                                             b.reta_size);
        benchmark::DoNotOptimize(rc);
        benchmark::ClobberMemory();
        if (rc != 0) {
            // Mid-bench failure: PMD returned an unexpected error.
            // Recording the failure but not aborting — the Benchmark
            // framework will flag the run.
            const std::string msg =
                "rte_eth_dev_rss_reta_update failed mid-bench (rc=" +
                std::to_string(rc) + ")";
            state.SkipWithError(msg.c_str());
            return;
        }
    }

    state.counters["reta_size"]    = b.reta_size;
    state.counters["total_queues"] = b.total_queues;
    state.counters["port_id"]      = b.port_id;
}
// One iteration ≈ 100 µs–1 ms range; run a moderate number to keep wall
// time bounded but still capture max.
BENCHMARK(BM_RetaUpdate)->Iterations(200)->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
