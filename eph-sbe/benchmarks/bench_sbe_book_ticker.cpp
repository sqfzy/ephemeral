/// @file bench_sbe_book_ticker.cpp
/// Binance spot SBE BookTickerResponse decode throughput.
///
/// Measures the full zero-copy decode path: parse() the SBE header, then
/// for_each_ticker() walk the repeating group decoding every field
/// (mantissa×10^exp prices/quantities + varString8 symbol). Depends only on
/// eph-sbe — no networking, no DPDK.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "eph/sbe.hpp"

using namespace eph::sbe;

namespace {

// Append helpers — build a wire BookTickerResponse (spot_3_2.xml id=212).
void put_le16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
void put_le_i64(std::vector<uint8_t>& b, int64_t v) {
    uint64_t u;
    std::memcpy(&u, &v, 8);
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>((u >> (8 * i)) & 0xFF));
}

/// Build a BookTickerResponse carrying `n` tickers (symbol "BTCUSDT").
std::vector<uint8_t> make_message(uint16_t n) {
    std::vector<uint8_t> b;
    put_le16(b, 0);    // root blockLength
    put_le16(b, 212);  // templateId
    put_le16(b, 3);    // schemaId
    put_le16(b, 2);    // version
    put_le16(b, 34);   // group blockLength
    put_le16(b, n);    // numInGroup
    for (uint16_t i = 0; i < n; ++i) {
        b.push_back(static_cast<uint8_t>(-2));  // priceExponent
        b.push_back(static_cast<uint8_t>(-8));  // qtyExponent
        put_le_i64(b, 6543210 + i);             // bidPrice mantissa
        put_le_i64(b, 1234000);                 // bidQty
        put_le_i64(b, 6543220 + i);             // askPrice mantissa
        put_le_i64(b, 150000000);               // askQty
        const std::string sym = "BTCUSDT";
        b.push_back(static_cast<uint8_t>(sym.size()));
        b.insert(b.end(), sym.begin(), sym.end());
    }
    return b;
}

void decode_one(const std::vector<uint8_t>& buf) {
    auto v = parse(buf.data(), buf.size());
    if (!v) return;
    namespace bt = binance::book_ticker;
    (void)binance::for_each_ticker(*v, [](const uint8_t* t) {
        benchmark::DoNotOptimize(bt::bid_price(t));
        benchmark::DoNotOptimize(bt::bid_qty(t));
        benchmark::DoNotOptimize(bt::ask_price(t));
        benchmark::DoNotOptimize(bt::ask_qty(t));
        benchmark::DoNotOptimize(bt::symbol(t));
    });
}

} // namespace

static void BM_decode_single_ticker(benchmark::State& state) {
    const auto buf = make_message(1);
    for (auto _ : state) decode_one(buf);
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_decode_single_ticker);

static void BM_decode_multi_ticker(benchmark::State& state) {
    const auto n = static_cast<uint16_t>(state.range(0));
    const auto buf = make_message(n);
    for (auto _ : state) decode_one(buf);
    state.SetItemsProcessed(state.iterations() * n);  // per-ticker rate
}
BENCHMARK(BM_decode_multi_ticker)->Arg(8)->Arg(64)->Arg(512);

BENCHMARK_MAIN();
