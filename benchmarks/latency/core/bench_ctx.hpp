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
/// (i.e. multiple bench peer processes attaching to a shared
/// `eph-nicd` daemon), else empty string. The historical "autojoin"
/// shape is gone — see the daemon-reshape note further down.
///
/// In MP mode, each peer process produces its own JSON output. Without
/// a per-process discriminator the files collide on disk — the second
/// process to flush silently overwrites the first's measurements.
/// This helper appends `_pid<getpid()>` so each process gets a unique
/// filename (e.g. `lat_tcp_dpdk_rtt_pid12345_<ts>.json`).
///
/// Detection (post-daemon-reshape, 2026-05-02 onward): the autojoin
/// envvar gating (`EPH_LAT_AUTOJOIN_MAX_PROCS`) is gone — under the
/// daemon model every `lat_*_dpdk` binary is inherently a secondary
/// peer that the orchestrator may launch in parallel against the same
/// shared daemon, so there's no negotiable "MP mode" flag to key off.
/// We therefore append the suffix unconditionally **under DPDK
/// builds** (`EPH_USE_DPDK` defined at compile time) and never for
/// kernel-only builds (single-process by construction — kernel
/// scenarios share no state and a per-PID suffix would just clutter
/// the output directory). An optional env override `EPH_LAT_FORCE_PID`
/// is honoured in either build for tests / debug runs that want the
/// suffix forcibly enabled or disabled (`=0` to disable, anything
/// else non-empty to force-enable). The legacy
/// `EPH_LAT_AUTOJOIN_MAX_PROCS` envvar is **also** still honoured
/// here for the strict-parse `[2,64]` contract: an explicit set of
/// the legacy var continues to enable the suffix (back-compat for any
/// outside-the-tree script still exporting it). Setting it to a
/// rejected value (`garbage` / `0` / `1` / `99`) is a no-op — same
/// as before the reshape.
[[nodiscard]] inline std::string mp_output_suffix() noexcept {
    auto pid_suffix = []() {
        return std::string{"_pid"} + std::to_string(::getpid());
    };

    // Hard override — wins over both the build-mode default and the
    // legacy envvar. `=0` / `=false` / `=no` force-disable; any other
    // non-empty value force-enables.
    if (const char* force = std::getenv("EPH_LAT_FORCE_PID");
            force && *force) {
        const std::string_view v{force};
        if (v == "0" || v == "false" || v == "FALSE" || v == "no" || v == "NO") {
            return std::string{};
        }
        return pid_suffix();
    }

    // Legacy envvar — strict-parse contract preserved verbatim from the
    // pre-reshape gate (commit `c7989efb`) for any out-of-tree caller
    // still exporting it. A non-numeric / out-of-range value is treated
    // as "not MP" rather than misconfigured-MP, so a typo never
    // silently injects a `_pid<n>` suffix on a kernel run.
    if (const char* mp = std::getenv("EPH_LAT_AUTOJOIN_MAX_PROCS");
            mp && *mp) {
        char* end = nullptr;
        const unsigned long mp_raw = std::strtoul(mp, &end, 10);
        if (end != mp && *end == '\0' && mp_raw >= 2 && mp_raw <= 64) {
            return pid_suffix();
        }
    }

#if defined(EPH_USE_DPDK)
    // DPDK builds are inherently secondary-attach under the daemon
    // model — N peers may run in parallel against the same NIC, so
    // every output file MUST be per-PID to avoid silent overwrites.
    return pid_suffix();
#else
    // Kernel-only build — single-process by construction; suffix
    // would just be noise. Override available via `EPH_LAT_FORCE_PID`
    // if a test wants it.
    return std::string{};
#endif
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
