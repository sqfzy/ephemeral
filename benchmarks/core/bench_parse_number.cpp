/// @file bench_parse_number.cpp
/// Benchmarks for parse_number() and parse_int() — hot-path parsing
/// called per field in JSON/FIX adapters.

#include <cstdint>
#include <string_view>

#include <benchmark/benchmark.h>

#include "eph/core/parse_number.hpp"

using namespace eph::core;

// ---------------------------------------------------------------------------
// parse_number benchmarks
// ---------------------------------------------------------------------------

// Typical exchange price: "87245.30"
static void BM_ParseNumber_Price(benchmark::State& state) {
    constexpr std::string_view input = "87245.30";
    for (auto _ : state) {
        auto r = parse_number(input);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseNumber_Price);

// Typical exchange quantity: "0.00012500"
static void BM_ParseNumber_SmallQty(benchmark::State& state) {
    constexpr std::string_view input = "0.00012500";
    for (auto _ : state) {
        auto r = parse_number(input);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseNumber_SmallQty);

// Simple integer as double: "42"
static void BM_ParseNumber_Integer(benchmark::State& state) {
    constexpr std::string_view input = "42";
    for (auto _ : state) {
        auto r = parse_number(input);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseNumber_Integer);

// Scientific notation: "1.5e10"
static void BM_ParseNumber_Scientific(benchmark::State& state) {
    constexpr std::string_view input = "1.5e10";
    for (auto _ : state) {
        auto r = parse_number(input);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseNumber_Scientific);

// Long decimal: "123456789.123456789" (18 chars)
static void BM_ParseNumber_LongDecimal(benchmark::State& state) {
    constexpr std::string_view input = "123456789.123456789";
    for (auto _ : state) {
        auto r = parse_number(input);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseNumber_LongDecimal);

// Negative price: "-87245.30"
static void BM_ParseNumber_NegativePrice(benchmark::State& state) {
    constexpr std::string_view input = "-87245.30";
    for (auto _ : state) {
        auto r = parse_number(input);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseNumber_NegativePrice);

// Error path: malformed input
static void BM_ParseNumber_ErrorPath(benchmark::State& state) {
    constexpr std::string_view input = "not_a_number";
    for (auto _ : state) {
        auto r = parse_number(input);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseNumber_ErrorPath);

// ---------------------------------------------------------------------------
// parse_int benchmarks
// ---------------------------------------------------------------------------

// Typical sequence number: "1234567"
static void BM_ParseInt_SeqNum(benchmark::State& state) {
    constexpr std::string_view input = "1234567";
    for (auto _ : state) {
        auto r = parse_int(input);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseInt_SeqNum);

// Timestamp-like: "1711612345678" (13 digits)
static void BM_ParseInt_Timestamp(benchmark::State& state) {
    constexpr std::string_view input = "1711612345678";
    for (auto _ : state) {
        auto r = parse_int(input);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseInt_Timestamp);

// INT64_MAX boundary: "9223372036854775807" (19 digits)
static void BM_ParseInt_MaxValue(benchmark::State& state) {
    constexpr std::string_view input = "9223372036854775807";
    for (auto _ : state) {
        auto r = parse_int(input);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseInt_MaxValue);

// Small integer: "1"
static void BM_ParseInt_SingleDigit(benchmark::State& state) {
    constexpr std::string_view input = "1";
    for (auto _ : state) {
        auto r = parse_int(input);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseInt_SingleDigit);

// Negative integer: "-42"
static void BM_ParseInt_Negative(benchmark::State& state) {
    constexpr std::string_view input = "-42";
    for (auto _ : state) {
        auto r = parse_int(input);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseInt_Negative);

BENCHMARK_MAIN();
