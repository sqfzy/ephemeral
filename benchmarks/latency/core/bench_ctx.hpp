/// @file core/bench_ctx.hpp
/// Per-scenario worker context for the `run_lat_<sc>_loop()` functions
/// extracted into `benchmarks/latency/scenarios/`.
///
/// **Purpose**: each lat_*.cpp historically held its measurement state
/// (Recorder, Stream, Poller, payload, deadline, sample_idx, etc.) in
/// main()'s local scope and closed over it from on_message lambdas.
/// Lifting the inner loops to standalone functions in `scenarios/`
/// keeps the closure surface compact: each scenario function receives
/// its state via this value type instead of relying on main() locals.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace bench {

/// Return a per-process JSON output suffix when running in MP mode
/// (autojoin), else empty string.
///
/// In MP mode, each peer process produces its own JSON output. Without
/// a per-process discriminator the files collide on disk — the second
/// process to flush silently overwrites the first's measurements.
/// This helper appends `_pid<getpid()>` so each process gets a unique
/// filename (e.g. `lat_tcp_dpdk_rtt_pid12345_<ts>.json`).
///
/// Detection: `EPH_LAT_AUTOJOIN_MAX_PROCS` envvar non-empty. This is
/// the same envvar that triggers the MP autojoin path in dpdk_env.hpp,
/// so the suffix is on iff the user opted into multi-process mode.
/// Single-process bench commands (`lat tcp --dpdk`) leave the suffix
/// empty, preserving byte-equal output filenames with the legacy path.
[[nodiscard]] inline std::string mp_output_suffix() noexcept {
    if (const char* mp = std::getenv("EPH_LAT_AUTOJOIN_MAX_PROCS");
        mp && *mp) {
        return std::string{"_pid"} + std::to_string(::getpid());
    }
    return {};
}


class BenchConfig;
class Scenario;
struct DpdkBenchView;

namespace ed {
// Forward decls so this header doesn't need to include
// eph/net/dpdk/poller.hpp; the Poller pointer is opaque to BenchCtx
// (only the per-scenario function casts and uses it).
template <class P> class DpdkPoller;
} // namespace ed

/// Per-scenario worker context. Move-only by virtue of holding pointers.
struct BenchCtx {
    /// Loaded `config.toml` (top-level `[networking]` / `[cpu]` /
    /// `[scenarios.*]`). Non-owning.
    const BenchConfig* bench_cfg = nullptr;

    /// `[scenarios.<this run's scenario name>]`. Non-owning.
    const Scenario*    scenario_cfg = nullptr;

    /// Shared DPDK view (Platform&, IPs, MACs, port_id, pool).
    /// Non-owning. nullptr is allowed when the scenario constructs its
    /// own DpdkBenchEnv (kernel-only scenarios that don't need the view).
    DpdkBenchView*     view = nullptr;

    /// Per-queue Poller, registered with Platform on this run's
    /// `queue_id`. Non-owning; main() owns the unique_ptr. Type erased
    /// to keep this header light; scenario functions re-declare the
    /// typed pointer as needed.
    void*              poller = nullptr;

    /// RX queue this scenario polls. Derived from
    /// `env.platform.effective_rx_queue_range().first` so MP-secondary
    /// processes bind to the queue they actually own.
    uint16_t           queue_id = 0;

    /// Shared shutdown flag. Points at the binary's own
    /// `bench::shutdown_requested()` proxy or a static atomic.
    std::atomic<bool>* running = nullptr;
};

} // namespace bench
