/// @file bench_hugepage.cpp
/// Benchmarks for HugePage allocation — compare hugepage vs standard allocation.
/// Measures allocation+deallocation cycle for various sizes.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <benchmark/benchmark.h>

#include "eph/utils/hugepage.hpp"

using namespace eph::utils;

// ---------------------------------------------------------------------------
// HugePage::make vs std::make_unique — construction + destruction
// ---------------------------------------------------------------------------

static void BM_HugePage_Make_4KB(benchmark::State& state) {
    for (auto _ : state) {
        auto p = HugePage::make<std::array<char, 4096>>();
        benchmark::DoNotOptimize(p.get());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HugePage_Make_4KB);

static void BM_StdUnique_Make_4KB(benchmark::State& state) {
    for (auto _ : state) {
        auto p = std::make_unique<std::array<char, 4096>>();
        benchmark::DoNotOptimize(p.get());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StdUnique_Make_4KB);

static void BM_HugePage_Make_1MB(benchmark::State& state) {
    for (auto _ : state) {
        auto p = HugePage::make<std::array<char, 1024 * 1024>>();
        benchmark::DoNotOptimize(p.get());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HugePage_Make_1MB);

static void BM_StdUnique_Make_1MB(benchmark::State& state) {
    for (auto _ : state) {
        auto p = std::make_unique<std::array<char, 1024 * 1024>>();
        benchmark::DoNotOptimize(p.get());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StdUnique_Make_1MB);

// ---------------------------------------------------------------------------
// Raw allocate / deallocate cycle
// ---------------------------------------------------------------------------

static void BM_HugePage_Allocate_Deallocate_64KB(benchmark::State& state) {
    constexpr size_t kSize = 64 * 1024;
    for (auto _ : state) {
        bool is_hp = false;
        size_t alloc_size = 0;
        void* ptr = HugePage::allocate(kSize, 64, is_hp, alloc_size);
        benchmark::DoNotOptimize(ptr);
        HugePage::deallocate(ptr, alloc_size, is_hp);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HugePage_Allocate_Deallocate_64KB);

// ---------------------------------------------------------------------------
// Memory access pattern — measure TLB benefit on large working sets
// ---------------------------------------------------------------------------

static void BM_HugePage_SequentialAccess_4MB(benchmark::State& state) {
    constexpr size_t kSize = 4 * 1024 * 1024;
    auto buf = HugePage::make<std::array<char, kSize>>();
    for (auto _ : state) {
        // Sequential write to exercise TLB
        std::memset(buf.get(), 0x42, kSize);
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * kSize);
}
BENCHMARK(BM_HugePage_SequentialAccess_4MB);

static void BM_StdUnique_SequentialAccess_4MB(benchmark::State& state) {
    constexpr size_t kSize = 4 * 1024 * 1024;
    auto buf = std::make_unique<std::array<char, kSize>>();
    for (auto _ : state) {
        std::memset(buf.get(), 0x42, kSize);
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * kSize);
}
BENCHMARK(BM_StdUnique_SequentialAccess_4MB);

BENCHMARK_MAIN();
