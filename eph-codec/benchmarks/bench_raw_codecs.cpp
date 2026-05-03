/// @file bench_raw_codecs.cpp
/// Microbenchmarks for `eph::codec::RawStreamCodec` and `RawDatagramCodec`
/// — the two identity codecs used as the no-framing pass-through paths.
///
/// Coverage gap (batch 11): both codecs are on the hot path of every UDP
/// market-data feed and "framing handled at app layer" TCP flow, but
/// neither had a microbench. The RawStream encode is a literal `memcpy +
/// return` and so is *expected* to compile to the platform memcpy; the
/// bench pins that expectation against future refactors.
///
/// Why these particular cases:
/// - **RawStreamEncode/Decode at 16/256/4096 B**: spans the small-control,
///   typical-tick, and bulk-burst sizes. Decode at non-zero length is
///   essentially a `trim_front(n)` and a span construction — a regression
///   would mean the codec started doing per-byte work.
/// - **RawDatagramDecode/Encode**: same shape but enforces the "1 datagram
///   = 1 frame" contract via a `std::function` sink. The sink is a known
///   hot-path cost (function-pointer-dispatch); benching it ensures it
///   stays at one indirect call rather than a virtual+heap allocation.
/// - **RawStreamDecodeEmpty**: the "need-more-data" Ok(None) path that a
///   poller hits any time the socket reports readable but the read returns
///   0 bytes. Confirms the empty-view fast-return doesn't regress.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include <benchmark/benchmark.h>
#include <spdlog/spdlog.h>

#include "eph/codec/detail/span_packet_view.hpp"
#include "eph/codec/raw_datagram_codec.hpp"
#include "eph/codec/raw_stream_codec.hpp"
#include "eph/core/codec.hpp"

using eph::codec::RawDatagramCodec;
using eph::codec::RawStreamCodec;
using eph::codec::SpanPacketView;
using eph::core::OutputBuffer;

// ---------------------------------------------------------------------------
// RawStreamCodec
// ---------------------------------------------------------------------------

static void BM_RawStreamEncode(benchmark::State& state) {
    const std::size_t n = static_cast<std::size_t>(state.range(0));
    std::vector<uint8_t> dst(n);
    std::vector<uint8_t> payload(n);
    RawStreamCodec codec;
    for (auto _ : state) {
        auto r = codec.encode(dst.data(), dst.size(),
                              std::span<const uint8_t>{payload});
        benchmark::DoNotOptimize(r);
        benchmark::DoNotOptimize(dst);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(n));
}
BENCHMARK(BM_RawStreamEncode)->Arg(16)->Arg(256)->Arg(4096);

static void BM_RawStreamDecode(benchmark::State& state) {
    const std::size_t n = static_cast<std::size_t>(state.range(0));
    std::vector<uint8_t> wire(n);
    std::array<uint8_t, 32> out_storage{};
    OutputBuffer out{out_storage.data(), out_storage.size()};
    RawStreamCodec codec;
    for (auto _ : state) {
        // Fresh view per iteration — decode advances head to end and any
        // re-iteration would just take the empty-view branch.
        SpanPacketView view{wire.data(), wire.size()};
        auto r = codec.decode(view, out);
        benchmark::DoNotOptimize(r);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(n));
}
BENCHMARK(BM_RawStreamDecode)->Arg(16)->Arg(256)->Arg(4096);

static void BM_RawStreamDecodeEmpty(benchmark::State& state) {
    // Empty-view path: poller fires but read returned 0 bytes. Codec must
    // bail with Ok(None) and not touch any storage.
    std::array<uint8_t, 32> out_storage{};
    OutputBuffer out{out_storage.data(), out_storage.size()};
    RawStreamCodec codec;
    uint8_t dummy = 0;
    for (auto _ : state) {
        SpanPacketView view{&dummy, 0};
        auto r = codec.decode(view, out);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_RawStreamDecodeEmpty);

// ---------------------------------------------------------------------------
// RawDatagramCodec
// ---------------------------------------------------------------------------

static void BM_RawDatagramEncode(benchmark::State& state) {
    const std::size_t n = static_cast<std::size_t>(state.range(0));
    std::vector<uint8_t> dst(n);
    std::vector<uint8_t> payload(n);
    RawDatagramCodec codec;
    for (auto _ : state) {
        auto r = codec.encode(dst.data(), dst.size(),
                              std::span<const uint8_t>{payload});
        benchmark::DoNotOptimize(r);
        benchmark::DoNotOptimize(dst);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(n));
}
BENCHMARK(BM_RawDatagramEncode)->Arg(64)->Arg(512)->Arg(1500);

static void BM_RawDatagramDecode(benchmark::State& state) {
    const std::size_t n = static_cast<std::size_t>(state.range(0));
    std::vector<uint8_t> wire(n);
    std::array<uint8_t, 32> out_storage{};
    OutputBuffer out{out_storage.data(), out_storage.size()};
    RawDatagramCodec codec;
    // Sink intentionally captures by value to avoid lambda-state lookups;
    // the cost we want to measure is the codec's invocation, not closure
    // capture overhead.
    std::size_t accum_len = 0;
    std::function<void(RawDatagramCodec::Frame)> sink =
        [&accum_len](RawDatagramCodec::Frame f) noexcept {
            accum_len += f.size();
        };
    for (auto _ : state) {
        SpanPacketView view{wire.data(), wire.size()};
        auto r = codec.decode(view, out, sink);
        benchmark::DoNotOptimize(r);
    }
    benchmark::DoNotOptimize(accum_len);
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(n));
}
BENCHMARK(BM_RawDatagramDecode)->Arg(64)->Arg(512)->Arg(1500);

int main(int argc, char** argv) {
    // The error-rejection paths (BufferFull / empty-datagram) emit WARN
    // logs at every call. Mute runtime logging globally so the bench
    // measures the codec, not the spdlog flush. Compile-time
    // SPDLOG_ACTIVE_LEVEL only filters debug/trace.
    spdlog::set_level(spdlog::level::off);

    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
