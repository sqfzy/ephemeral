/// @file bench_signals.cpp
/// Microbenchmarks for `eph::book` market-microstructure signals.
///
/// Coverage gap (batch 11): the book signal calculators (order_imbalance,
/// weighted_mid, microprice, spread_bps, vwap, depth_ratio) are pure
/// functions called many times per quote on signal-driven strategies. No
/// bench existed before this file. Pinning the costs ensures any future
/// "let's also iterate the depth for VWAP" tweak doesn't quietly slow
/// the per-quote signal pass into the microsecond range.
///
/// Why these specific cases:
/// - **OrderImbalance / DepthRatio**: pure scalar arithmetic on
///   pre-computed totals from the book. Should be ~1 ns each.
/// - **WeightedMid / Spread**: best-of-book lookups + small arithmetic;
///   the lookup goes through ArrayBook::best_bid/best_ask which is two
///   bounds-checked reads.
/// - **Vwap**: scales with depth — bench at 5 / 20 levels covering the
///   typical "top of book" vs "deep ladder" use cases.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include <benchmark/benchmark.h>

#include "eph/book/array_book.hpp"
#include "eph/book/map_book.hpp"
#include "eph/book/signals.hpp"

using eph::book::ArrayBook;
using eph::book::MapBook;
using eph::book::depth_ratio;
using eph::book::microprice;
using eph::book::order_imbalance;
using eph::book::spread_bps;
using eph::book::vwap;
using eph::book::weighted_mid;

namespace {

/// Build an ArrayBook with `levels` populated bid + ask sides. Realistic
/// depth ladder shape: prices step away from BBO by 0.05 each side.
template <std::size_t N>
[[nodiscard]] ArrayBook<N> make_array(std::size_t levels) {
    ArrayBook<N> book;
    for (std::size_t i = 0; i < levels; ++i) {
        book.update_bid(100.00 - 0.05 * static_cast<double>(i),
                        10.0 + static_cast<double>(i));
        book.update_ask(100.50 + 0.05 * static_cast<double>(i),
                        10.0 + static_cast<double>(i));
    }
    return book;
}

} // namespace

static void BM_SignalOrderImbalance(benchmark::State& state) {
    auto book = make_array<20>(20);
    for (auto _ : state) {
        auto v = order_imbalance(book);
        benchmark::DoNotOptimize(v);
    }
}
BENCHMARK(BM_SignalOrderImbalance);

static void BM_SignalDepthRatio(benchmark::State& state) {
    auto book = make_array<20>(20);
    for (auto _ : state) {
        auto v = depth_ratio(book);
        benchmark::DoNotOptimize(v);
    }
}
BENCHMARK(BM_SignalDepthRatio);

static void BM_SignalWeightedMid(benchmark::State& state) {
    auto book = make_array<20>(20);
    for (auto _ : state) {
        auto v = weighted_mid(book);
        benchmark::DoNotOptimize(v);
    }
}
BENCHMARK(BM_SignalWeightedMid);

static void BM_SignalMicroprice(benchmark::State& state) {
    // Microprice has the same formula as weighted_mid but is exposed as a
    // distinct call site; bench separately to confirm no extra cost is
    // hidden in the alias.
    auto book = make_array<20>(20);
    for (auto _ : state) {
        auto v = microprice(book);
        benchmark::DoNotOptimize(v);
    }
}
BENCHMARK(BM_SignalMicroprice);

static void BM_SignalSpreadBps(benchmark::State& state) {
    auto book = make_array<20>(20);
    for (auto _ : state) {
        auto v = spread_bps(book);
        benchmark::DoNotOptimize(v);
    }
}
BENCHMARK(BM_SignalSpreadBps);

// ---------------------------------------------------------------------------
// VWAP scales with depth — bench top-of-book vs deeper ladder.
// ---------------------------------------------------------------------------

static void BM_SignalVwap(benchmark::State& state) {
    const std::size_t depth = static_cast<std::size_t>(state.range(0));
    auto book = make_array<32>(depth);
    auto bids = book.bids();
    for (auto _ : state) {
        auto v = vwap(bids);
        benchmark::DoNotOptimize(v);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(depth));
}
BENCHMARK(BM_SignalVwap)->Arg(1)->Arg(5)->Arg(20);

// ---------------------------------------------------------------------------
// VWAP NaN-skip path: defense-in-depth branch in `vwap()` skips levels with
// non-finite price or qty. ArrayBook/MapBook normally reject these at
// insert, but vwap accepts any span — a caller building a span by hand
// hits the skip branch. No bench exercised it, so a regression in the
// `[[unlikely]]` hint or in moving the isfinite check below the multiply
// would silently raise per-quote VWAP cost.
//
// We construct a 20-level span where roughly every other level is NaN
// (sparse NaN pattern matching what a partially-flushed external feed
// might present) and confirm the skip branch stays cheap.
// ---------------------------------------------------------------------------

static void BM_SignalVwapSparseNaN(benchmark::State& state) {
    constexpr std::size_t depth = 20;
    std::array<eph::book::PriceLevel, depth> levels{};
    const double nan_v = std::numeric_limits<double>::quiet_NaN();
    for (std::size_t i = 0; i < depth; ++i) {
        if ((i & 1u) != 0u) {
            // Sparse NaN — every odd level. Mix of NaN price and NaN qty so
            // both checks in the `isfinite(price) || isfinite(qty)` test
            // get exercised across iterations.
            levels[i] = (i & 2u)
                ? eph::book::PriceLevel{nan_v, 10.0 + static_cast<double>(i)}
                : eph::book::PriceLevel{100.0 - 0.05 * static_cast<double>(i), nan_v};
        } else {
            levels[i] = eph::book::PriceLevel{
                100.0 - 0.05 * static_cast<double>(i),
                10.0 + static_cast<double>(i)};
        }
    }
    std::span<const eph::book::PriceLevel> view{levels};
    for (auto _ : state) {
        auto v = vwap(view);
        benchmark::DoNotOptimize(v);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(depth));
}
BENCHMARK(BM_SignalVwapSparseNaN);

// ---------------------------------------------------------------------------
// Same signals over MapBook — confirms the duck-typed interface doesn't
// hide a copy or virtual-dispatch surprise on the alternate book type.
// ---------------------------------------------------------------------------

static void BM_SignalMapBookOrderImbalance(benchmark::State& state) {
    MapBook book;
    for (std::size_t i = 0; i < 20; ++i) {
        book.update_bid(100.00 - 0.05 * static_cast<double>(i),
                        10.0 + static_cast<double>(i));
        book.update_ask(100.50 + 0.05 * static_cast<double>(i),
                        10.0 + static_cast<double>(i));
    }
    for (auto _ : state) {
        auto v = order_imbalance(book);
        benchmark::DoNotOptimize(v);
    }
}
BENCHMARK(BM_SignalMapBookOrderImbalance);

BENCHMARK_MAIN();
