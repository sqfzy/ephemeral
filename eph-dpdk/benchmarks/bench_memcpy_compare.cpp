/// @file bench_memcpy_compare.cpp
/// Compare rte_memcpy (DPDK) vs std::memcpy (glibc) across typical HFT payload sizes.
///
/// HFT context: market data frames are typically small (16-512 bytes for book
/// updates, ping/pong frames are ~12 bytes). TLS record bodies top out at 16KB.
/// Test sizes cover: TCP header (20B), small WS frame (64B), typical book
/// update (256B), ITCH message max (52B), large book snapshot (4KB), TLS
/// record max (16KB).

#include <cstdint>
#include <cstring>
#include <vector>

#include <benchmark/benchmark.h>

// rte_memcpy requires __SSE2__ for x86 fallback, or AVX2/AVX-512 when enabled.
// On this toolchain we include rte_config first.
#include <rte_config.h>
#include <rte_memcpy.h>

namespace {

// Aligned buffers to give rte_memcpy the best case. Both src and dst are
// cache-line aligned (64B) to avoid mixing alignment effects into the
// comparison.
alignas(64) uint8_t g_src[16384];
alignas(64) uint8_t g_dst[16384];

// Initialize once at startup with deterministic non-zero data.
struct Init {
    Init() {
        for (size_t i = 0; i < sizeof(g_src); ++i) {
            g_src[i] = static_cast<uint8_t>(i & 0xFF);
        }
    }
};
static Init g_init;

} // namespace

// ---------------------------------------------------------------------------
// std::memcpy (glibc) baseline
// ---------------------------------------------------------------------------

static void BM_StdMemcpy(benchmark::State& state) {
    const size_t n = static_cast<size_t>(state.range(0));
    for (auto _ : state) {
        std::memcpy(g_dst, g_src, n);
        benchmark::DoNotOptimize(g_dst);
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * n);
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StdMemcpy)->Arg(16)->Arg(20)->Arg(52)->Arg(64)->Arg(128)
    ->Arg(256)->Arg(512)->Arg(1024)->Arg(4096)->Arg(16384);

// ---------------------------------------------------------------------------
// rte_memcpy (DPDK) comparison
// ---------------------------------------------------------------------------

static void BM_RteMemcpy(benchmark::State& state) {
    const size_t n = static_cast<size_t>(state.range(0));
    for (auto _ : state) {
        rte_memcpy(g_dst, g_src, n);
        benchmark::DoNotOptimize(g_dst);
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * n);
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RteMemcpy)->Arg(16)->Arg(20)->Arg(52)->Arg(64)->Arg(128)
    ->Arg(256)->Arg(512)->Arg(1024)->Arg(4096)->Arg(16384);

// ---------------------------------------------------------------------------
// Unaligned source — tests the real-world case where network packets
// are not aligned to cache line boundaries.
// ---------------------------------------------------------------------------

static void BM_StdMemcpy_Unaligned(benchmark::State& state) {
    const size_t n = static_cast<size_t>(state.range(0));
    // Offset by 3 bytes to force unaligned access
    uint8_t* src = g_src + 3;
    uint8_t* dst = g_dst + 3;
    for (auto _ : state) {
        std::memcpy(dst, src, n);
        benchmark::DoNotOptimize(dst);
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * n);
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StdMemcpy_Unaligned)->Arg(64)->Arg(512)->Arg(4096);

static void BM_RteMemcpy_Unaligned(benchmark::State& state) {
    const size_t n = static_cast<size_t>(state.range(0));
    uint8_t* src = g_src + 3;
    uint8_t* dst = g_dst + 3;
    for (auto _ : state) {
        rte_memcpy(dst, src, n);
        benchmark::DoNotOptimize(dst);
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * n);
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RteMemcpy_Unaligned)->Arg(64)->Arg(512)->Arg(4096);

BENCHMARK_MAIN();
