/// @file bench_json_adapters.cpp
/// Microbenchmarks for the Bybit and OKX JSON adapters.
///
/// Coverage gap (batch 11): only the Binance bookTicker had a microbench
/// before this file; the Bybit and OKX adapters — used by every
/// strategy that consumes those venues — were uncovered. The shape of
/// the work is similar (parse outer envelope → re-parse nested data
/// object → extract typed fields), but each venue's payload format
/// differs enough that per-venue numbers are useful.
///
/// Why these particular cases:
/// - **BybitPushExtract**: outer-envelope parse for the routing fields
///   (topic / type / data_raw). Hot path on every WS message before any
///   per-channel handler decides what to do.
/// - **BybitBookTickerExtract**: full parse path: outer envelope + inner
///   data re-parse + 6 string field extractions. Realistic shape of the
///   "tickers" channel handler.
/// - **OkxPushExtract**: same shape as Bybit but the inner re-parse
///   targets the "arg" object (channel / instId routing).
/// - **JsonParseLargeRichPayload**: a 1.5 KiB OKX-style books5 payload
///   to gauge the JSON parser's per-byte throughput on the upper end of
///   typical WS messages.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <benchmark/benchmark.h>
#include <spdlog/spdlog.h>

#include "eph/json/adapters/bybit.hpp"
#include "eph/json/adapters/okx.hpp"
#include "eph/json/parser.hpp"

namespace {

// Realistic Bybit V5 tickers push (linear/spot bookTicker shape).
constexpr std::string_view kBybitTickers =
    R"({"topic":"tickers.BTCUSDT","type":"snapshot",)"
    R"("data":{"symbol":"BTCUSDT","bid1Price":"87000.0",)"
    R"("bid1Size":"1.2","ask1Price":"87001.0","ask1Size":"0.5",)"
    R"("lastPrice":"87000.5","ts":"1711612345678"},"ts":1711612345679})";

// Realistic OKX V5 BBO-tbt push.
constexpr std::string_view kOkxBbo =
    R"({"arg":{"channel":"bbo-tbt","instId":"BTC-USDT"},)"
    R"("data":[{"asks":[["87001.0","0.5","0","0"]],)"
    R"("bids":[["87000.0","1.2","0","0"]],)"
    R"("ts":"1711612345678","seqId":12345}]})";

// Larger payload — OKX books5 5-level depth shape, ~1.5 KiB after expansion.
constexpr std::string_view kOkxBooks5 =
    R"({"arg":{"channel":"books5","instId":"BTC-USDT"},)"
    R"("data":[{"asks":[)"
    R"(["87001.0","0.5","0","2"],)"
    R"(["87002.0","1.2","0","3"],)"
    R"(["87003.5","0.8","0","1"],)"
    R"(["87004.0","2.1","0","5"],)"
    R"(["87005.0","0.3","0","2"]],)"
    R"("bids":[)"
    R"(["87000.0","1.2","0","4"],)"
    R"(["86999.5","0.5","0","1"],)"
    R"(["86998.0","2.0","0","6"],)"
    R"(["86997.0","1.5","0","3"],)"
    R"(["86996.5","0.7","0","2"]],)"
    R"("ts":"1711612345678","seqId":98765,"checksum":-1234567890,)"
    R"("instId":"BTC-USDT"}]})";

} // namespace

// ---------------------------------------------------------------------------
// Bybit
// ---------------------------------------------------------------------------

static void BM_BybitJsonParse(benchmark::State& state) {
    auto data = reinterpret_cast<const uint8_t*>(kBybitTickers.data());
    auto len  = kBybitTickers.size();
    for (auto _ : state) {
        auto r = eph::json::parse(data, len);
        benchmark::DoNotOptimize(r);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(len));
}
BENCHMARK(BM_BybitJsonParse);

static void BM_BybitPushExtract(benchmark::State& state) {
    auto data = reinterpret_cast<const uint8_t*>(kBybitTickers.data());
    auto len  = kBybitTickers.size();
    for (auto _ : state) {
        auto r = eph::json::parse(data, len);
        if (r) {
            auto push = eph::json::bybit::BybitPushMessage::from(*r);
            benchmark::DoNotOptimize(push);
        }
    }
}
BENCHMARK(BM_BybitPushExtract);

static void BM_BybitBookTickerExtract(benchmark::State& state) {
    auto data = reinterpret_cast<const uint8_t*>(kBybitTickers.data());
    auto len  = kBybitTickers.size();
    for (auto _ : state) {
        auto r = eph::json::parse(data, len);
        if (r) {
            auto t = eph::json::bybit::BybitBookTicker::from(*r);
            benchmark::DoNotOptimize(t);
        }
    }
}
BENCHMARK(BM_BybitBookTickerExtract);

// ---------------------------------------------------------------------------
// OKX
// ---------------------------------------------------------------------------

static void BM_OkxJsonParse(benchmark::State& state) {
    auto data = reinterpret_cast<const uint8_t*>(kOkxBbo.data());
    auto len  = kOkxBbo.size();
    for (auto _ : state) {
        auto r = eph::json::parse(data, len);
        benchmark::DoNotOptimize(r);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(len));
}
BENCHMARK(BM_OkxJsonParse);

static void BM_OkxPushExtract(benchmark::State& state) {
    auto data = reinterpret_cast<const uint8_t*>(kOkxBbo.data());
    auto len  = kOkxBbo.size();
    for (auto _ : state) {
        auto r = eph::json::parse(data, len);
        if (r) {
            auto push = eph::json::okx::OkxPushMessage::from(*r);
            benchmark::DoNotOptimize(push);
        }
    }
}
BENCHMARK(BM_OkxPushExtract);

// ---------------------------------------------------------------------------
// Larger payload — gauges per-byte parser throughput
// ---------------------------------------------------------------------------

static void BM_OkxBooks5Parse(benchmark::State& state) {
    auto data = reinterpret_cast<const uint8_t*>(kOkxBooks5.data());
    auto len  = kOkxBooks5.size();
    for (auto _ : state) {
        auto r = eph::json::parse(data, len);
        benchmark::DoNotOptimize(r);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(len));
}
BENCHMARK(BM_OkxBooks5Parse);

int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::off);
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
