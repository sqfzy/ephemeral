/// @file bench_ws_codec.cpp
/// Microbenchmarks for `eph::codec::WsCodec` — RFC 6455 frame parser +
/// state machine sitting on every WS-based market-data feed.
///
/// Coverage gap (batch 11): WsCodec is the most complex codec in the
/// repo (frame parse + control-frame state machine + auto pong/close-ack
/// + fragmentation reassembly + optional permessage-deflate). It had no
/// microbench. This file pins the per-call cost of:
///
/// - The 95% case: server-to-client unmasked text frame, single-message,
///   small (32 B) and typical (256 B / 1024 B) payload sizes.
/// - The encode (client-to-server) path: the fused copy+mask helper that
///   every outgoing message hits.
/// - The auto-pong path: a server-sent ping triggers an in-place pong
///   write into the OutputBuffer plus an Ok(None) return.
/// - The fragmented reassembly path: a 2-fragment text message — the
///   internal accumulator is the riskiest performance regression vector
///   (memcpy + size growth on every fragment).
///
/// Why these choices specifically:
/// - WS server frames are NOT masked (RFC 6455 §5.1), so a server-to-
///   client decode skips the unmask XOR pass — measuring this path is
///   the realistic shape for a market-data consumer.
/// - The encode path IS masked (clients MUST mask): we measure the
///   fused single-pass copy+mask in `ws::encode_frame`.
/// - We deliberately do NOT bench permessage-deflate: it depends on
///   `enable_permessage_deflate` having been called and on a real
///   compressed payload; that's a separate concern and has its own test
///   suite (`test_ws_codec_deflate`).

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include <benchmark/benchmark.h>
#include <spdlog/spdlog.h>

#include "eph/codec/detail/span_packet_view.hpp"
#include "eph/codec/ws_codec.hpp"
#include "eph/core/codec.hpp"
#include "eph/net/detail/websocket.hpp"

using eph::codec::SpanPacketView;
using eph::codec::WsCodec;
using eph::core::OutputBuffer;

namespace {

namespace ws = eph::net::ws;

/// Build a server-to-client text frame (FIN=1, MASK=0). RFC 6455 §5.1 —
/// servers MUST NOT mask outbound frames. This is the realistic shape
/// every market-data WS consumer sees.
[[nodiscard]] std::vector<uint8_t> build_server_text(std::size_t payload_len) {
    std::vector<uint8_t> wire;
    // Header: byte0 = FIN | text(0x1) = 0x81; byte1 = (no mask) | len.
    wire.push_back(0x81);
    if (payload_len < 126) {
        wire.push_back(static_cast<uint8_t>(payload_len));
    } else if (payload_len <= 0xFFFF) {
        wire.push_back(126);
        wire.push_back(static_cast<uint8_t>(payload_len >> 8));
        wire.push_back(static_cast<uint8_t>(payload_len & 0xFF));
    } else {
        wire.push_back(127);
        for (int i = 7; i >= 0; --i) {
            wire.push_back(static_cast<uint8_t>(
                (static_cast<uint64_t>(payload_len) >> (i * 8)) & 0xFF));
        }
    }
    // Payload: deterministic non-zero pattern so any unintended unmask
    // pass would still produce non-trivial work.
    for (std::size_t i = 0; i < payload_len; ++i) {
        wire.push_back(static_cast<uint8_t>('a' + (i % 26)));
    }
    return wire;
}

/// Build a server-to-client ping frame. RFC 6455 §5.5.2 — control frames
/// must be ≤ 125 bytes; we use a small non-zero "ping payload".
[[nodiscard]] std::vector<uint8_t> build_server_ping() {
    std::vector<uint8_t> wire;
    wire.push_back(0x89);  // FIN | ping(0x9)
    wire.push_back(0x04);  // no mask, payload_len=4
    wire.push_back('p');
    wire.push_back('i');
    wire.push_back('n');
    wire.push_back('g');
    return wire;
}

/// Build TWO server-to-client text fragments that together form a single
/// 200-byte message: first 100 bytes (FIN=0), then continuation (FIN=1).
/// Exercises the WsCodec internal reassembly buffer.
[[nodiscard]] std::pair<std::vector<uint8_t>, std::vector<uint8_t>>
build_server_fragmented_text(std::size_t total_len) {
    const auto half = total_len / 2;
    std::vector<uint8_t> a, b;
    // Fragment 1: text opcode (0x1), FIN=0 → 0x01.
    a.push_back(0x01);
    a.push_back(static_cast<uint8_t>(half));
    for (std::size_t i = 0; i < half; ++i) {
        a.push_back(static_cast<uint8_t>('A' + (i % 26)));
    }
    // Fragment 2: continuation (0x0), FIN=1 → 0x80.
    const auto rest = total_len - half;
    b.push_back(0x80);
    b.push_back(static_cast<uint8_t>(rest));
    for (std::size_t i = 0; i < rest; ++i) {
        b.push_back(static_cast<uint8_t>('a' + (i % 26)));
    }
    return {std::move(a), std::move(b)};
}

} // namespace

// ---------------------------------------------------------------------------
// Decode — server-to-client text frame, the 95% case
// ---------------------------------------------------------------------------

static void BM_WsCodecDecodeServerText(benchmark::State& state) {
    const std::size_t payload_len = static_cast<std::size_t>(state.range(0));
    auto wire = build_server_text(payload_len);
    std::array<uint8_t, 64> out_storage{};
    OutputBuffer out{out_storage.data(), out_storage.size()};

    for (auto _ : state) {
        WsCodec codec;
        SpanPacketView view{wire.data(), wire.size()};
        auto r = codec.decode(view, out);
        benchmark::DoNotOptimize(r);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(wire.size()));
}
BENCHMARK(BM_WsCodecDecodeServerText)->Arg(32)->Arg(256)->Arg(1024);

// ---------------------------------------------------------------------------
// Encode — client-to-server (always masked per RFC 6455 §5.1)
// ---------------------------------------------------------------------------

static void BM_WsCodecEncodeBinary(benchmark::State& state) {
    const std::size_t payload_len = static_cast<std::size_t>(state.range(0));
    // Header is at most 14 bytes; include a generous tail.
    std::vector<uint8_t> dst(14 + payload_len);
    std::vector<uint8_t> payload(payload_len);
    for (std::size_t i = 0; i < payload_len; ++i) {
        payload[i] = static_cast<uint8_t>('a' + (i % 26));
    }
    WsCodec codec;
    for (auto _ : state) {
        auto r = codec.encode(dst.data(), dst.size(),
                              std::span<const uint8_t>{payload});
        benchmark::DoNotOptimize(r);
        benchmark::DoNotOptimize(dst);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(payload_len));
}
BENCHMARK(BM_WsCodecEncodeBinary)->Arg(32)->Arg(256)->Arg(1024);

// ---------------------------------------------------------------------------
// Decode — server ping → auto-pong + Ok(None) return
// ---------------------------------------------------------------------------

static void BM_WsCodecDecodeAutoPong(benchmark::State& state) {
    auto wire = build_server_ping();
    std::array<uint8_t, 64> out_storage{};

    for (auto _ : state) {
        WsCodec codec;
        // Fresh OutputBuffer per iteration: auto-pong appends to it, so
        // accumulating across loops would skew the per-call number.
        OutputBuffer out{out_storage.data(), out_storage.size()};
        SpanPacketView view{wire.data(), wire.size()};
        auto r = codec.decode(view, out);
        benchmark::DoNotOptimize(r);
        benchmark::DoNotOptimize(out_storage);
    }
}
BENCHMARK(BM_WsCodecDecodeAutoPong);

// ---------------------------------------------------------------------------
// Decode — fragmented message (Ok(None) on first, Ok(Some(reassembled))
// on second). The internal reassembly buffer is the riskiest perf
// regression vector — a misplaced .reserve() growth or per-fragment
// memcpy double-up surfaces here.
// ---------------------------------------------------------------------------

static void BM_WsCodecDecodeFragmentedReassembly(benchmark::State& state) {
    auto [frag1, frag2] = build_server_fragmented_text(/*total=*/200);
    std::array<uint8_t, 64> out_storage{};

    for (auto _ : state) {
        WsCodec codec;
        OutputBuffer out{out_storage.data(), out_storage.size()};
        SpanPacketView v1{frag1.data(), frag1.size()};
        auto r1 = codec.decode(v1, out);
        benchmark::DoNotOptimize(r1);
        SpanPacketView v2{frag2.data(), frag2.size()};
        auto r2 = codec.decode(v2, out);
        benchmark::DoNotOptimize(r2);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_WsCodecDecodeFragmentedReassembly);

// ---------------------------------------------------------------------------
// Decode — partial header (Ok(None) on < 2 bytes available)
// ---------------------------------------------------------------------------

static void BM_WsCodecDecodePartialHeader(benchmark::State& state) {
    // Only 1 byte delivered — codec must early-return None.
    std::array<uint8_t, 1> wire{0x81};
    std::array<uint8_t, 64> out_storage{};
    OutputBuffer out{out_storage.data(), out_storage.size()};

    for (auto _ : state) {
        WsCodec codec;
        SpanPacketView view{wire.data(), wire.size()};
        auto r = codec.decode(view, out);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_WsCodecDecodePartialHeader);

int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::off);
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
