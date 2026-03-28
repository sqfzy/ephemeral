#include <benchmark/benchmark.h>
#include "eph/json/parser.hpp"
#include "eph/json/adapters/binance.hpp"

static constexpr std::string_view kBookTicker =
    R"({"e":"bookTicker","u":4736462646,"s":"BTCUSDT","b":"87245.30","B":"0.500","a":"87245.40","A":"1.200","T":1711612345678,"E":1711612345679})";

static void BM_JsonParse(benchmark::State& state) {
    auto data = reinterpret_cast<const uint8_t*>(kBookTicker.data());
    auto len = kBookTicker.size();
    for (auto _ : state) {
        auto result = eph::json::parse(data, len);
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * len);
}
BENCHMARK(BM_JsonParse);

static void BM_JsonParseAndExtract(benchmark::State& state) {
    auto data = reinterpret_cast<const uint8_t*>(kBookTicker.data());
    auto len = kBookTicker.size();
    for (auto _ : state) {
        auto result = eph::json::parse(data, len);
        if (result) {
            auto ticker = eph::json::binance::BookTicker::from(*result);
            benchmark::DoNotOptimize(ticker);
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_JsonParseAndExtract);

static void BM_SymbolHash(benchmark::State& state) {
    auto data = reinterpret_cast<const uint8_t*>(kBookTicker.data());
    auto len = kBookTicker.size();
    for (auto _ : state) {
        auto h = eph::json::binance::symbol_hash(data, len);
        benchmark::DoNotOptimize(h);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SymbolHash);

BENCHMARK_MAIN();
