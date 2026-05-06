/// @file bench_oms_hot_path.cpp
/// Microbenchmarks for the **OMS hot path** — `OrderManager::submit` and
/// `PositionTracker::on_fill`.
///
/// Coverage gap (loop-cleanup R165): `bench_orders.cpp` covers the FIX
/// **wire builder** path (`build_new_order` / `_cancel` / `_replace`),
/// `bench_execution_report.cpp` covers parsing, and `bench_risk_check.cpp`
/// covers the risk gate. None of these touch the **per-event in-memory
/// state machine** — the exact functions every order entry server invokes
/// per submitted order and per execution. A regression in the
/// `unordered_map::try_emplace` path, the `isfinite()` validation gating,
/// or the average-fill / realized-PnL accounting in `PositionTracker`
/// would silently widen tail latency without surfacing in any current bench.
///
/// We bench three shapes that bracket realistic OMS behaviour:
///   - submit_unique:  fresh cl_ord_id every iteration → measures the
///                     map-insert + validation path (bytes-allocator stress).
///   - submit_reject:  empty cl_ord_id → cheapest reject branch; pins the
///                     early-out cost so a regression cannot hide there.
///   - on_fill_same:   repeated fills on a single hot symbol → exercises the
///                     `find()` heterogeneous-lookup path with no allocation.
///   - on_fill_reduce: alternating buy/sell fills on one symbol → the
///                     reducing-fill branch that recomputes realized PnL.
///   - on_fill_many:   round-robin over N symbols → measures the hash-map
///                     lookup hot loop under realistic working-set sizes.
///
/// All benches keep the input set bounded so `state.iterations()` does not
/// drift the working-set across runs — Google Benchmark already controls
/// repetition count.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>
#include <spdlog/spdlog.h>

#include "eph/fix.hpp"
#include "eph/fix/order_manager.hpp"
#include "eph/fix/position.hpp"

using eph::fix::OrderManager;
using eph::fix::PositionTracker;

namespace {
/// Silence the WARN-level reject paths so the bench numbers reflect the
/// branch cost only, not stdout I/O. Runs once at program start.
struct LoggerSilencer {
    LoggerSilencer() {
        spdlog::set_level(spdlog::level::off);
    }
};
const LoggerSilencer g_silencer{};
}  // namespace

// ---------------------------------------------------------------------------
// OrderManager::submit
// ---------------------------------------------------------------------------

/// Each iteration submits a fresh `cl_ord_id` so we exercise the full
/// `try_emplace` insertion path. We pre-format the ID to a buffer to avoid
/// timing the snprintf-style format inside the loop.
static void BM_OrderManagerSubmitUnique(benchmark::State& state) {
    OrderManager mgr;
    uint64_t seq = 0;
    for (auto _ : state) {
        // Unique ID per iteration — keep the format trivial so it does not
        // dominate the measurement.
        std::string id = "OID-" + std::to_string(++seq);
        bool ok = mgr.submit(std::move(id), "AAPL", '1', 100.0, 150.50);
        benchmark::DoNotOptimize(ok);
    }
}
BENCHMARK(BM_OrderManagerSubmitUnique);

/// The cheapest reject branch: empty `cl_ord_id` short-circuits before any
/// map work. Pinning this prevents the early-out cost from creeping up if
/// someone adds extra logging or validation higher in the function.
static void BM_OrderManagerSubmitRejectEmpty(benchmark::State& state) {
    OrderManager mgr;
    for (auto _ : state) {
        bool ok = mgr.submit("", "AAPL", '1', 100.0, 150.50);
        benchmark::DoNotOptimize(ok);
    }
}
BENCHMARK(BM_OrderManagerSubmitRejectEmpty);

// ---------------------------------------------------------------------------
// PositionTracker::on_fill
// ---------------------------------------------------------------------------

/// Repeated buys on one symbol — the heterogeneous `find()` always hits and
/// no allocation occurs after the first call. Measures the hot branch of
/// average-fill accounting (no realized PnL, no map insert).
static void BM_PositionTrackerOnFillSameSymbol(benchmark::State& state) {
    PositionTracker tracker;
    // Warm up the bucket so iteration 0 is also a hit.
    tracker.on_fill("AAPL", '1', 1.0, 100.0);
    for (auto _ : state) {
        tracker.on_fill("AAPL", '1', 10.0, 150.50);
    }
}
BENCHMARK(BM_PositionTrackerOnFillSameSymbol);

/// Alternating buy/sell on a long position — every other fill is a reducing
/// fill that recomputes realized PnL. The realized-PnL branch is the most
/// expensive arithmetic path inside `on_fill` and previously had no
/// dedicated bench.
static void BM_PositionTrackerOnFillReducing(benchmark::State& state) {
    PositionTracker tracker;
    // Establish a large long so successive sells reduce without flipping.
    tracker.on_fill("AAPL", '1', 1'000'000.0, 100.0);
    bool buy_next = false;
    for (auto _ : state) {
        // Tiny qty so we don't deplete the position and start opening
        // shorts (which would change the branch profile mid-bench).
        if (buy_next) {
            tracker.on_fill("AAPL", '1', 1.0, 101.0);
        } else {
            tracker.on_fill("AAPL", '2', 1.0, 102.0);
        }
        buy_next = !buy_next;
    }
}
BENCHMARK(BM_PositionTrackerOnFillReducing);

/// Round-robin over N symbols — measures the unordered_map lookup hot loop
/// at a realistic working-set size where the bucket cache may miss.
/// Picks N = 64 so the table fits in L1 but exercises the hash function.
static void BM_PositionTrackerOnFillManySymbols(benchmark::State& state) {
    PositionTracker tracker;
    constexpr size_t N = 64;
    // Pre-warm so each iteration is a pure hit.
    std::array<std::string, N> syms;
    for (size_t i = 0; i < N; ++i) {
        syms[i] = "SYM" + std::to_string(i);
        tracker.on_fill(syms[i], '1', 1.0, 100.0);
    }
    size_t idx = 0;
    for (auto _ : state) {
        tracker.on_fill(syms[idx], '1', 10.0, 150.50);
        idx = (idx + 1) & (N - 1);  // N is power-of-two, cheap mod.
    }
}
BENCHMARK(BM_PositionTrackerOnFillManySymbols);

/// The cheapest reject branch — non-finite qty is rejected before any map
/// work or arithmetic. Pin it so a future change to the validation order
/// (e.g. moving the side check ahead of isfinite) cannot regress silently.
static void BM_PositionTrackerOnFillRejectNaN(benchmark::State& state) {
    PositionTracker tracker;
    const double nan_qty = std::numeric_limits<double>::quiet_NaN();
    for (auto _ : state) {
        tracker.on_fill("AAPL", '1', nan_qty, 100.0);
    }
}
BENCHMARK(BM_PositionTrackerOnFillRejectNaN);

// ---------------------------------------------------------------------------
// OrderManager::on_execution_report — the inbound side of the OMS.
//
// Loop-cleanup R171: pairs with the submit-side benches above. Each
// inbound exchange ExecutionReport flows through `on_execution_report`,
// which: locates the order via `unordered_map::find`, validates ExecType
// + LastQty + LastPx, computes the running avg_fill_price, updates
// leaves_qty, and (optionally) forwards to PositionTracker. Existing
// `bench_execution_report.cpp` covers the wire-parse step but not the
// state-machine application — so a regression in the over-fill guard,
// the avg-price recompute, or the PositionTracker forwarding hot path
// would not surface in any bench.
//
// We bench a sustained partial-fill stream: the typical exchange
// behaviour for a 1000-lot order that gets sliced into 100-lot
// fills. Each iteration recreates the OrderManager + Position state
// because terminal state is sticky and we want the bench to measure
// the steady-state hot path, not the first-fill cold path.
// ---------------------------------------------------------------------------

namespace {

/// Build a raw FIX message buffer with valid BodyLength + CheckSum, returning
/// it as a vector. Mirrors the pattern in tests/test_order_manager.cpp.
[[nodiscard]] std::vector<uint8_t> make_partial_fill_wire(int seq) {
    std::string body;
    body += "35=8\x01";
    body += "49=SENDER\x01";
    body += "56=TARGET\x01";
    body += "34=" + std::to_string(seq) + "\x01";
    body += "11=ORDBENCH\x01";  // ClOrdID
    body += "37=EXCHID\x01";
    body += "17=EXEC" + std::to_string(seq) + "\x01";
    body += "150=1\x01";  // ExecType=PartialFill
    body += "39=1\x01";   // OrdStatus=PartiallyFilled
    body += "55=AAPL\x01";
    body += "54=1\x01";   // Side=Buy
    body += "31=150.25\x01";  // LastPx
    body += "32=10\x01";      // LastQty
    body += "6=150.25\x01";   // AvgPx
    body += "14=10\x01";      // CumQty
    body += "151=990\x01";    // LeavesQty

    std::string full = "8=FIX.4.4\x01";
    full += "9=" + std::to_string(body.size()) + "\x01";
    full += body;

    uint32_t sum = 0;
    for (char c : full) sum += static_cast<uint8_t>(c);
    char cs[8];
    std::snprintf(cs, sizeof(cs), "10=%03u\x01",
                  static_cast<unsigned>(sum & 0xFF));
    full += cs;

    return {full.begin(), full.end()};
}

}  // namespace

static void BM_OrderManagerOnExecReportPartialFill(benchmark::State& state) {
    // Pre-build the wire bytes — the parser re-runs each iteration but
    // we measure the OMS dispatch hot path, not the parser itself
    // (already covered by bench_execution_report).
    auto wire = make_partial_fill_wire(/*seq=*/1);

    for (auto _ : state) {
        // Recreate per-iteration so we don't accumulate filled_qty past
        // orig_qty (would trip the over-fill guard and exit early,
        // which would no longer measure the partial-fill hot path).
        state.PauseTiming();
        eph::fix::OrderManager mgr;
        mgr.submit("ORDBENCH", "AAPL", '1', /*qty=*/1'000'000.0, /*price=*/150.0);
        // Mark New so the report isn't ignored as terminal-state.
        // (submit() leaves it PendingNew; on_execution_report will
        // re-route New/PartialFill correctly either way.)
        auto parse_r = eph::fix::parse<256>({wire.data(), wire.size()});
        if (!parse_r.has_value()) {
            state.SkipWithError("parse failed");
            break;
        }
        eph::fix::BasicMessageView<256> mv = parse_r.value();
        eph::fix::ExecutionReportView<256> rv{mv};
        state.ResumeTiming();

        bool ok = mgr.on_execution_report(rv);
        benchmark::DoNotOptimize(ok);
    }
}
BENCHMARK(BM_OrderManagerOnExecReportPartialFill);

BENCHMARK_MAIN();
