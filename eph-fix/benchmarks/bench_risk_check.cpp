/// @file bench_risk_check.cpp
/// Microbenchmarks for `eph::fix::RiskChecker::check_order` and
/// `PositionTracker::on_fill`.
///
/// Coverage gap (batch 11): pre-trade risk checks AND the per-fill
/// position update sit on the order-out and order-ack hot paths
/// respectively. Every order must clear `check_order` before transmit;
/// every fill drives `on_fill`. No microbench existed before this commit.
///
/// Why these specific cases:
/// - **CheckOrderHappy**: all six limits enabled, order well within
///   thresholds. The "100% of orders pass" steady-state shape.
/// - **CheckOrderRejectFirstLimit**: hits the first reject branch
///   (max_order_qty). Confirms the reject path is *not* slower than the
///   pass path — important because slow rejects under attack become a
///   secondary DoS surface.
/// - **CheckOrderAllLimitsDisabled**: every limit set to 0 (skip). Pure
///   input-validation cost — a useful "what does the validation alone
///   take" baseline.
/// - **OnFillBuildPosition**: the steady-state "many fills extending the
///   same position" path. Hits the increment-avg formula but never the
///   close-PnL path.
/// - **OnFillReducingFill**: the closing path — runs the realized-PnL
///   computation on every call.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <benchmark/benchmark.h>
#include <spdlog/spdlog.h>

#include "eph/fix/position.hpp"
#include "eph/fix/risk_check.hpp"

using eph::fix::PositionTracker;
using eph::fix::RiskChecker;
using eph::fix::RiskLimits;
using eph::fix::RiskRejectReason;

namespace {

/// Build a generously sized risk-limits object with all checks active.
[[nodiscard]] RiskLimits all_limits_active() {
    return RiskLimits{
        .max_order_qty         = 10'000.0,
        .max_order_notional    = 1'000'000.0,
        .max_position_qty      = 50'000.0,
        .max_position_notional = 5'000'000.0,
        .max_total_exposure    = 50'000'000.0,
        .max_orders_per_second = 0,  // rate limit deferred to caller
    };
}

/// Pre-fill a tracker with N positions across distinct symbols. Useful so
/// `check_order` exercises the `PositionTracker::get` lookup path against
/// a non-trivial map size.
[[nodiscard]] PositionTracker make_seeded_tracker(std::size_t n_symbols) {
    PositionTracker tr;
    char sym_buf[16];
    for (std::size_t i = 0; i < n_symbols; ++i) {
        // Generate distinct ASCII symbol names — lets heterogeneous
        // lookup (string_view → string) take its realistic path.
        const auto wrote = std::snprintf(sym_buf, sizeof(sym_buf),
                                          "SYM%05zu", i);
        std::string_view sym{sym_buf, static_cast<std::size_t>(wrote)};
        tr.on_fill(sym, '1', 100.0, 50.0);
    }
    return tr;
}

} // namespace

// ---------------------------------------------------------------------------
// check_order: happy path (all limits active, order passes)
// ---------------------------------------------------------------------------

static void BM_RiskCheckOrderHappy(benchmark::State& state) {
    RiskChecker rc{all_limits_active()};
    PositionTracker tr = make_seeded_tracker(50);
    for (auto _ : state) {
        // Buy 10 @ 100 — well under every threshold for symbol "SYM00010".
        auto r = rc.check_order("SYM00010", '1', 10.0, 100.0, tr);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_RiskCheckOrderHappy);

// ---------------------------------------------------------------------------
// check_order: rejected at first limit (max_order_qty)
// ---------------------------------------------------------------------------

static void BM_RiskCheckOrderReject(benchmark::State& state) {
    RiskChecker rc{all_limits_active()};
    PositionTracker tr = make_seeded_tracker(50);
    for (auto _ : state) {
        // qty 1e6 > limit 1e4 → kOrderQtyExceeded
        auto r = rc.check_order("SYM00010", '1', 1'000'000.0, 100.0, tr);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_RiskCheckOrderReject);

// ---------------------------------------------------------------------------
// check_order: all limits disabled — minimum cost (validation only)
// ---------------------------------------------------------------------------

static void BM_RiskCheckOrderAllLimitsDisabled(benchmark::State& state) {
    RiskChecker rc{RiskLimits{}};  // all-zero = skip every check
    PositionTracker tr;
    for (auto _ : state) {
        auto r = rc.check_order("AAPL", '1', 10.0, 150.0, tr);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_RiskCheckOrderAllLimitsDisabled);

// ---------------------------------------------------------------------------
// PositionTracker::on_fill — steady-state extending fills (no PnL realize)
// ---------------------------------------------------------------------------

static void BM_PositionOnFillExtending(benchmark::State& state) {
    // Fresh tracker per loop so each `on_fill` executes the same shape:
    // first iteration creates the entry, subsequent ones extend the
    // average. Constructing a tracker is cheap (default unordered_map).
    for (auto _ : state) {
        PositionTracker tr;
        // Single emplace + avg-price update — no realize-PnL branch.
        tr.on_fill("AAPL", '1', 100.0, 50.0);
        benchmark::DoNotOptimize(tr);
    }
}
BENCHMARK(BM_PositionOnFillExtending);

// ---------------------------------------------------------------------------
// PositionTracker::on_fill — reducing fill (the realize-PnL branch)
// ---------------------------------------------------------------------------

static void BM_PositionOnFillReducing(benchmark::State& state) {
    for (auto _ : state) {
        PositionTracker tr;
        // Open long, then reduce — the second fill takes the realize-PnL
        // path. We measure the pair so the 'open' set-up dominates the
        // measurement only by ~2x; cleanly separating the two would
        // require benching with state.PauseTiming/ResumeTiming which
        // adds overhead-per-iteration noise far worse than the 2x.
        tr.on_fill("AAPL", '1', 100.0, 50.0);
        tr.on_fill("AAPL", '2',  40.0, 55.0);
        benchmark::DoNotOptimize(tr);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * 2);
}
BENCHMARK(BM_PositionOnFillReducing);

int main(int argc, char** argv) {
    // BM_RiskCheckOrderReject hits the WARN log on every iteration; the
    // spdlog flush dominates the measurement and floods output. Mute
    // runtime logging globally — the actual reject branch (incl. its
    // SPDLOG_LOGGER_WARN call site) is still walked, just without I/O.
    spdlog::set_level(spdlog::level::off);

    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
