/// @file bench_mold64_codec.cpp
/// Microbenchmarks for `eph::codec::Mold64Codec` — Nasdaq MoldUDP64
/// container codec used by every ITCH multicast feed consumer.
///
/// Coverage gap (batch 11): the per-datagram delivery loop iterates `N`
/// length-prefixed messages and forwards each through a `std::function`
/// sink — a hot path for any market-data consumer. There was no
/// microbench before this file. We measure both representative N-per-
/// datagram fanouts (1 / 4 / 16) at typical ITCH message sizes (8 / 40 B)
/// and pin the in-order steady-state, the gap-detected path (sequence_
/// number > expected_seq_), the end-of-session fast-return, and the
/// malformed-header CodecBad rejection.
///
/// Why these particular cases:
/// - **Decode N×size**: real ITCH packets pack 1-50 messages per datagram
///   at ~8-40 bytes each. The per-message cost dominates; the bench
///   confirms throughput scales linearly with N (no quadratic surprise).
/// - **DecodeWithGap**: the gap branch only differs by a counter increment
///   and a WARN log. Bench checks the cost stays close to the
///   steady-state path so a sustained gap-storm doesn't tank throughput.
/// - **DecodeEndOfSession**: count == 0xFFFF is the session-end marker.
///   Fast-return path (no per-message loop) — confirms it stays ~tens of
///   ns and not "I forgot a branch and walked the empty body".
/// - **DecodeMalformedHeader**: a truncated datagram. Hits the CodecBad
///   error path. Same DoS-resistance check as the LengthPrefixCodec
///   oversize bench — a flooder hammering bad headers must not be more
///   expensive than dropping a real packet.
/// - **EncodeOne**: the convenience encoder used by tests. Validates the
///   header + length prefix + payload memcpy stays cheap.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <vector>

#include <benchmark/benchmark.h>
#include <spdlog/spdlog.h>

#include "eph/codec/detail/span_packet_view.hpp"
#include "eph/codec/mold64_codec.hpp"
#include "eph/core/codec.hpp"
#include "eph/itch/moldudp64.hpp"

using eph::codec::Mold64Codec;
using eph::codec::Mold64Config;
using eph::codec::SpanPacketView;
using eph::core::OutputBuffer;

namespace {

/// Build a real MoldUDP64 datagram with `n_msgs` payload bodies of `body_len`
/// bytes each. Sequence numbers start at `seq_start` (BE in the header) and
/// the per-message bodies are deterministic (i & 0xFF) so the codec doesn't
/// see all-zeros (which can fold poorly under a vectorising memcpy).
[[nodiscard]] std::vector<uint8_t> build_packet(uint64_t seq_start,
                                                std::size_t n_msgs,
                                                std::size_t body_len) {
    std::vector<uint8_t> out;
    out.resize(eph::itch::moldudp64::kHeaderLen);
    // 10 NUL session bytes (default in `out`); seq big-endian uint64.
    for (int i = 0; i < 8; ++i) {
        out[10 + i] = static_cast<uint8_t>(
            (seq_start >> ((7 - i) * 8)) & 0xFF);
    }
    const auto count = static_cast<uint16_t>(n_msgs);
    out[18] = static_cast<uint8_t>((count >> 8) & 0xFF);
    out[19] = static_cast<uint8_t>(count & 0xFF);
    for (std::size_t i = 0; i < n_msgs; ++i) {
        const auto blen = static_cast<uint16_t>(body_len);
        out.push_back(static_cast<uint8_t>((blen >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(blen & 0xFF));
        for (std::size_t b = 0; b < body_len; ++b) {
            out.push_back(static_cast<uint8_t>(b & 0xFF));
        }
    }
    return out;
}

/// End-of-session marker: count == 0xFFFF, no body bytes.
[[nodiscard]] std::vector<uint8_t> build_eos_packet(uint64_t seq) {
    std::vector<uint8_t> out(eph::itch::moldudp64::kHeaderLen);
    for (int i = 0; i < 8; ++i) {
        out[10 + i] = static_cast<uint8_t>((seq >> ((7 - i) * 8)) & 0xFF);
    }
    out[18] = 0xFF;
    out[19] = 0xFF;
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Decode: in-order steady-state, varying N × payload-size
// ---------------------------------------------------------------------------

static void BM_Mold64DecodeInOrder(benchmark::State& state) {
    const std::size_t n_msgs   = static_cast<std::size_t>(state.range(0));
    const std::size_t body_len = static_cast<std::size_t>(state.range(1));
    auto wire = build_packet(/*seq=*/1, n_msgs, body_len);

    std::array<uint8_t, 32> out_storage{};
    OutputBuffer out{out_storage.data(), out_storage.size()};

    // Sink that just sums payload lengths so the compiler can't strip the
    // delivery side-effect. `accum` lives across iterations so the sink
    // is unmistakably observable.
    std::size_t accum = 0;
    std::function<void(Mold64Codec::Frame)> sink =
        [&accum](Mold64Codec::Frame f) noexcept {
            accum += f.payload.size();
        };

    for (auto _ : state) {
        // Each iteration must restart the codec at expected_seq=1 so we
        // bench the steady-state path (no gap) every loop. Constructing a
        // codec is just two uint64 stores, negligible vs. the parse loop.
        Mold64Codec codec{Mold64Config{.initial_expected_seq = 1}};
        SpanPacketView view{wire.data(), wire.size()};
        auto r = codec.decode(view, out, sink);
        benchmark::DoNotOptimize(r);
    }
    benchmark::DoNotOptimize(accum);
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(n_msgs));
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(wire.size()));
}
BENCHMARK(BM_Mold64DecodeInOrder)
    ->Args({1, 8})->Args({1, 40})
    ->Args({4, 8})->Args({4, 40})
    ->Args({16, 8})->Args({16, 40});

// ---------------------------------------------------------------------------
// Decode with gap: incoming seq jumped past expected. Confirms the gap
// branch (counter increment + WARN log gated by spdlog::off in main) is
// not measurably slower than the in-order path.
// ---------------------------------------------------------------------------

static void BM_Mold64DecodeWithGap(benchmark::State& state) {
    // Codec expects seq=1; packet arrives with seq=1000 (gap of 999).
    auto wire = build_packet(/*seq=*/1000, /*n_msgs=*/4, /*body_len=*/40);
    std::array<uint8_t, 32> out_storage{};
    OutputBuffer out{out_storage.data(), out_storage.size()};
    std::size_t accum = 0;
    std::function<void(Mold64Codec::Frame)> sink =
        [&accum](Mold64Codec::Frame f) noexcept {
            accum += f.payload.size();
        };

    for (auto _ : state) {
        Mold64Codec codec{Mold64Config{.initial_expected_seq = 1}};
        SpanPacketView view{wire.data(), wire.size()};
        auto r = codec.decode(view, out, sink);
        benchmark::DoNotOptimize(r);
    }
    benchmark::DoNotOptimize(accum);
}
BENCHMARK(BM_Mold64DecodeWithGap);

// ---------------------------------------------------------------------------
// Decode end-of-session: count == 0xFFFF fast-return — no per-message loop.
// ---------------------------------------------------------------------------

static void BM_Mold64DecodeEndOfSession(benchmark::State& state) {
    auto wire = build_eos_packet(/*seq=*/42);
    std::array<uint8_t, 32> out_storage{};
    OutputBuffer out{out_storage.data(), out_storage.size()};
    std::function<void(Mold64Codec::Frame)> sink =
        [](Mold64Codec::Frame) noexcept {};

    for (auto _ : state) {
        Mold64Codec codec;
        SpanPacketView view{wire.data(), wire.size()};
        auto r = codec.decode(view, out, sink);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_Mold64DecodeEndOfSession);

// ---------------------------------------------------------------------------
// Decode malformed header — DoS-resistance check: a flooder spraying short
// datagrams shouldn't make the codec more expensive than the legitimate
// per-packet cost.
// ---------------------------------------------------------------------------

static void BM_Mold64DecodeMalformedHeader(benchmark::State& state) {
    // Truncated to 10 bytes — well under the 20-byte header.
    std::array<uint8_t, 10> wire{};
    std::array<uint8_t, 32> out_storage{};
    OutputBuffer out{out_storage.data(), out_storage.size()};
    std::function<void(Mold64Codec::Frame)> sink =
        [](Mold64Codec::Frame) noexcept {};

    for (auto _ : state) {
        Mold64Codec codec;
        SpanPacketView view{wire.data(), wire.size()};
        auto r = codec.decode(view, out, sink);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_Mold64DecodeMalformedHeader);

// ---------------------------------------------------------------------------
// Encode one message — convenience producer (intended for tests). Pinning
// the cost ensures the header+prefix+memcpy doesn't quietly grow.
// ---------------------------------------------------------------------------

static void BM_Mold64EncodeOne(benchmark::State& state) {
    const std::size_t body_len = static_cast<std::size_t>(state.range(0));
    std::vector<uint8_t> dst(eph::itch::moldudp64::kHeaderLen + 2 + body_len);
    std::vector<uint8_t> body(body_len);
    Mold64Codec codec;
    Mold64Codec::Frame frame{
        .seq_num = 7,
        .payload = std::span<const uint8_t>{body},
    };
    for (auto _ : state) {
        auto r = codec.encode(dst.data(), dst.size(), frame);
        benchmark::DoNotOptimize(r);
        benchmark::DoNotOptimize(dst);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(dst.size()));
}
BENCHMARK(BM_Mold64EncodeOne)->Arg(8)->Arg(40)->Arg(512);

int main(int argc, char** argv) {
    // The malformed-header / gap-detection / end-of-session paths emit
    // WARN/INFO logs at every call. At >100k iterations/s the spdlog
    // output dominates the measurement; mute runtime logging globally so
    // the bench measures the codec, not the logger.
    spdlog::set_level(spdlog::level::off);

    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
