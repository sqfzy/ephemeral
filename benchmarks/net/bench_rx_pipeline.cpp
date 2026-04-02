/// @file bench_rx_pipeline.cpp
/// In-memory RX pipeline micro-benchmarks.
///
/// Measures the pure computation cost of the RX data path without any
/// network I/O, isolating two key stages:
///
///   1. Twophase frame filter — per-symbol latest-only dedup
///   2. Full RX pipeline — TLS decrypt → WS decode → filter → deliver
///
/// Parameters are chosen to match real Binance combined bookTicker traffic:
///   - frames/record: 1–100 (Binance: 1–60 depending on volatility)
///   - payload size: 64–512 bytes (Binance bookTicker: ~200 bytes)
///   - symbol count: 3–20

#include <cstdint>
#include <cstring>
#include <random>
#include <span>
#include <vector>

#include <benchmark/benchmark.h>

#include "eph/core/length_prefix_framer.hpp"
#include "eph/transport/raw_framer.hpp"
#include "eph/transport/tls_record.hpp"
#include "eph/transport/transport_types.hpp"
#include "eph/transport/websocket.hpp"

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

void fill_random(uint8_t* buf, size_t len, uint32_t seed = 42) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<uint16_t> dist(0, 255);
    for (size_t i = 0; i < len; ++i)
        buf[i] = static_cast<uint8_t>(dist(rng));
}

/// Build a TLS hot-state with random key material (same key for both directions).
eph::net::TlsHotState make_tls_state(uint32_t seed = 42) {
    eph::net::TlsHotState state{};
    fill_random(state.write.ki.key, eph::net::tls_const::kAes256KeyLen, seed);
    fill_random(state.write.ki.iv,  eph::net::tls_const::kTls13NonceLen, seed + 1);
    std::memcpy(state.read.ki.key, state.write.ki.key, eph::net::tls_const::kAes256KeyLen);
    std::memcpy(state.read.ki.iv,  state.write.ki.iv,  eph::net::tls_const::kTls13NonceLen);
    state.write.seq = 0;
    state.read.seq  = 0;
    return state;
}

/// Simple hash: return first byte as symbol id. Matches the 4-byte direct
/// read pattern used in the real Binance benchmark (but simpler for testing).
uint32_t test_symbol_hash(const uint8_t* data, size_t len) {
    return len > 0 ? data[0] : 0;
}

/// Encode a single unmasked WS text frame (server→client format).
size_t encode_ws_server_frame(uint8_t* out, const uint8_t* payload, size_t len) {
    // FIN + text opcode
    out[0] = 0x81;
    size_t hdr = 0;
    if (len < 126) {
        out[1] = static_cast<uint8_t>(len);
        hdr = 2;
    } else if (len < 65536) {
        out[1] = 126;
        out[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
        out[3] = static_cast<uint8_t>(len & 0xFF);
        hdr = 4;
    } else {
        out[1] = 127;
        for (int i = 0; i < 8; ++i)
            out[2 + i] = static_cast<uint8_t>((len >> (56 - 8 * i)) & 0xFF);
        hdr = 10;
    }
    std::memcpy(out + hdr, payload, len);
    return hdr + len;
}

/// Build a plaintext buffer containing N WS frames with rotating symbol IDs,
/// then TLS-encrypt it into a single TLS record.
/// Returns {encrypted_record, record_len, plaintext_len}.
struct PreparedRecord {
    std::vector<uint8_t> encrypted;  // TLS record (header + ciphertext + tag)
    size_t record_len = 0;
    size_t plaintext_len = 0;
    size_t num_frames = 0;
};

PreparedRecord build_record(size_t frames, size_t payload_size,
                            size_t num_symbols,
                            eph::net::TlsRecordCrypto& enc) {
    // Generate plaintext: N WS frames with rotating symbol prefixes
    std::vector<uint8_t> plaintext;
    plaintext.reserve(frames * (payload_size + 10));
    std::vector<uint8_t> payload(payload_size);

    for (size_t i = 0; i < frames; ++i) {
        fill_random(payload.data(), payload_size, static_cast<uint32_t>(42 + i));
        payload[0] = static_cast<uint8_t>((i % num_symbols) + 1);

        uint8_t frame_buf[16384];
        size_t frame_len = encode_ws_server_frame(frame_buf, payload.data(), payload_size);
        plaintext.insert(plaintext.end(), frame_buf, frame_buf + frame_len);
    }

    auto plaintext_len = plaintext.size();
    auto enc_size = eph::net::TlsRecordCrypto::encrypted_size(
        static_cast<uint16_t>(plaintext_len));
    std::vector<uint8_t> encrypted(enc_size + 16);

    uint16_t record_len = enc.encrypt(
        plaintext.data(), static_cast<uint16_t>(plaintext_len),
        encrypted.data());

    return {std::move(encrypted), record_len, plaintext_len, frames};
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// BM_TwophaseFilter — pure filter throughput
// ─────────────────────────────────────────────────────────────────────────────

static void BM_TwophaseFilter(benchmark::State& state) {
    auto num_frames  = static_cast<size_t>(state.range(0));
    auto num_symbols = static_cast<size_t>(state.range(1));

    // Build FrameViews with rotating symbol payloads
    constexpr size_t kPayloadSize = 256;
    std::vector<std::vector<uint8_t>> payloads(num_frames);
    for (size_t i = 0; i < num_frames; ++i) {
        payloads[i].resize(kPayloadSize);
        payloads[i][0] = static_cast<uint8_t>((i % num_symbols) + 1);
    }

    auto filter = eph::net::make_twophase_filter(test_symbol_hash);

    for ([[maybe_unused]] auto _ : state) {
        // Reset deliver flags each iteration
        std::vector<eph::net::FrameView> views(num_frames);
        for (size_t i = 0; i < num_frames; ++i) {
            views[i] = {payloads[i].data(),
                        static_cast<uint16_t>(kPayloadSize), 1, true};
        }

        filter(std::span{views.data(), views.size()});

        benchmark::DoNotOptimize(views.data());
    }

    state.SetItemsProcessed(state.iterations() *
                            static_cast<int64_t>(num_frames));
    state.counters["frames"] = static_cast<double>(num_frames);
    state.counters["symbols"] = static_cast<double>(num_symbols);
}

BENCHMARK(BM_TwophaseFilter)
    ->ArgsProduct({
        {1, 10, 30, 50, 100},  // frames per batch
        {3, 10},               // symbol count
    })
    ->Unit(benchmark::kNanosecond);

// ─────────────────────────────────────────────────────────────────────────────
// BM_RxPipeline — decrypt → decode → filter → deliver (in-memory)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_RxPipeline(benchmark::State& state) {
    auto num_frames   = static_cast<size_t>(state.range(0));
    auto payload_size = static_cast<size_t>(state.range(1));
    constexpr size_t kNumSymbols = 3;

    // Setup: build encrypted TLS record
    auto hot = make_tls_state();
    auto enc = eph::net::TlsRecordCrypto::create(hot);
    if (!enc) { state.SkipWithError(enc.error()); return; }

    // Decrypt uses same key but separate state
    eph::net::TlsHotState dec_hot{};
    std::memcpy(dec_hot.read.ki.key, hot.write.ki.key, eph::net::tls_const::kAes256KeyLen);
    std::memcpy(dec_hot.read.ki.iv,  hot.write.ki.iv,  eph::net::tls_const::kTls13NonceLen);

    auto record = build_record(num_frames, payload_size, kNumSymbols, *enc);
    if (record.record_len == 0) {
        state.SkipWithError("Failed to build TLS record");
        return;
    }

    std::vector<uint8_t> decrypt_buf(record.plaintext_len + 256);
    auto filter = eph::net::make_twophase_filter(test_symbol_hash);
    volatile uint64_t sink = 0;

    for ([[maybe_unused]] auto _ : state) {
        // Re-create decryptor each iteration to reset sequence counter
        dec_hot.read.seq = 0;
        auto dec = eph::net::TlsRecordCrypto::create(dec_hot);
        if (!dec) [[unlikely]] { state.SkipWithError(dec.error()); return; }

        // Stage 1: TLS decrypt
        uint16_t decrypted_len = 0;
        bool ok = dec->decrypt(
            record.encrypted.data(),
            static_cast<uint16_t>(record.record_len),
            decrypt_buf.data(), decrypted_len);
        if (!ok) [[unlikely]] {
            state.SkipWithError("TLS decrypt failed");
            return;
        }

        // Stage 2: WS frame decode
        std::vector<eph::net::FrameView> views;
        views.reserve(num_frames);
        size_t offset = 0;
        while (offset < decrypted_len) {
            auto frame = eph::net::ws::decode_frame(
                decrypt_buf.data() + offset, decrypted_len - offset);
            if (!frame) break;

            if (frame->is_data() && frame->payload_len > 0) {
                views.push_back({
                    frame->payload,
                    static_cast<uint16_t>(frame->payload_len),
                    frame->opcode, true});
            }
            offset += frame->total_len;
        }

        // Stage 3: Twophase filter
        filter(std::span{views.data(), views.size()});

        // Stage 4: Deliver (simulate rx_enqueue with DoNotOptimize)
        for (auto& v : views) {
            if (v.deliver) {
                sink = *reinterpret_cast<const uint64_t*>(v.payload);
            }
        }
        benchmark::DoNotOptimize(sink);
    }

    state.SetBytesProcessed(state.iterations() *
                            static_cast<int64_t>(record.record_len));
    state.counters["frames"] = static_cast<double>(num_frames);
    state.counters["payload"] = static_cast<double>(payload_size);
    state.counters["record_bytes"] = static_cast<double>(record.record_len);
}

// TLS record max payload is ~16KB. Limit combinations to stay within that.
// 100 frames × 64 bytes = ~6.6KB (ok), 100 × 256 = ~26KB (too large).
BENCHMARK(BM_RxPipeline)
    ->Args({1, 64})->Args({1, 256})->Args({1, 512})
    ->Args({10, 64})->Args({10, 256})->Args({10, 512})
    ->Args({30, 64})->Args({30, 256})->Args({30, 512})
    ->Args({100, 64})  // only 64B payload fits in 16KB TLS record at 100 frames
    ->Unit(benchmark::kNanosecond);

// ─────────────────────────────────────────────────────────────────────────────
// BM_RxPipeline_LengthPrefix — decrypt → length-prefix decode → deliver
//
// Simulates ITCH-style protocol: 2-byte big-endian length prefix framing
// over TLS, no WebSocket overhead. Measures the cost of the length-prefix
// framer vs the WS framer in the pipeline.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// Encode a single length-prefixed message (2-byte BE length + payload).
size_t encode_length_prefix_frame(uint8_t* out, const uint8_t* payload, size_t len) {
    out[0] = static_cast<uint8_t>((len >> 8) & 0xFF);
    out[1] = static_cast<uint8_t>(len & 0xFF);
    std::memcpy(out + 2, payload, len);
    return 2 + len;
}

PreparedRecord build_record_lp(size_t frames, size_t payload_size,
                                eph::net::TlsRecordCrypto& enc) {
    std::vector<uint8_t> plaintext;
    plaintext.reserve(frames * (payload_size + 2));
    std::vector<uint8_t> payload(payload_size);

    for (size_t i = 0; i < frames; ++i) {
        fill_random(payload.data(), payload_size, static_cast<uint32_t>(42 + i));
        payload[0] = static_cast<uint8_t>((i % 3) + 1); // symbol id

        uint8_t frame_buf[16384];
        size_t frame_len = encode_length_prefix_frame(frame_buf, payload.data(), payload_size);
        plaintext.insert(plaintext.end(), frame_buf, frame_buf + frame_len);
    }

    auto plaintext_len = plaintext.size();
    auto enc_size = eph::net::TlsRecordCrypto::encrypted_size(
        static_cast<uint16_t>(plaintext_len));
    std::vector<uint8_t> encrypted(enc_size + 16);

    uint16_t record_len = enc.encrypt(
        plaintext.data(), static_cast<uint16_t>(plaintext_len),
        encrypted.data());

    return {std::move(encrypted), record_len, plaintext_len, frames};
}

/// Build a single plaintext blob for RawFramer (no per-message framing).
PreparedRecord build_record_raw(size_t payload_size,
                                 eph::net::TlsRecordCrypto& enc) {
    std::vector<uint8_t> plaintext(payload_size);
    fill_random(plaintext.data(), payload_size, 42);

    auto enc_size = eph::net::TlsRecordCrypto::encrypted_size(
        static_cast<uint16_t>(payload_size));
    std::vector<uint8_t> encrypted(enc_size + 16);

    uint16_t record_len = enc.encrypt(
        plaintext.data(), static_cast<uint16_t>(payload_size),
        encrypted.data());

    return {std::move(encrypted), record_len, payload_size, 1};
}

} // anonymous namespace

static void BM_RxPipeline_LengthPrefix(benchmark::State& state) {
    auto num_frames   = static_cast<size_t>(state.range(0));
    auto payload_size = static_cast<size_t>(state.range(1));

    auto hot = make_tls_state();
    auto enc = eph::net::TlsRecordCrypto::create(hot);
    if (!enc) { state.SkipWithError(enc.error()); return; }

    eph::net::TlsHotState dec_hot{};
    std::memcpy(dec_hot.read.ki.key, hot.write.ki.key, eph::net::tls_const::kAes256KeyLen);
    std::memcpy(dec_hot.read.ki.iv,  hot.write.ki.iv,  eph::net::tls_const::kTls13NonceLen);

    auto record = build_record_lp(num_frames, payload_size, *enc);
    if (record.record_len == 0) {
        state.SkipWithError("Failed to build TLS record");
        return;
    }

    std::vector<uint8_t> decrypt_buf(record.plaintext_len + 256);
    volatile uint64_t sink = 0;

    for ([[maybe_unused]] auto _ : state) {
        dec_hot.read.seq = 0;
        auto dec = eph::net::TlsRecordCrypto::create(dec_hot);
        if (!dec) [[unlikely]] { state.SkipWithError(dec.error()); return; }

        // Stage 1: TLS decrypt
        uint16_t decrypted_len = 0;
        bool ok = dec->decrypt(
            record.encrypted.data(),
            static_cast<uint16_t>(record.record_len),
            decrypt_buf.data(), decrypted_len);
        if (!ok) [[unlikely]] { state.SkipWithError("TLS decrypt failed"); return; }

        // Stage 2: LengthPrefixFramer decode
        size_t offset = 0;
        size_t delivered = 0;
        while (offset < decrypted_len) {
            auto frame = eph::net::LengthPrefixFramer{}.decode(
                decrypt_buf.data() + offset, decrypted_len - offset);
            if (!frame) break;

            // Stage 3: Deliver
            sink = *reinterpret_cast<const uint64_t*>(frame->payload);
            delivered++;
            offset += frame->total_len;
        }
        benchmark::DoNotOptimize(sink);
    }

    state.SetBytesProcessed(state.iterations() *
                            static_cast<int64_t>(record.record_len));
    state.counters["frames"] = static_cast<double>(num_frames);
    state.counters["payload"] = static_cast<double>(payload_size);
    state.counters["record_bytes"] = static_cast<double>(record.record_len);
}

BENCHMARK(BM_RxPipeline_LengthPrefix)
    ->Args({1, 64})->Args({1, 256})->Args({1, 512})
    ->Args({10, 64})->Args({10, 256})->Args({10, 512})
    ->Args({30, 64})->Args({30, 256})->Args({30, 512})
    ->Args({100, 64})
    ->Unit(benchmark::kNanosecond);

// ─────────────────────────────────────────────────────────────────────────────
// BM_RxPipeline_Raw — decrypt → raw deliver (no framing layer)
//
// Baseline: TLS decrypt + immediate delivery. No WS or length-prefix framing.
// Shows the minimum achievable pipeline latency (TLS AEAD cost only).
// Represents FIX-over-TLS or any protocol with application-level framing.
// ─────────────────────────────────────────────────────────────────────────────

static void BM_RxPipeline_Raw(benchmark::State& state) {
    auto payload_size = static_cast<size_t>(state.range(0));

    auto hot = make_tls_state();
    auto enc = eph::net::TlsRecordCrypto::create(hot);
    if (!enc) { state.SkipWithError(enc.error()); return; }

    eph::net::TlsHotState dec_hot{};
    std::memcpy(dec_hot.read.ki.key, hot.write.ki.key, eph::net::tls_const::kAes256KeyLen);
    std::memcpy(dec_hot.read.ki.iv,  hot.write.ki.iv,  eph::net::tls_const::kTls13NonceLen);

    auto record = build_record_raw(payload_size, *enc);
    if (record.record_len == 0) {
        state.SkipWithError("Failed to build TLS record");
        return;
    }

    std::vector<uint8_t> decrypt_buf(record.plaintext_len + 256);
    volatile uint64_t sink = 0;

    for ([[maybe_unused]] auto _ : state) {
        dec_hot.read.seq = 0;
        auto dec = eph::net::TlsRecordCrypto::create(dec_hot);
        if (!dec) [[unlikely]] { state.SkipWithError(dec.error()); return; }

        // Stage 1: TLS decrypt
        uint16_t decrypted_len = 0;
        bool ok = dec->decrypt(
            record.encrypted.data(),
            static_cast<uint16_t>(record.record_len),
            decrypt_buf.data(), decrypted_len);
        if (!ok) [[unlikely]] { state.SkipWithError("TLS decrypt failed"); return; }

        // Stage 2: RawFramer decode (passthrough)
        auto frame = eph::net::RawFramer{}.decode(
            decrypt_buf.data(), decrypted_len);

        // Stage 3: Deliver
        if (frame) {
            sink = *reinterpret_cast<const uint64_t*>(frame->payload);
        }
        benchmark::DoNotOptimize(sink);
    }

    state.SetBytesProcessed(state.iterations() *
                            static_cast<int64_t>(record.record_len));
    state.counters["payload"] = static_cast<double>(payload_size);
    state.counters["record_bytes"] = static_cast<double>(record.record_len);
}

BENCHMARK(BM_RxPipeline_Raw)
    ->Arg(64)->Arg(256)->Arg(512)->Arg(1024)->Arg(4096)
    ->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();
