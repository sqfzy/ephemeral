/// @file bench_orders.cpp
/// Microbenchmarks for `eph::fix::build_new_order` /
/// `build_cancel_order` / `build_replace_order`.
///
/// Coverage gap (batch 11): `bench_fix_parse` covers the generic
/// `MessageBuilder` path but not the high-level NewOrder/Cancel/Replace
/// helpers — the **exact functions** every OMS calls per outbound
/// message. Pinning their per-call cost ensures any future regression
/// in the helpers' validation gating, defaults, or timestamp handling
/// surfaces immediately.

#include <cstddef>
#include <cstdint>

#include <benchmark/benchmark.h>
#include <spdlog/spdlog.h>

#include "eph/fix/orders.hpp"

using eph::fix::OrdType;
using eph::fix::Side;
using eph::fix::TimeInForce;
using eph::fix::build_cancel_order;
using eph::fix::build_new_order;
using eph::fix::build_replace_order;

// ---------------------------------------------------------------------------
// build_new_order: Limit + Market shapes
// ---------------------------------------------------------------------------

static void BM_BuildNewOrderLimit(benchmark::State& state) {
    uint8_t buf[1024];
    for (auto _ : state) {
        auto n = build_new_order(
            buf, sizeof(buf),
            "CLIENT01", "EXCH01",
            "client-order-001",
            "AAPL", Side::Buy, OrdType::Limit,
            /*qty=*/100.0, /*price=*/150.50,
            TimeInForce::Day,
            /*sending_time_ns=*/1733846400000000000ULL);
        benchmark::DoNotOptimize(n);
        benchmark::DoNotOptimize(buf);
    }
}
BENCHMARK(BM_BuildNewOrderLimit);

static void BM_BuildNewOrderMarket(benchmark::State& state) {
    // Market order skips the price field entirely.
    uint8_t buf[1024];
    for (auto _ : state) {
        auto n = build_new_order(
            buf, sizeof(buf),
            "CLIENT01", "EXCH01",
            "client-order-001",
            "AAPL", Side::Buy, OrdType::Market,
            /*qty=*/100.0,
            /*price=*/0.0,
            TimeInForce::IOC,
            /*sending_time_ns=*/1733846400000000000ULL);
        benchmark::DoNotOptimize(n);
        benchmark::DoNotOptimize(buf);
    }
}
BENCHMARK(BM_BuildNewOrderMarket);

// ---------------------------------------------------------------------------
// build_cancel_order — every OMS-side cancel hits this
// ---------------------------------------------------------------------------

static void BM_BuildCancelOrder(benchmark::State& state) {
    uint8_t buf[1024];
    for (auto _ : state) {
        auto n = build_cancel_order(
            buf, sizeof(buf),
            "CLIENT01", "EXCH01",
            /*cl_ord_id=*/"cancel-id-002",
            /*orig_cl_ord_id=*/"client-order-001",
            "AAPL", Side::Buy);
        benchmark::DoNotOptimize(n);
    }
}
BENCHMARK(BM_BuildCancelOrder);

// ---------------------------------------------------------------------------
// build_replace_order — Limit shape
// ---------------------------------------------------------------------------

static void BM_BuildReplaceOrderLimit(benchmark::State& state) {
    uint8_t buf[1024];
    for (auto _ : state) {
        auto n = build_replace_order(
            buf, sizeof(buf),
            "CLIENT01", "EXCH01",
            /*cl_ord_id=*/"replace-id-003",
            /*orig_cl_ord_id=*/"client-order-001",
            "AAPL", Side::Buy, OrdType::Limit,
            /*qty=*/120.0, /*price=*/150.75);
        benchmark::DoNotOptimize(n);
    }
}
BENCHMARK(BM_BuildReplaceOrderLimit);

// ---------------------------------------------------------------------------
// build_new_order — invalid qty rejection (DoS-resistance check)
// ---------------------------------------------------------------------------

static void BM_BuildNewOrderRejectInvalidQty(benchmark::State& state) {
    uint8_t buf[1024];
    for (auto _ : state) {
        auto n = build_new_order(
            buf, sizeof(buf),
            "CLIENT01", "EXCH01",
            "client-order-001",
            "AAPL", Side::Buy, OrdType::Limit,
            /*qty=*/-1.0,  // invalid
            /*price=*/150.50);
        benchmark::DoNotOptimize(n);
    }
}
BENCHMARK(BM_BuildNewOrderRejectInvalidQty);

int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::off);
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
