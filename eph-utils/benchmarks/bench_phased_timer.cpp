/// @file bench_phased_timer.cpp
/// Microbenchmarks for `eph::utils::PhasedTimer`.
///
/// Coverage gap (loop-cleanup R170): `phased_timer.hpp` is invoked in
/// every benchmark hot loop — `bench_time.cpp` covers `TSC::now()` /
/// `TSC::to_ns()` / `TSC::to_cycles()` but the PhasedTimer wrapper that
/// every microbench and every latency-bench calls per iteration was
/// never benched. The header docstring claims "~20 cycles per
/// iteration"; pinning `is_warmup()` and `is_running()` at a fixed cost
/// makes any future bloat (extra atomic ops, syscall slip, branch
/// mispredict regressions) immediately visible.
///
/// Also pin `start()` — the saturating-add overflow guard lives there;
/// a regression in the saturate path (e.g. promoting to a u128) would
/// be invisible without a bench.

#include <chrono>
#include <cstdint>

#include <benchmark/benchmark.h>

#include "eph/utils/phased_timer.hpp"
#include "eph/utils/time.hpp"

using eph::utils::PhasedTimer;
using eph::utils::TSC;

namespace {
/// Calibrate TSC once at startup so `TSC::to_cycles` does not return
/// nullopt and collapse the deadlines to zero. Same pattern as
/// bench_time.cpp's implicit assumption of an inited TSC.
struct TscBootstrap {
    TscBootstrap() { TSC::init(); }
};
const TscBootstrap g_bootstrap{};
}  // namespace

// ---------------------------------------------------------------------------
// Hot-loop checks: is_running() / is_warmup()
//
// These are called once per bench iteration in every consumer, so any
// regression compounds linearly with iteration count.
// ---------------------------------------------------------------------------

static void BM_PhasedTimerIsRunning(benchmark::State& state) {
    PhasedTimer t;
    // 1h window so the timer never expires during the bench.
    t.start(std::chrono::nanoseconds::zero(), std::chrono::hours{1});
    for (auto _ : state) {
        bool b = t.is_running();
        benchmark::DoNotOptimize(b);
    }
}
BENCHMARK(BM_PhasedTimerIsRunning);

static void BM_PhasedTimerIsWarmup(benchmark::State& state) {
    PhasedTimer t;
    t.start(std::chrono::hours{1}, std::chrono::hours{1});
    for (auto _ : state) {
        bool b = t.is_warmup();
        benchmark::DoNotOptimize(b);
    }
}
BENCHMARK(BM_PhasedTimerIsWarmup);

/// Pre-start (started_=false) — the cheapest path; an `if (!started_)`
/// short-circuit. Pin so a future change that adds extra checks before
/// the started_ test cannot regress the not-yet-armed case.
static void BM_PhasedTimerIsRunningPreStart(benchmark::State& state) {
    PhasedTimer t;  // never started
    for (auto _ : state) {
        bool b = t.is_running();
        benchmark::DoNotOptimize(b);
    }
}
BENCHMARK(BM_PhasedTimerIsRunningPreStart);

// ---------------------------------------------------------------------------
// start() — the saturating-add deadline arming. Cold path (called once
// per bench) but worth pinning so the overflow-guard path stays cheap.
// ---------------------------------------------------------------------------

static void BM_PhasedTimerStart(benchmark::State& state) {
    PhasedTimer t;
    for (auto _ : state) {
        t.start(std::chrono::milliseconds{100},
                std::chrono::seconds{10});
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_PhasedTimerStart);

/// Saturating-add overflow path: passing nanoseconds::max forces
/// to_cycles to return saturated UINT64_MAX, which triggers the clamp
/// branch in start(). Pin the saturated path explicitly.
static void BM_PhasedTimerStartSaturated(benchmark::State& state) {
    PhasedTimer t;
    for (auto _ : state) {
        t.start(std::chrono::nanoseconds::max(),
                std::chrono::nanoseconds::max());
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_PhasedTimerStartSaturated);

BENCHMARK_MAIN();
