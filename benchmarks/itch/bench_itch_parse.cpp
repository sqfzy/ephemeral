/// @file bench_itch_parse.cpp
/// ITCH 5.0 parser benchmarks — parse throughput and field accessor latency.
///
/// Depends only on eph-itch (no DPDK required).

#include <array>
#include <cstdint>
#include <cstring>
#include <random>

#include <benchmark/benchmark.h>

#include "eph/itch.hpp"

using namespace eph::itch;

// ---------------------------------------------------------------------------
// Helpers: construct valid ITCH message payloads
// ---------------------------------------------------------------------------
namespace {

// AddOrder buffer size: use 36 to safely accommodate all field accessors
// (kAddOrderSize=35 may be off-by-one; accessors read up to offset 35).
constexpr size_t kAddOrderBufSize = 36;

/// Fill a buffer with a valid AddOrder ('A') message.
void fill_add_order(uint8_t* buf) {
    std::memset(buf, 0, kAddOrderBufSize);
    buf[0] = kAddOrder;  // 'A'
    buf[1] = 0; buf[2] = 1;  // stock_locate
    buf[3] = 0; buf[4] = 0;  // tracking_number
    // timestamp (6 bytes)
    uint64_t ts = 36000000000000ULL;
    buf[5]  = static_cast<uint8_t>((ts >> 40) & 0xFF);
    buf[6]  = static_cast<uint8_t>((ts >> 32) & 0xFF);
    buf[7]  = static_cast<uint8_t>((ts >> 24) & 0xFF);
    buf[8]  = static_cast<uint8_t>((ts >> 16) & 0xFF);
    buf[9]  = static_cast<uint8_t>((ts >>  8) & 0xFF);
    buf[10] = static_cast<uint8_t>(ts & 0xFF);
    // order_ref (8 bytes)
    uint64_t ref = 12345678;
    for (size_t i = 0; i < 8; ++i)
        buf[11 + i] = static_cast<uint8_t>((ref >> (56 - 8*i)) & 0xFF);
    buf[19] = 'B';  // side
    buf[20] = 0; buf[21] = 0; buf[22] = 0; buf[23] = 100; // shares
    std::memcpy(buf + 24, "AAPL    ", 8);  // stock
    // price (4 bytes)
    uint32_t price = 1500000;
    buf[32] = static_cast<uint8_t>((price >> 24) & 0xFF);
    buf[33] = static_cast<uint8_t>((price >> 16) & 0xFF);
    buf[34] = static_cast<uint8_t>((price >>  8) & 0xFF);
    buf[35] = static_cast<uint8_t>(price & 0xFF);
}

/// Fill a buffer with a valid OrderDelete ('D') message.
/// Size = 19 bytes.
void fill_order_delete(uint8_t* buf) {
    std::memset(buf, 0, kOrderDeleteSize);
    buf[0] = kOrderDelete;  // 'D'
    buf[1] = 0; buf[2] = 1;  // stock_locate
}

/// Fill a buffer with a valid SystemEvent ('S') message.
/// Size = 12 bytes.
void fill_system_event(uint8_t* buf) {
    std::memset(buf, 0, kSystemEventSize);
    buf[0] = kSystemEvent;  // 'S'
    // SystemEvent: type(1) + locate(2) + tracking(2) + timestamp(6) = 11 bytes
    // No additional fields beyond the common header
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Parse single message
// ---------------------------------------------------------------------------

static void BM_ItchParseAddOrder(benchmark::State& state) {
    std::array<uint8_t, kAddOrderBufSize> buf{};
    fill_add_order(buf.data());

    for (auto _ : state) {
        auto result = parse(buf.data(), buf.size());
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ItchParseAddOrder);

static void BM_ItchParseOrderDelete(benchmark::State& state) {
    std::array<uint8_t, kOrderDeleteSize> buf{};
    fill_order_delete(buf.data());

    for (auto _ : state) {
        auto result = parse(buf.data(), buf.size());
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ItchParseOrderDelete);

static void BM_ItchParseSystemEvent(benchmark::State& state) {
    std::array<uint8_t, kSystemEventSize> buf{};
    fill_system_event(buf.data());

    for (auto _ : state) {
        auto result = parse(buf.data(), buf.size());
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ItchParseSystemEvent);

// ---------------------------------------------------------------------------
// Field accessor latency (read after parse)
// ---------------------------------------------------------------------------

static void BM_ItchAddOrderFieldAccess(benchmark::State& state) {
    std::array<uint8_t, kAddOrderBufSize> buf{};
    fill_add_order(buf.data());

    auto result = parse(buf.data(), buf.size());
    auto& view = *result;

    for (auto _ : state) {
        auto sl = view.stock_locate();
        auto ts = view.timestamp_ns();
        auto ref = add_order::order_ref(view.data);
        auto sd = add_order::side(view.data);
        auto sh = add_order::shares(view.data);
        auto px = add_order::price(view.data);
        benchmark::DoNotOptimize(sl);
        benchmark::DoNotOptimize(ts);
        benchmark::DoNotOptimize(ref);
        benchmark::DoNotOptimize(sd);
        benchmark::DoNotOptimize(sh);
        benchmark::DoNotOptimize(px);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ItchAddOrderFieldAccess);

// ---------------------------------------------------------------------------
// Batch parse: simulate processing a stream of messages
// ---------------------------------------------------------------------------

static void BM_ItchParseBatch(benchmark::State& state) {
    constexpr size_t kBatchSize = 1024;
    // Pre-build a batch of mixed messages (AddOrder and OrderDelete, alternating)
    std::vector<std::array<uint8_t, kAddOrderBufSize>> msgs(kBatchSize);
    for (size_t i = 0; i < kBatchSize; ++i) {
        if (i % 2 == 0) {
            fill_add_order(msgs[i].data());
        } else {
            fill_order_delete(msgs[i].data());
        }
    }

    for (auto _ : state) {
        for (size_t i = 0; i < kBatchSize; ++i) {
            auto result = parse(msgs[i].data(),
                                i % 2 == 0 ? kAddOrderBufSize : kOrderDeleteSize);
            benchmark::DoNotOptimize(result);
        }
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kBatchSize));
}
BENCHMARK(BM_ItchParseBatch);

// ---------------------------------------------------------------------------
// LengthPrefixFramer decode (ITCH framing layer)
// ---------------------------------------------------------------------------

static void BM_ItchFramerDecode(benchmark::State& state) {
    // Build a length-prefixed AddOrder message
    std::array<uint8_t, 2 + kAddOrderBufSize> framed{};
    uint16_t len = static_cast<uint16_t>(kAddOrderBufSize);
    framed[0] = static_cast<uint8_t>((len >> 8) & 0xFF);
    framed[1] = static_cast<uint8_t>(len & 0xFF);
    fill_add_order(framed.data() + 2);

    for (auto _ : state) {
        auto result = eph::net::LengthPrefixFramer::decode(framed.data(), framed.size());
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ItchFramerDecode);

BENCHMARK_MAIN();
