/// @file bench_length_prefix_codec.cpp
/// Microbenchmarks for `eph::codec::LengthPrefixCodec` — the canonical
/// stream framer used by `eph-itch` SoupBinTCP-shaped flows and any
/// length-prefixed protocol over `eph-net-kernel` / `eph-net-dpdk`.
///
/// Coverage gap (batch 11): `eph-codec` had **no** benchmarks before this
/// file. We measure both sides (encode / decode) plus the streaming-
/// reassembly common case (multiple frames in one PacketView) so future
/// changes that touch endianness handling, the size-cap branch, or the
/// trim_front loop have a baseline to measure regressions against.
///
/// Why these particular cases:
/// - **EncodeSmall / EncodeLarge**: encode is two BE writes + memcpy. The
///   small case (16-byte payload) measures the per-frame fixed cost
///   dominated by the prefix write; the large case (4 KiB) measures the
///   memcpy-dominated cost. The ratio gives a quick sense of whether a
///   future change has tilted toward header overhead or toward bulk copy.
/// - **DecodeSingleFrame**: the ITCH-over-SoupBinTCP shape — one frame per
///   PacketView call. Hits all three branches: header read, length parse,
///   payload extract + trim_front.
/// - **DecodeBatchInOneView**: the realistic kernel/DPDK shape where epoll
///   / mbuf returns multiple packets concatenated. Loop until `Ok(None)`
///   exposes the trim_front cost amortized over N frames.
/// - **DecodeOversizeRejectFastPath**: the `kMaxFrameLen` guard fires
///   `[[unlikely]]` once per malicious peer; benchmarking it confirms the
///   error-path is not silently slow (would matter if a flooder kept the
///   kernel busy hammering the cap).

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <benchmark/benchmark.h>
#include <spdlog/spdlog.h>

#include "eph/codec/detail/span_packet_view.hpp"
#include "eph/codec/length_prefix_codec.hpp"
#include "eph/core/codec.hpp"

using eph::codec::LengthPrefixCodec;
using eph::codec::SpanPacketView;
using eph::core::OutputBuffer;

namespace {

/// Build a single-frame wire image (4-byte BE prefix + payload bytes).
[[nodiscard]] std::vector<uint8_t> build_frame(std::size_t payload_len) {
    std::vector<uint8_t> wire(4 + payload_len);
    const auto n = static_cast<std::uint32_t>(payload_len);
    wire[0] = static_cast<uint8_t>((n >> 24) & 0xFF);
    wire[1] = static_cast<uint8_t>((n >> 16) & 0xFF);
    wire[2] = static_cast<uint8_t>((n >>  8) & 0xFF);
    wire[3] = static_cast<uint8_t>((n      ) & 0xFF);
    // Deterministic payload — content shouldn't matter for a length-only
    // codec, but a non-zero pattern catches accidental dependence on
    // specific byte values (e.g. someone reading inside the payload).
    for (std::size_t i = 0; i < payload_len; ++i) {
        wire[4 + i] = static_cast<uint8_t>(i & 0xFF);
    }
    return wire;
}

/// Build N concatenated frames of the same payload size — the realistic
/// shape when an epoll/PMD batch yields multiple packets in one read.
[[nodiscard]] std::vector<uint8_t> build_batch(std::size_t n_frames,
                                                std::size_t payload_len) {
    std::vector<uint8_t> all;
    all.reserve(n_frames * (4 + payload_len));
    for (std::size_t i = 0; i < n_frames; ++i) {
        const auto one = build_frame(payload_len);
        all.insert(all.end(), one.begin(), one.end());
    }
    return all;
}

} // namespace

// ---------------------------------------------------------------------------
// Encode
// ---------------------------------------------------------------------------

static void BM_LpcEncodeSmall(benchmark::State& state) {
    LengthPrefixCodec codec;
    std::array<uint8_t, 64> dst{};
    std::array<uint8_t, 16> payload{};
    for (auto _ : state) {
        auto r = codec.encode(dst.data(), dst.size(),
                              std::span<const uint8_t>{payload});
        benchmark::DoNotOptimize(r);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(4 + payload.size()));
}
BENCHMARK(BM_LpcEncodeSmall);

static void BM_LpcEncodeLarge(benchmark::State& state) {
    LengthPrefixCodec codec;
    constexpr std::size_t kPayload = 4096;
    std::vector<uint8_t> dst(4 + kPayload);
    std::vector<uint8_t> payload(kPayload);
    for (auto _ : state) {
        auto r = codec.encode(dst.data(), dst.size(),
                              std::span<const uint8_t>{payload});
        benchmark::DoNotOptimize(r);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(4 + kPayload));
}
BENCHMARK(BM_LpcEncodeLarge);

// ---------------------------------------------------------------------------
// Decode — single frame per call (ITCH-over-SoupBinTCP shape)
// ---------------------------------------------------------------------------

static void BM_LpcDecodeSingleFrame(benchmark::State& state) {
    const std::size_t payload_len = static_cast<std::size_t>(state.range(0));
    auto wire = build_frame(payload_len);
    LengthPrefixCodec codec;
    std::array<uint8_t, 32> out_storage{};
    OutputBuffer out{out_storage.data(), out_storage.size()};

    for (auto _ : state) {
        // Reset the view each iteration — decode advances it internally,
        // and a fresh view per loop is the realistic per-packet shape.
        SpanPacketView view{wire.data(), wire.size()};
        auto r = codec.decode(view, out);
        benchmark::DoNotOptimize(r);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(wire.size()));
}
BENCHMARK(BM_LpcDecodeSingleFrame)->Arg(16)->Arg(64)->Arg(256)->Arg(1024);

// ---------------------------------------------------------------------------
// Decode — batched: many frames in one PacketView, drain to None
// ---------------------------------------------------------------------------

static void BM_LpcDecodeBatchInOneView(benchmark::State& state) {
    constexpr std::size_t kPayload  = 64;
    const std::size_t n_frames = static_cast<std::size_t>(state.range(0));
    auto wire = build_batch(n_frames, kPayload);
    LengthPrefixCodec codec;
    std::array<uint8_t, 32> out_storage{};
    OutputBuffer out{out_storage.data(), out_storage.size()};

    for (auto _ : state) {
        SpanPacketView view{wire.data(), wire.size()};
        // Drain the whole view in one iteration — mirrors the kernel /
        // DPDK loop "while a frame is available, dispatch it".
        for (std::size_t i = 0; i < n_frames; ++i) {
            auto r = codec.decode(view, out);
            benchmark::DoNotOptimize(r);
        }
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(n_frames));
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(wire.size()));
}
BENCHMARK(BM_LpcDecodeBatchInOneView)->Arg(8)->Arg(32)->Arg(128);

// ---------------------------------------------------------------------------
// Decode — header parsed, payload not yet arrived (Ok(None) fast return)
// ---------------------------------------------------------------------------

static void BM_LpcDecodePartialPayload(benchmark::State& state) {
    // 100-byte payload claimed, only 50 bytes delivered — exercises the
    // "header parsed but payload incomplete" branch which is hot when a
    // slow upstream is dribbling bytes.
    auto wire = build_frame(100);
    const std::size_t partial = 4 + 50;
    LengthPrefixCodec codec;
    std::array<uint8_t, 32> out_storage{};
    OutputBuffer out{out_storage.data(), out_storage.size()};

    for (auto _ : state) {
        SpanPacketView view{wire.data(), partial};
        auto r = codec.decode(view, out);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_LpcDecodePartialPayload);

// ---------------------------------------------------------------------------
// Decode — oversized prefix → CodecOverflow rejection (DoS-resistance check)
// ---------------------------------------------------------------------------

static void BM_LpcDecodeOversizeReject(benchmark::State& state) {
    // Pre-build the rogue header — kMaxFrameLen + 1 in big-endian.
    const std::uint32_t bogus = LengthPrefixCodec::kMaxFrameLen + 1;
    std::array<uint8_t, 4> rogue{
        static_cast<uint8_t>((bogus >> 24) & 0xFF),
        static_cast<uint8_t>((bogus >> 16) & 0xFF),
        static_cast<uint8_t>((bogus >>  8) & 0xFF),
        static_cast<uint8_t>((bogus      ) & 0xFF),
    };
    LengthPrefixCodec codec;
    std::array<uint8_t, 32> out_storage{};
    OutputBuffer out{out_storage.data(), out_storage.size()};

    for (auto _ : state) {
        SpanPacketView view{rogue.data(), rogue.size()};
        auto r = codec.decode(view, out);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_LpcDecodeOversizeReject);

int main(int argc, char** argv) {
    // BM_LpcDecodeOversizeReject hits the codec's WARN path each call;
    // at >100k iterations/second the spdlog flush flood dominates the
    // reported time and drowns the benchmark output. Silence runtime
    // logging globally for this binary — compile-time SPDLOG_ACTIVE_LEVEL
    // already filters debug/trace, but warn/error need an explicit
    // runtime mute. The logger objects (and the slow path itself) are
    // still constructed, so the measurement still reflects the cost of
    // hitting the rejection branch end-to-end.
    spdlog::set_level(spdlog::level::off);

    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
