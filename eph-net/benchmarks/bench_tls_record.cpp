/// @file bench_tls_record.cpp
/// Per-record encrypt / decrypt microbenchmarks for the TLS hot path.
///
/// Why this exists: TLS 1.2 support (added 2026-05-08) inserts a small
/// `switch (record_format_)` in `TlsEncryptor::encrypt` and
/// `TlsDecryptor::decrypt`. The branch is fixed for the lifetime of a
/// session, so prediction should be perfect — but "should be" needs
/// numbers. This bench captures TLS 1.3 vs TLS 1.2 GCM vs TLS 1.2
/// CHACHA20 cycle costs at typical HFT payload sizes (32 / 256 / 1024 B)
/// so a future change touching the record layer has an apples-to-apples
/// baseline.
///
/// Acceptance: TLS 1.3 numbers must not regress from the pre-TLS-1.2
/// baseline (what this branch saved when it was AES-GCM only). TLS 1.2
/// AES-GCM should track within ~10% of TLS 1.3 at the same key size
/// (extra cost: 8B explicit nonce + 13B AAD construction vs 5B AAD).
/// CHACHA20 cycles are software-only on most x86 / arm64 — slower than
/// AES-GCM on hosts with AES-NI / ARMv8 Crypto, but still in the
/// hundred-of-ns range for these sizes.

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include <benchmark/benchmark.h>

#include "eph/net/detail/tls_record.hpp"

namespace {

using namespace eph::net;

/// Build a TlsHotState with deterministic key/IV bytes for the given
/// record format and key length. The actual cryptographic content
/// doesn't matter — bench only measures cycles, not correctness.
TlsHotState make_state(TlsRecordFormat fmt, size_t key_len) {
    TlsHotState s{};
    s.version       = (fmt == TlsRecordFormat::Tls13)
                    ? TlsVersion::Tls13 : TlsVersion::Tls12;
    s.record_format = fmt;
    for (size_t i = 0; i < key_len; ++i) {
        s.write.ki.key[i] = static_cast<uint8_t>(0xA0 + i);
        s.read.ki.key[i]  = static_cast<uint8_t>(0xA0 + i);
    }
    for (size_t i = 0; i < tls_const::kTls13NonceLen; ++i) {
        s.write.ki.iv[i] = static_cast<uint8_t>(0x10 + i);
        s.read.ki.iv[i]  = static_cast<uint8_t>(0x10 + i);
    }
    return s;
}

void encrypt_loop(benchmark::State& st, TlsRecordFormat fmt, size_t key_len) {
    auto s = make_state(fmt, key_len);
    auto enc_r = TlsEncryptor::create(s, key_len);
    if (!enc_r) {
        st.SkipWithError(enc_r.error().c_str());
        return;
    }

    const auto pt_len = static_cast<uint16_t>(st.range(0));
    std::vector<uint8_t> plaintext(pt_len, 0x55);
    std::vector<uint8_t> record(enc_r->encrypted_size(pt_len));

    for (auto _ : st) {
        const uint16_t w = enc_r->encrypt(plaintext.data(), pt_len, record.data());
        benchmark::DoNotOptimize(w);
    }
    st.SetBytesProcessed(int64_t(st.iterations()) * pt_len);
}

void roundtrip_loop(benchmark::State& st, TlsRecordFormat fmt, size_t key_len) {
    auto s = make_state(fmt, key_len);
    auto enc_r = TlsEncryptor::create(s, key_len);
    auto dec_r = TlsDecryptor::create(s, key_len);
    if (!enc_r || !dec_r) {
        st.SkipWithError("create failed");
        return;
    }

    const auto pt_len = static_cast<uint16_t>(st.range(0));
    std::vector<uint8_t> plaintext(pt_len, 0x55);
    std::vector<uint8_t> record(enc_r->encrypted_size(pt_len));
    std::vector<uint8_t> recovered(pt_len);

    for (auto _ : st) {
        const uint16_t w = enc_r->encrypt(plaintext.data(), pt_len, record.data());
        uint16_t out_len = 0;
        const bool ok = dec_r->decrypt(record.data(), w,
                                        recovered.data(), out_len);
        benchmark::DoNotOptimize(ok);
        benchmark::DoNotOptimize(out_len);
    }
    st.SetBytesProcessed(int64_t(st.iterations()) * pt_len);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────
// Encrypt only — capture the per-format cost of producing one record.
// ─────────────────────────────────────────────────────────────────────

static void BM_Tls13_Aes128_Encrypt(benchmark::State& st) {
    encrypt_loop(st, TlsRecordFormat::Tls13, 16);
}
BENCHMARK(BM_Tls13_Aes128_Encrypt)->Arg(32)->Arg(256)->Arg(1024);

static void BM_Tls13_Aes256_Encrypt(benchmark::State& st) {
    encrypt_loop(st, TlsRecordFormat::Tls13, 32);
}
BENCHMARK(BM_Tls13_Aes256_Encrypt)->Arg(32)->Arg(256)->Arg(1024);

static void BM_Tls12AesGcm_128_Encrypt(benchmark::State& st) {
    encrypt_loop(st, TlsRecordFormat::Tls12AesGcm, 16);
}
BENCHMARK(BM_Tls12AesGcm_128_Encrypt)->Arg(32)->Arg(256)->Arg(1024);

static void BM_Tls12AesGcm_256_Encrypt(benchmark::State& st) {
    encrypt_loop(st, TlsRecordFormat::Tls12AesGcm, 32);
}
BENCHMARK(BM_Tls12AesGcm_256_Encrypt)->Arg(32)->Arg(256)->Arg(1024);

static void BM_Tls12Chacha20_Encrypt(benchmark::State& st) {
    encrypt_loop(st, TlsRecordFormat::Tls12Chacha20, 32);
}
BENCHMARK(BM_Tls12Chacha20_Encrypt)->Arg(32)->Arg(256)->Arg(1024);

// ─────────────────────────────────────────────────────────────────────
// Roundtrip — encrypt then decrypt one record. Approximates the
// full inbound + outbound cost on a request-response style stream.
// ─────────────────────────────────────────────────────────────────────

static void BM_Tls13_Aes128_Roundtrip(benchmark::State& st) {
    roundtrip_loop(st, TlsRecordFormat::Tls13, 16);
}
BENCHMARK(BM_Tls13_Aes128_Roundtrip)->Arg(32)->Arg(256)->Arg(1024);

static void BM_Tls12AesGcm_128_Roundtrip(benchmark::State& st) {
    roundtrip_loop(st, TlsRecordFormat::Tls12AesGcm, 16);
}
BENCHMARK(BM_Tls12AesGcm_128_Roundtrip)->Arg(32)->Arg(256)->Arg(1024);

static void BM_Tls12Chacha20_Roundtrip(benchmark::State& st) {
    roundtrip_loop(st, TlsRecordFormat::Tls12Chacha20, 32);
}
BENCHMARK(BM_Tls12Chacha20_Roundtrip)->Arg(32)->Arg(256)->Arg(1024);

BENCHMARK_MAIN();
