/// @file bench_ws.cpp
/// WebSocket layer benchmarks — masking, frame encode/decode.
///
/// Depends only on eph-net (no DPDK required).

#include <cstdint>
#include <random>
#include <vector>

#include <benchmark/benchmark.h>

#include "eph/transport/detail/websocket.hpp"

namespace {

void fill_random(uint8_t* buf, size_t len, uint32_t seed = 42) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<uint16_t> dist(0, 255);
    for (size_t i = 0; i < len; ++i) {
        buf[i] = static_cast<uint8_t>(dist(rng));
    }
}

void PayloadSizeArgs(::benchmark::Benchmark* b) {
    for (int sz : {64, 128, 256, 512, 1024}) b->Arg(sz);
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// WebSocket masking (XOR)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_WsMasking(benchmark::State& state) {
    auto sz = static_cast<size_t>(state.range(0));
    std::vector<uint8_t> data(sz, 0xAA);
    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};

    for (auto _ : state) {
        eph::net::ws::apply_mask(data.data(), sz, mask);
        benchmark::DoNotOptimize(data.data());
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(sz));
}
BENCHMARK(BM_WsMasking)->Apply(PayloadSizeArgs);

// ─────────────────────────────────────────────────────────────────────────────
// WebSocket frame encode
// ─────────────────────────────────────────────────────────────────────────────

static void BM_WsEncode(benchmark::State& state) {
    auto sz = static_cast<size_t>(state.range(0));
    std::vector<uint8_t> payload(sz);
    fill_random(payload.data(), sz);
    std::vector<uint8_t> out(eph::net::ws::kMaxFrameHeaderLen + sz);
    auto tmpl = eph::net::ws::FrameTemplate::for_binary();

    for (auto _ : state) {
        auto n = tmpl.encode(out.data(), payload.data(), sz);
        benchmark::DoNotOptimize(n);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(sz));
}
BENCHMARK(BM_WsEncode)->Apply(PayloadSizeArgs);

// ─────────────────────────────────────────────────────────────────────────────
// WebSocket frame decode
// ─────────────────────────────────────────────────────────────────────────────

static void BM_WsDecode(benchmark::State& state) {
    auto sz = static_cast<size_t>(state.range(0));
    std::vector<uint8_t> payload(sz);
    fill_random(payload.data(), sz);
    std::vector<uint8_t> frame_buf(eph::net::ws::kMaxFrameHeaderLen + sz);
    size_t frame_len = eph::net::ws::encode_frame(
        frame_buf.data(), eph::net::ws::opcode::kBinary,
        payload.data(), sz);

    for (auto _ : state) {
        auto r = eph::net::ws::decode_frame(frame_buf.data(), frame_len);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_WsDecode)->Apply(PayloadSizeArgs);

// ─────────────────────────────────────────────────────────────────────────────
// WebSocket control frame build (ping/pong) — covers null-pointer guard path
// ─────────────────────────────────────────────────────────────────────────────

static void BM_WsBuildPing_Empty(benchmark::State& state) {
    uint8_t buf[64];
    for (auto _ : state) {
        auto n = eph::net::ws::build_ping_frame(buf);
        benchmark::DoNotOptimize(n);
    }
}
BENCHMARK(BM_WsBuildPing_Empty);

static void BM_WsBuildPing_WithPayload(benchmark::State& state) {
    uint8_t payload[32];
    fill_random(payload, sizeof(payload));
    uint8_t buf[64];
    for (auto _ : state) {
        auto n = eph::net::ws::build_ping_frame(buf, payload, sizeof(payload));
        benchmark::DoNotOptimize(n);
    }
}
BENCHMARK(BM_WsBuildPing_WithPayload);

static void BM_WsBuildPong(benchmark::State& state) {
    uint8_t payload[32];
    fill_random(payload, sizeof(payload));
    uint8_t buf[64];
    for (auto _ : state) {
        auto n = eph::net::ws::build_pong_frame(buf, payload, sizeof(payload));
        benchmark::DoNotOptimize(n);
    }
}
BENCHMARK(BM_WsBuildPong);

static void BM_WsBuildClose(benchmark::State& state) {
    uint8_t buf[64];
    for (auto _ : state) {
        auto n = eph::net::ws::build_close_frame(buf,
            eph::net::ws::close_code::kNormal, "bye");
        benchmark::DoNotOptimize(n);
    }
}
BENCHMARK(BM_WsBuildClose);

// ─────────────────────────────────────────────────────────────────────────────
// WebSocket encode+decode roundtrip (full pipeline)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_WsRoundtrip(benchmark::State& state) {
    auto sz = static_cast<size_t>(state.range(0));
    std::vector<uint8_t> payload(sz);
    fill_random(payload.data(), sz);
    std::vector<uint8_t> frame_buf(eph::net::ws::kMaxFrameHeaderLen + sz);

    for (auto _ : state) {
        auto enc_len = eph::net::ws::encode_frame(
            frame_buf.data(), eph::net::ws::opcode::kBinary,
            payload.data(), sz);
        auto r = eph::net::ws::decode_frame(frame_buf.data(), enc_len);
        benchmark::DoNotOptimize(r);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(sz));
}
BENCHMARK(BM_WsRoundtrip)->Apply(PayloadSizeArgs);

BENCHMARK_MAIN();
