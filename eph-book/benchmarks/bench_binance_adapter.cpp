/// @file bench_binance_adapter.cpp
/// Microbenchmarks for `eph::book::BinanceBookAdapter` — the Binance
/// bookTicker → ArrayBook bridge.
///
/// **Coverage gap (loop-cleanup R162):** Binance bookTicker is the
/// canonical BBO snapshot stream — every depth-update on every active
/// pair pushes through `update_from_ticker`. A single subscription to
/// the all-pairs combined stream (`/stream?streams=!bookTicker`) on
/// Binance Spot delivers O(10k–100k msg/s); the cost of this function
/// directly bounds how many pairs an HFT bot can track on one consumer
/// thread. The integration tests cover correctness (basic, snapshot,
/// retire-old, mid/spread) but no microbench existed for the per-tick
/// hot path. Pin the cost so a regression in:
///   - the 4× parse_number on string price/qty fields
///   - the BBO-retire-old logic (last_*_valid_ tracking)
///   - the underlying ArrayBook update_bid/update_ask
/// gets noticed at the bench level.
///
/// We measure against pre-built `BookTicker` structs (not raw JSON)
/// because `parse_number` already has its own bench in eph-core; this
/// file isolates the adapter cost.

#include <benchmark/benchmark.h>
#include <cstdint>
#include <string>

#include "eph/book/binance_adapter.hpp"
#include "eph/json/adapters/binance.hpp"

namespace {

using eph::book::BinanceBookAdapter;
using eph::json::binance::BookTicker;

/// Pre-build a BookTicker with the given price/qty strings. Strings
/// are stored externally (in static buffers) so `string_view` members
/// stay valid for the duration of the bench.
struct TickerHolder {
    std::string symbol;
    std::string bid_price, bid_qty;
    std::string ask_price, ask_qty;
    int64_t update_id;

    BookTicker view() const noexcept {
        BookTicker t;
        t.symbol     = symbol;
        t.bid_price  = bid_price;
        t.bid_qty    = bid_qty;
        t.ask_price  = ask_price;
        t.ask_qty    = ask_qty;
        t.update_id  = update_id;
        return t;
    }
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Same-price path (the most common steady-state case): the BBO is
// stable, only quantities change. Exercises the level-modify code path
// in ArrayBook (no level move, no retire-old branch).
// ---------------------------------------------------------------------------
static void BM_BinanceAdapter_UpdateSamePrice(benchmark::State& state) {
    BinanceBookAdapter<20> adapter;
    TickerHolder t{
        .symbol     = "BTCUSDT",
        .bid_price  = "87245.30",
        .bid_qty    = "1.5",
        .ask_price  = "87245.40",
        .ask_qty    = "2.3",
        .update_id  = 1,
    };
    // Prime the adapter so the first measured call exercises the
    // steady-state same-price branch (last_*_valid_ already true).
    (void)adapter.update_from_ticker(t.view());

    int64_t i = 0;
    for (auto _ : state) {
        // Vary update_id to defeat constant-folding; price/qty stay fixed
        // so the same-price code path dominates.
        t.update_id = ++i;
        bool ok = adapter.update_from_ticker(t.view());
        benchmark::DoNotOptimize(ok);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BinanceAdapter_UpdateSamePrice);

// ---------------------------------------------------------------------------
// Price-move path: every tick reports a new BBO price. This forces
// the retire-old-level branch (qty=0 update on the previous price),
// which is structurally distinct from the same-price path (one extra
// `book_.update_bid(old_price, 0.0)` call per side that moved).
// Realistic for highly-volatile pairs / news events.
// ---------------------------------------------------------------------------
static void BM_BinanceAdapter_UpdatePriceMove(benchmark::State& state) {
    BinanceBookAdapter<20> adapter;
    TickerHolder t{
        .symbol     = "BTCUSDT",
        .bid_price  = "87245.30",
        .bid_qty    = "1.5",
        .ask_price  = "87245.40",
        .ask_qty    = "2.3",
        .update_id  = 1,
    };
    (void)adapter.update_from_ticker(t.view());

    // Pre-stage a small set of price strings so we can rotate through
    // them without paying string-construction cost per iteration.
    constexpr int kPrices = 4;
    const std::string bids[kPrices] = {
        "87245.30", "87245.31", "87245.32", "87245.33"};
    const std::string asks[kPrices] = {
        "87245.40", "87245.41", "87245.42", "87245.43"};

    int idx = 0;
    int64_t i = 0;
    for (auto _ : state) {
        t.bid_price = bids[idx];
        t.ask_price = asks[idx];
        t.update_id = ++i;
        idx = (idx + 1) % kPrices;
        bool ok = adapter.update_from_ticker(t.view());
        benchmark::DoNotOptimize(ok);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BinanceAdapter_UpdatePriceMove);

// Reject-path bench is intentionally omitted: a single non-numeric
// field triggers a synchronous spdlog WARN call, which dominates the
// per-call cost (~1 µs of stderr formatting per iteration on Linux).
// That measurement says more about the logger than about the adapter.
// If a future PR adds a reject-counter / metric without log spam, a
// genuine reject-path bench becomes worthwhile.

BENCHMARK_MAIN();
