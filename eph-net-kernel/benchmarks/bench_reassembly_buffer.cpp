/// @file bench_reassembly_buffer.cpp
/// Microbenchmarks for `eph::net::kernel::detail::ReassemblyBuffer`.
///
/// Coverage gap (loop-cleanup R173): ReassemblyBuffer sits between every
/// `KernelTcpStream::poll_once_` recv and the codec decode path; every
/// streaming frame we receive passes through it. Hot ops:
///
///   - `commit_write(n)`: clamp + tail advance, called once per recv
///   - `consume(n)`: head advance + auto-reset, called once per emitted
///                   frame (inside the codec decode loop)
///   - `compact()`: memmove of the unread region, called by the Stream
///                  before every recv if the buffer has any drift
///
/// `commit_write` carries an explicit clamp branch to defend against a
/// caller miscomputing the recv length; `compact` calls memmove on the
/// unread region. A regression in either path (e.g. an extra branch, a
/// switch from memmove to a hand-rolled byte copy, or removal of the
/// auto-reset short-circuit in consume) would silently widen recv-side
/// latency on every stream. None had bench coverage.

#include <cstddef>
#include <cstdint>
#include <vector>

#include <benchmark/benchmark.h>

#include "eph/net/kernel/detail/reassembly_buffer.hpp"

using eph::net::kernel::detail::ReassemblyBuffer;

// ---------------------------------------------------------------------------
// commit_write: tail advance + capacity clamp. Hot path is `n <=
// writable_capacity()` so the clamp branch is predictable.
// ---------------------------------------------------------------------------

static void BM_ReasmCommitWrite(benchmark::State& state) {
    ReassemblyBuffer buf{1 << 20};  // 1 MiB headroom so we don't hit the cap.
    for (auto _ : state) {
        buf.commit_write(64);
        benchmark::DoNotOptimize(buf.read_ptr());
        // Drain so we don't fill the buffer over the bench run.
        buf.consume(64);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_ReasmCommitWrite);

// ---------------------------------------------------------------------------
// consume: head advance with auto-reset to head_=tail_=0 when fully drained.
// The auto-reset is the steady-state path: TCP RX usually drains exactly
// one frame per receive cycle.
// ---------------------------------------------------------------------------

static void BM_ReasmConsumeFullDrain(benchmark::State& state) {
    ReassemblyBuffer buf{1 << 20};
    for (auto _ : state) {
        buf.commit_write(64);
        buf.consume(64);  // Hits the head==tail auto-reset path.
        benchmark::DoNotOptimize(buf.readable());
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_ReasmConsumeFullDrain);

/// Partial consume — head advances but does not equal tail, so the
/// auto-reset short-circuit is skipped. Less common on stream codecs that
/// emit one frame per call, but the WS / mold64 paths can leave a
/// partial frame at the end of a recv batch.
static void BM_ReasmConsumePartial(benchmark::State& state) {
    ReassemblyBuffer buf{1 << 20};
    buf.commit_write(128);  // Pre-fill so the buffer has unread tail.
    for (auto _ : state) {
        buf.consume(1);  // Always leaves bytes behind so head_ != tail_.
        benchmark::DoNotOptimize(buf.readable());
        if (buf.readable() < 8) {
            // Refill periodically so we don't deplete the buffer.
            buf.commit_write(128);
        }
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_ReasmConsumePartial);

// ---------------------------------------------------------------------------
// compact: memmove the unread region back to offset 0.
// ---------------------------------------------------------------------------

/// Cheapest compact: head==0 short-circuit. Pin so a regression that
/// removes the early-out cannot creep up the per-recv cost.
static void BM_ReasmCompactNoOp(benchmark::State& state) {
    ReassemblyBuffer buf{1 << 20};  // head_ stays at 0.
    for (auto _ : state) {
        buf.compact();
        benchmark::DoNotOptimize(buf.readable());
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_ReasmCompactNoOp);

/// Realistic compact: 64 unread bytes drifted to the middle of a 1MB
/// buffer. The dominant cost is the memmove; pin it so a regression that
/// switches to a manual byte copy is visible.
static void BM_ReasmCompactRealistic(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        ReassemblyBuffer buf{1 << 20};
        // Simulate a half-full buffer with the unread window pushed
        // half a megabyte deep.
        buf.commit_write(1 << 19);   // tail = 512K
        buf.consume((1 << 19) - 64); // head = 512K - 64, readable = 64
        state.ResumeTiming();

        buf.compact();
    }
}
BENCHMARK(BM_ReasmCompactRealistic);

BENCHMARK_MAIN();
