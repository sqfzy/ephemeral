/// @file core/bench_ctx.hpp
/// Per-scenario worker context for the parallel-bench `lat_multi_dpdk`
/// runner and for the per-scenario `run_lat_<sc>_loop()` extracted
/// inner-measurement-loop functions in `benchmarks/latency/scenarios/`.
///
/// **Purpose**: each lat_*.cpp historically held its measurement state
/// (Recorder, Stream, Poller, payload, deadline, sample_idx, etc.) in
/// main()'s local scope and closed over it from on_message lambdas.
/// To let `lat_multi_dpdk` run N scenarios concurrently in one process
/// (one EAL worker lcore per scenario, sharing one Platform), the inner
/// loops must become standalone functions that receive their state via
/// a value type instead of relying on main() locals.
///
/// `BenchCtx` is that value type. It holds **non-owning references** to
/// the bench config, the per-scenario Scenario subtable, the shared
/// `DpdkBenchView` (which references the single Platform), the per-queue
/// Poller, and the shared shutdown flag. Each scenario function turns
/// its previously-captured locals (Recorders, payload buffers, ts_buf,
/// frame encoders) into function-local variables.
///
/// In single-binary mode (`lat tcp --dpdk` etc.), main() constructs
/// a BenchCtx with `slot_index = -1` and calls the same scenario
/// function — so the byte-level behaviour is preserved by design
/// (slot_index < 0 → no `_slot<i>` suffix on Recorder names → original
/// JSON filename format).

#pragma once

#include <atomic>
#include <cstdint>

namespace bench {

class BenchConfig;
class Scenario;
struct DpdkBenchView;

namespace dpdk {
// Forward-declared: full type lives in
// `benchmarks/latency/core/dpdk_env.hpp`. This header keeps
// pure-kernel scenario binaries from pulling in DPDK headers via
// the `BenchCtx` field.
}

namespace ed {
// Forward decls so this header doesn't need to include
// eph/net/dpdk/poller.hpp; the Poller pointer is opaque to BenchCtx
// (only the per-scenario function casts and uses it).
template <class P> class DpdkPoller;
} // namespace ed

/// Per-scenario worker context. Move-only by virtue of holding pointers
/// (no copies expected; each worker lcore gets its own instance).
///
/// Lifetime: BenchCtx outlives every worker thread that reads from it.
/// In `lat_multi_dpdk`, all BenchCtx instances live in main() until
/// `rte_eal_wait_lcore` returns for every worker.
struct BenchCtx {
    /// Loaded `config.toml` (top-level `[networking]` / `[cpu]` /
    /// `[scenarios.*]`). Non-owning.
    const BenchConfig* bench_cfg = nullptr;

    /// `[scenarios.<this run's scenario name>]`. Non-owning.
    const Scenario*    scenario_cfg = nullptr;

    /// Shared DPDK view (Platform&, IPs, MACs, port_id, pool).
    /// Non-owning. Single-binary mode: nullptr is allowed when the
    /// scenario function constructs its own DpdkBenchEnv (kernel-only
    /// scenarios that don't need the view path).
    DpdkBenchView*     view = nullptr;

    /// Per-queue Poller, registered with Platform on this run's
    /// `queue_id`. Non-owning; lat_multi_dpdk owns the unique_ptr.
    /// Type erased to keep this header light; scenario functions
    /// re-declare the typed pointer as needed.
    void*              poller = nullptr;

    /// RX queue this scenario polls. In autojoin / mp_topology this
    /// would be derived from `effective_rx_queue_range()`; for the
    /// single-process N-lcore runner it is set directly from
    /// `[parallel].runs[i].queue`.
    uint16_t           queue_id = 0;

    /// CPU this run's lcore is pinned to (per `[parallel].runs[i].cpu`).
    /// Informational; pin already happened during `Platform::create_with_eal`.
    uint16_t           cpu_id = 0;

    /// EAL lcore id this scenario runs on (per `[parallel].runs[i].lcore`).
    uint16_t           lcore_id = 0;

    /// Slot index in `[parallel].runs[]`. **Negative = single-binary
    /// mode** (no `_slot<i>` JSON suffix → byte-equal with pre-reshape
    /// `lat <sc> --dpdk` output). Non-negative = multi-scenario mode.
    int                slot_index = -1;

    /// Shared shutdown flag. Single-binary mode points at the binary's
    /// own `bench::shutdown_requested()` proxy or a static atomic;
    /// lat_multi_dpdk points at its global `g_running`.
    std::atomic<bool>* running = nullptr;
};

} // namespace bench
