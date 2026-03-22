/// @file bench_tls.cpp
/// TLS 1.3 record layer benchmarks — AES-256-GCM encrypt/decrypt.
///
/// Parameterized across payload sizes (64B–4096B) to reveal
/// AES-NI amortization effects on throughput.
///
/// Depends only on eph-net (no DPDK required).

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include <benchmark/benchmark.h>

#include "eph/net/tls_record.hpp"

namespace {

void fill_random(uint8_t* buf, size_t len, uint32_t seed = 42) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<uint16_t> dist(0, 255);
    for (size_t i = 0; i < len; ++i) {
        buf[i] = static_cast<uint8_t>(dist(rng));
    }
}

eph::net::TlsHotState make_roundtrip_state(uint32_t seed = 42) {
    eph::net::TlsHotState state{};
    fill_random(state.write.ki.key, eph::net::tls_const::kAes256KeyLen, seed);
    fill_random(state.write.ki.iv,  eph::net::tls_const::kTls13NonceLen, seed + 1);
    std::memcpy(state.read.ki.key, state.write.ki.key, eph::net::tls_const::kAes256KeyLen);
    std::memcpy(state.read.ki.iv,  state.write.ki.iv,  eph::net::tls_const::kTls13NonceLen);
    state.write.seq = 0;
    state.read.seq  = 0;
    return state;
}

void TlsPayloadSizeArgs(::benchmark::Benchmark* b) {
    for (int sz : {64, 128, 256, 512, 1024, 2048, 4096}) b->Arg(sz);
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// TLS record encrypt (AES-256-GCM)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_TlsEncrypt(benchmark::State& state) {
    auto sz = static_cast<uint16_t>(state.range(0));
    auto hot = make_roundtrip_state();
    auto crypto = eph::net::TlsRecordCrypto::create(hot);
    if (!crypto) { state.SkipWithError(crypto.error()); return; }

    // +1 byte for encrypt()'s temporary content-type append
    std::vector<uint8_t> plaintext(sz + 1);
    fill_random(plaintext.data(), sz, 10);
    uint16_t out_size = eph::net::TlsRecordCrypto::encrypted_size(sz);
    std::vector<uint8_t> ciphertext(out_size);

    for (auto _ : state) {
        auto n = crypto->encrypt(plaintext.data(), sz, ciphertext.data());
        benchmark::DoNotOptimize(n);
    }
    state.SetBytesProcessed(state.iterations() * sz);
}
BENCHMARK(BM_TlsEncrypt)->Apply(TlsPayloadSizeArgs);

// ─────────────────────────────────────────────────────────────────────────────
// TLS record decrypt (AES-256-GCM)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_TlsDecrypt(benchmark::State& state) {
    auto sz = static_cast<uint16_t>(state.range(0));
    auto hot = make_roundtrip_state();

    // Encrypt once to produce a valid record for decrypting
    auto enc = eph::net::TlsRecordCrypto::create(hot);
    if (!enc) { state.SkipWithError(enc.error()); return; }

    std::vector<uint8_t> plaintext(sz + 1);
    fill_random(plaintext.data(), sz, 10);
    uint16_t record_size = eph::net::TlsRecordCrypto::encrypted_size(sz);

    // Pre-encrypt enough records for all iterations (each uses a unique seq)
    int64_t batch = state.max_iterations > 0 ? static_cast<int64_t>(state.max_iterations) : 10'000'000;
    if (batch > 2'000'000) batch = 2'000'000;

    std::vector<std::vector<uint8_t>> records(batch, std::vector<uint8_t>(record_size));
    for (int64_t i = 0; i < batch; ++i) {
        enc->encrypt(plaintext.data(), sz, records[i].data());
    }

    // Create decryptor with matching read keys
    eph::net::TlsHotState dec_hot{};
    std::memcpy(dec_hot.read.ki.key, hot.write.ki.key, eph::net::tls_const::kAes256KeyLen);
    std::memcpy(dec_hot.read.ki.iv,  hot.write.ki.iv,  eph::net::tls_const::kTls13NonceLen);
    auto dec = eph::net::TlsRecordCrypto::create(dec_hot);
    if (!dec) { state.SkipWithError(dec.error()); return; }

    std::vector<uint8_t> decrypted(sz + 16);
    int64_t idx = 0;

    for (auto _ : state) {
        if (idx > 0 && idx % batch == 0) {
            dec = eph::net::TlsRecordCrypto::create(dec_hot);
        }
        uint16_t dec_len;
        auto ok = dec->decrypt(records[idx % batch].data(), record_size,
                                decrypted.data(), dec_len);
        benchmark::DoNotOptimize(ok);
        idx++;
    }
    state.SetBytesProcessed(state.iterations() * sz);
}
BENCHMARK(BM_TlsDecrypt)->Apply(TlsPayloadSizeArgs);

BENCHMARK_MAIN();
