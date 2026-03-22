#include <benchmark/benchmark.h>

#include "eph/utils/time.hpp"

static void BM_TSCNow(benchmark::State& state) {
    for (auto _ : state) {
        uint64_t tsc = eph::utils::TSC::now();
        benchmark::DoNotOptimize(tsc);
    }
}
BENCHMARK(BM_TSCNow);
