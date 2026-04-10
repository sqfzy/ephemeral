/// @file bench_matrix.hpp
/// Benchmark matrix registration macro for parametric container benchmarks.
#pragma once

#include <benchmark/benchmark.h>

#define REGISTER_MATRIX(FUNC, ...)                                            \
    BENCHMARK_TEMPLATE(FUNC, 8, 1)->Name(#FUNC "/P:8/B:1") __VA_ARGS__;       \
    BENCHMARK_TEMPLATE(FUNC, 8, 8)->Name(#FUNC "/P:8/B:8") __VA_ARGS__;       \
    BENCHMARK_TEMPLATE(FUNC, 8, 64)->Name(#FUNC "/P:8/B:64") __VA_ARGS__;     \
    BENCHMARK_TEMPLATE(FUNC, 8, 512)->Name(#FUNC "/P:8/B:512") __VA_ARGS__;   \
    BENCHMARK_TEMPLATE(FUNC, 64, 1)->Name(#FUNC "/P:64/B:1") __VA_ARGS__;     \
    BENCHMARK_TEMPLATE(FUNC, 64, 8)->Name(#FUNC "/P:64/B:8") __VA_ARGS__;     \
    BENCHMARK_TEMPLATE(FUNC, 64, 64)->Name(#FUNC "/P:64/B:64") __VA_ARGS__;   \
    BENCHMARK_TEMPLATE(FUNC, 64, 512)->Name(#FUNC "/P:64/B:512") __VA_ARGS__; \
    BENCHMARK_TEMPLATE(FUNC, 512, 1)->Name(#FUNC "/P:512/B:1") __VA_ARGS__;   \
    BENCHMARK_TEMPLATE(FUNC, 512, 8)->Name(#FUNC "/P:512/B:8") __VA_ARGS__;   \
    BENCHMARK_TEMPLATE(FUNC, 512, 64)->Name(#FUNC "/P:512/B:64") __VA_ARGS__; \
    BENCHMARK_TEMPLATE(FUNC, 512, 512)->Name(#FUNC "/P:512/B:512") __VA_ARGS__
