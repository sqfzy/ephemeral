#include <benchmark/benchmark.h>
#include <charconv>
#include <cstdint>
#include <string_view>

#include "eph/book/array_book.hpp"
#include "eph/json/adapters/binance.hpp"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Parse a string_view as double via std::from_chars (no allocation).
static double sv_to_double(std::string_view sv) noexcept {
    double val = 0.0;
    std::from_chars(sv.data(), sv.data() + sv.size(), val);
    return val;
}

// Realistic Binance bookTicker JSON payload used in full-cycle benchmark.
static constexpr std::string_view kBookTicker =
    R"({"e":"bookTicker","u":4736462646,"s":"BTCUSDT","b":"87245.30","B":"0.500","a":"87245.40","A":"1.200","T":1711612345678,"E":1711612345679})";

// ---------------------------------------------------------------------------
// BM_BookUpdate — insert/modify a single price level
// ---------------------------------------------------------------------------

static void BM_BookUpdate(benchmark::State& state) {
    eph::book::ArrayBook<20> book;
    // Pre-populate a few levels so updates hit the modify path.
    for (int i = 0; i < 10; ++i) {
        book.update_bid(87240.0 + i, 0.5 + i * 0.1);
        book.update_ask(87250.0 + i, 0.5 + i * 0.1);
    }

    double price = 87245.0;
    double qty   = 1.0;
    for (auto _ : state) {
        // Alternates between modifying an existing level (hot path).
        book.update_bid(price, qty);
        benchmark::DoNotOptimize(book);
        qty += 0.001; // vary qty to prevent dead-code elimination
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BookUpdate);

// ---------------------------------------------------------------------------
// BM_BookBBO — query best bid/ask after a populated book
// ---------------------------------------------------------------------------

static void BM_BookBBO(benchmark::State& state) {
    eph::book::ArrayBook<20> book;
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
BENCHMARK(BM_BookBBO);

// ---------------------------------------------------------------------------
// BM_BookMidPrice — compute mid price from populated book
// ---------------------------------------------------------------------------

static void BM_BookMidPrice(benchmark::State& state) {
    eph::book::ArrayBook<20> book;
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
BENCHMARK(BM_BookMidPrice);

// ---------------------------------------------------------------------------
// BM_BookFullCycle — parse JSON → extract BookTicker → update book → BBO
// ---------------------------------------------------------------------------

static void BM_BookFullCycle(benchmark::State& state) {
    auto data = reinterpret_cast<const uint8_t*>(kBookTicker.data());
    auto len  = kBookTicker.size();

    eph::book::ArrayBook<20> book;

    for (auto _ : state) {
        // 1. Parse raw JSON
        auto result = eph::json::parse(data, len);
        if (!result) continue;

        // 2. Extract BookTicker fields (zero-copy)
        auto ticker = eph::json::binance::BookTicker::from(*result);
        if (!ticker) continue;

        // 3. Convert string prices/qtys to doubles and update both sides
        double bid_px  = sv_to_double(ticker->bid_price);
        double bid_qty = sv_to_double(ticker->bid_qty);
        double ask_px  = sv_to_double(ticker->ask_price);
        double ask_qty = sv_to_double(ticker->ask_qty);

        book.update_bid(bid_px, bid_qty);
        book.update_ask(ask_px, ask_qty);

        // 4. Read BBO
        auto bid = book.best_bid();
        auto ask = book.best_ask();
        benchmark::DoNotOptimize(bid);
        benchmark::DoNotOptimize(ask);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(len));
}
BENCHMARK(BM_BookFullCycle);

BENCHMARK_MAIN();
