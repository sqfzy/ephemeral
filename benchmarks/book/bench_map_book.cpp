/// @file bench_map_book.cpp
/// Benchmarks for MapBook — counterpart to bench_array_book for comparison.
/// MapBook uses std::map for unlimited depth; trades cache locality for flexibility.

#include <cstdint>
#include <string_view>

#include <benchmark/benchmark.h>

#include "eph/book/map_book.hpp"
#include "eph/json/adapters/binance.hpp"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static double sv_to_double(std::string_view sv) noexcept {
    double val = 0.0;
    std::from_chars(sv.data(), sv.data() + sv.size(), val);
    return val;
}

static constexpr std::string_view kBookTicker =
    R"({"e":"bookTicker","u":4736462646,"s":"BTCUSDT","b":"87245.30","B":"0.500","a":"87245.40","A":"1.200","T":1711612345678,"E":1711612345679})";

// ---------------------------------------------------------------------------
// BM_MapBookUpdate — insert/modify a single price level
// ---------------------------------------------------------------------------

static void BM_MapBookUpdate(benchmark::State& state) {
    eph::book::MapBook book;
    // Pre-populate levels
    for (int i = 0; i < 10; ++i) {
        book.update_bid(87240.0 + i, 0.5 + i * 0.1);
        book.update_ask(87250.0 + i, 0.5 + i * 0.1);
    }

    double price = 87245.0;
    double qty   = 1.0;
    for (auto _ : state) {
        book.update_bid(price, qty);
        benchmark::DoNotOptimize(book);
        qty += 0.001;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MapBookUpdate);

// ---------------------------------------------------------------------------
// BM_MapBookBBO — query best bid/ask
// ---------------------------------------------------------------------------

static void BM_MapBookBBO(benchmark::State& state) {
    eph::book::MapBook book;
    for (int i = 0; i < 10; ++i) {
        book.update_bid(87240.0 + i, 0.5 + i * 0.1);
        book.update_ask(87250.0 + i, 0.5 + i * 0.1);
    }

    for (auto _ : state) {
        auto bid = book.best_bid();
        auto ask = book.best_ask();
        benchmark::DoNotOptimize(bid);
        benchmark::DoNotOptimize(ask);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MapBookBBO);

// ---------------------------------------------------------------------------
// BM_MapBookMidPrice — compute mid price
// ---------------------------------------------------------------------------

static void BM_MapBookMidPrice(benchmark::State& state) {
    eph::book::MapBook book;
    for (int i = 0; i < 10; ++i) {
        book.update_bid(87240.0 + i, 0.5 + i * 0.1);
        book.update_ask(87250.0 + i, 0.5 + i * 0.1);
    }

    for (auto _ : state) {
        auto mid = book.mid_price();
        benchmark::DoNotOptimize(mid);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MapBookMidPrice);

// ---------------------------------------------------------------------------
// BM_MapBookFullCycle — parse JSON → extract → update → BBO
// ---------------------------------------------------------------------------

static void BM_MapBookFullCycle(benchmark::State& state) {
    auto data = reinterpret_cast<const uint8_t*>(kBookTicker.data());
    auto len  = kBookTicker.size();

    eph::book::MapBook book;

    for (auto _ : state) {
        auto result = eph::json::parse(data, len);
        if (!result) continue;

        auto ticker = eph::json::binance::BookTicker::from(*result);
        if (!ticker) continue;

        double bid_px  = sv_to_double(ticker->bid_price);
        double bid_qty = sv_to_double(ticker->bid_qty);
        double ask_px  = sv_to_double(ticker->ask_price);
        double ask_qty = sv_to_double(ticker->ask_qty);

        book.update_bid(bid_px, bid_qty);
        book.update_ask(ask_px, ask_qty);

        auto bid = book.best_bid();
        auto ask = book.best_ask();
        benchmark::DoNotOptimize(bid);
        benchmark::DoNotOptimize(ask);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(len));
}
BENCHMARK(BM_MapBookFullCycle);

// ---------------------------------------------------------------------------
// BM_MapBookDeepUpdate — update with 100 levels (stresses map traversal)
// ---------------------------------------------------------------------------

static void BM_MapBookDeepUpdate(benchmark::State& state) {
    eph::book::MapBook book;
    // Pre-populate 100 levels per side
    for (int i = 0; i < 100; ++i) {
        book.update_bid(87000.0 + i, 0.5 + i * 0.01);
        book.update_ask(87100.0 + i, 0.5 + i * 0.01);
    }

    double price = 87050.0;
    double qty   = 1.0;
    for (auto _ : state) {
        book.update_bid(price, qty);
        benchmark::DoNotOptimize(book);
        qty += 0.001;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MapBookDeepUpdate);

// ---------------------------------------------------------------------------
// BM_MapBookIsCrossed — check for crossed book condition
// ---------------------------------------------------------------------------

static void BM_MapBookIsCrossed(benchmark::State& state) {
    eph::book::MapBook book;
    for (int i = 0; i < 10; ++i) {
        book.update_bid(87240.0 + i, 0.5 + i * 0.1);
        book.update_ask(87250.0 + i, 0.5 + i * 0.1);
    }

    for (auto _ : state) {
        auto r = book.is_crossed();
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MapBookIsCrossed);

BENCHMARK_MAIN();
