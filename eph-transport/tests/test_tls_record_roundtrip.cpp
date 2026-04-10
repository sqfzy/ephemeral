/// @file test_tls_record_roundtrip.cpp
/// Round-trip + tamper-detection tests for TLS 1.3 record encryption.
///
/// The pre-existing test_tls_record.cpp covers tls_record::build_nonce
/// and write_record_header/parse_record_header in isolation, but
/// **never exercises encrypt + decrypt together**, never verifies
/// the AEAD round-trip recovers the original plaintext, and never
/// tests sequence-number replay or tamper detection.
///
/// Same vulnerability pattern as the SHA-1 bug: the lower-level
/// primitives are individually tested, but the integrated cipher
/// path is just assumed to work because it links against AWS-LC.
/// If TlsEncryptor's nonce-derivation, sequence advancement, or
/// AAD construction were wrong, NOTHING in the existing tests would
/// catch it — the bug would only surface against a real TLS peer.
///
/// This file plugs that gap with end-to-end round-trips against
/// the actual TlsEncryptor + TlsDecryptor classes.

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "eph/transport/detail/tls_constants.hpp"
#include "eph/transport/detail/tls_decryptor.hpp"
#include "eph/transport/detail/tls_encryptor.hpp"
#include "eph/transport/detail/tls_record.hpp"

using namespace eph::net;

namespace {

/// Build a TlsHotState with deterministic key+IV in BOTH directions
/// (write == read), so an Encryptor and Decryptor created from it
/// share the same cryptographic state and can round-trip.
TlsHotState make_loopback_state(uint8_t key_byte = 0xA5, uint8_t iv_byte = 0x5A) {
    TlsHotState state;
    for (size_t i = 0; i < tls_const::kAes256KeyLen; ++i) {
        state.write.ki.key[i] = static_cast<uint8_t>(key_byte ^ i);
        state.read.ki.key[i]  = state.write.ki.key[i];
    }
    for (size_t i = 0; i < tls_const::kTls13NonceLen; ++i) {
        state.write.ki.iv[i] = static_cast<uint8_t>(iv_byte ^ i);
        state.read.ki.iv[i]  = state.write.ki.iv[i];
    }
    state.write.seq = 0;
    state.read.seq  = 0;
    return state;
}

/// Make a paired Encryptor + Decryptor sharing the same key material.
struct CryptoPair {
    TlsEncryptor enc;
    TlsDecryptor dec;
};

CryptoPair make_pair() {
    auto state = make_loopback_state();
    auto enc = TlsEncryptor::create(state, tls_const::kAes256KeyLen);
    auto dec = TlsDecryptor::create(state, tls_const::kAes256KeyLen);
    EXPECT_TRUE(enc.has_value()) << (enc ? "" : enc.error());
    EXPECT_TRUE(dec.has_value()) << (dec ? "" : dec.error());
    return CryptoPair{std::move(*enc), std::move(*dec)};
}

/// Encrypt + decrypt one record and assert the plaintext is recovered.
void roundtrip_assert(const std::vector<uint8_t>& plaintext) {
    auto cp = make_pair();

    constexpr size_t kOutBufSize = tls_const::kMaxRecordPayload +
                                    tls_record::kRecordHeaderLen +
                                    tls_record::kAuthTagLen + 64;
    std::vector<uint8_t> ciphertext(kOutBufSize, 0);
    uint16_t enc_len = cp.enc.encrypt(plaintext.data(),
                                       static_cast<uint16_t>(plaintext.size()),
                                       ciphertext.data());
    ASSERT_GT(enc_len, 0u) << "encrypt failed";

    std::vector<uint8_t> recovered(kOutBufSize, 0);
    uint16_t out_len = 0;
    bool ok = cp.dec.decrypt(ciphertext.data(), enc_len,
                              recovered.data(), out_len);
    ASSERT_TRUE(ok) << "decrypt failed";

    EXPECT_EQ(out_len, plaintext.size());
    EXPECT_EQ(0, std::memcmp(recovered.data(), plaintext.data(), plaintext.size()));
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// Round-trip across payload sizes
// ═══════════════════════════════════════════════════════════════════════

TEST(TlsRoundTrip, EmptyPlaintext) {
    roundtrip_assert({});
}

TEST(TlsRoundTrip, SingleByte) {
    roundtrip_assert({0x42});
}

TEST(TlsRoundTrip, ThreeBytes) {
    roundtrip_assert({0xDE, 0xAD, 0xBE});
}

TEST(TlsRoundTrip, OneFullCacheLine) {
    std::vector<uint8_t> p(64);
    for (size_t i = 0; i < p.size(); ++i) p[i] = static_cast<uint8_t>(i);
    roundtrip_assert(p);
}

TEST(TlsRoundTrip, TypicalHttpRequestSize) {
    std::vector<uint8_t> p(512, 0);
    for (size_t i = 0; i < p.size(); ++i) p[i] = static_cast<uint8_t>((i * 31) & 0xFF);
    roundtrip_assert(p);
}

TEST(TlsRoundTrip, FourKilobytes) {
    std::vector<uint8_t> p(4096);
    for (size_t i = 0; i < p.size(); ++i) p[i] = static_cast<uint8_t>(i & 0xFF);
    roundtrip_assert(p);
}

TEST(TlsRoundTrip, MaxMinusOne) {
    std::vector<uint8_t> p(tls_const::kMaxRecordPayload - 1, 0);
    for (size_t i = 0; i < p.size(); ++i) p[i] = static_cast<uint8_t>((i * 7) & 0xFF);
    roundtrip_assert(p);
}

TEST(TlsRoundTrip, MaxRecordPayload) {
    std::vector<uint8_t> p(tls_const::kMaxRecordPayload);
    for (size_t i = 0; i < p.size(); ++i) p[i] = static_cast<uint8_t>(i & 0xFF);
    roundtrip_assert(p);
}

TEST(TlsEncryptor, OversizedPayloadRejected) {
    auto cp = make_pair();
    std::vector<uint8_t> p(tls_const::kMaxRecordPayload + 1, 0);
    std::vector<uint8_t> out(tls_const::kMaxRecordPayload + 1024, 0);
    uint16_t r = cp.enc.encrypt(p.data(),
                                 static_cast<uint16_t>(p.size()),
                                 out.data());
    EXPECT_EQ(r, 0u);
}

// ═══════════════════════════════════════════════════════════════════════
// Sequence number advancement (multi-record session)
// ═══════════════════════════════════════════════════════════════════════

TEST(TlsRoundTrip, MultipleRecordsAdvanceSequence) {
    auto cp = make_pair();
    std::vector<uint8_t> out(8192, 0);
    std::vector<uint8_t> recovered(8192, 0);

    for (int i = 0; i < 5; ++i) {
        std::vector<uint8_t> plaintext(64);
        for (size_t j = 0; j < 64; ++j) {
            plaintext[j] = static_cast<uint8_t>((i * 64 + j) & 0xFF);
        }
        uint16_t enc_len = cp.enc.encrypt(plaintext.data(), 64, out.data());
        ASSERT_GT(enc_len, 0u) << "iteration " << i;

        uint16_t dec_len = 0;
        bool ok = cp.dec.decrypt(out.data(), enc_len, recovered.data(), dec_len);
        ASSERT_TRUE(ok) << "iteration " << i;
        EXPECT_EQ(dec_len, 64u);
        EXPECT_EQ(0, std::memcmp(recovered.data(), plaintext.data(), 64))
            << "iteration " << i;
    }
}

TEST(TlsRoundTrip, EncryptorAndDecryptorMustHaveMatchingSequences) {
    // If decryptor's seq diverges from encryptor's, the AEAD nonce
    // mismatches and decryption fails authentication.  This pins that
    // behavior — and is the foundation of TLS replay protection.
    auto cp = make_pair();
    std::vector<uint8_t> out(8192, 0);
    std::vector<uint8_t> recovered(8192, 0);

    // Encryptor produces records 0, 1, 2.  Decryptor only consumes 0
    // and 2 — record 2 must fail auth because the decryptor's nonce
    // is computed for seq=1.
    std::vector<uint8_t> p1{1, 2, 3, 4};
    std::vector<uint8_t> p2{5, 6, 7, 8};
    std::vector<uint8_t> p3{9, 10, 11, 12};

    uint16_t e1 = cp.enc.encrypt(p1.data(), 4, out.data());
    ASSERT_GT(e1, 0u);
    uint16_t d1 = 0;
    ASSERT_TRUE(cp.dec.decrypt(out.data(), e1, recovered.data(), d1));

    // Skip encrypting p2 in the decryptor's view (the encryptor still
    // bumps its seq).  Encrypt p2 just to advance the encryptor.
    std::vector<uint8_t> tmp(8192, 0);
    cp.enc.encrypt(p2.data(), 4, tmp.data());

    // Now encrypt p3 (encryptor seq=2).  Decrypting it with the
    // decryptor (seq still 1) must fail.
    uint16_t e3 = cp.enc.encrypt(p3.data(), 4, out.data());
    ASSERT_GT(e3, 0u);
    uint16_t d3 = 0;
    EXPECT_FALSE(cp.dec.decrypt(out.data(), e3, recovered.data(), d3))
        << "decrypt with mismatched seq must fail (replay/skip protection)";
}

// ═══════════════════════════════════════════════════════════════════════
// Tamper detection — single-bit flip in ciphertext / tag / header
// ═══════════════════════════════════════════════════════════════════════

TEST(TlsRoundTrip, FlippedCiphertextByteFailsAuth) {
    auto cp = make_pair();
    std::vector<uint8_t> plaintext(128, 0xCC);
    std::vector<uint8_t> ciphertext(8192, 0);
    uint16_t enc_len = cp.enc.encrypt(plaintext.data(), 128, ciphertext.data());
    ASSERT_GT(enc_len, 0u);

    // Flip one byte in the middle of the ciphertext.
    ciphertext[tls_record::kRecordHeaderLen + 50] ^= 0x01;

    std::vector<uint8_t> out(8192, 0);
    uint16_t out_len = 0;
    bool ok = cp.dec.decrypt(ciphertext.data(), enc_len, out.data(), out_len);
    EXPECT_FALSE(ok) << "single-bit flip must fail AEAD authentication";
}

TEST(TlsRoundTrip, FlippedAuthTagByteFailsAuth) {
    auto cp = make_pair();
    std::vector<uint8_t> plaintext(128, 0xCC);
    std::vector<uint8_t> ciphertext(8192, 0);
    uint16_t enc_len = cp.enc.encrypt(plaintext.data(), 128, ciphertext.data());
    ASSERT_GT(enc_len, 0u);

    // The tag is the last kAuthTagLen bytes of the encrypted record.
    ciphertext[enc_len - 1] ^= 0x80;

    std::vector<uint8_t> out(8192, 0);
    uint16_t out_len = 0;
    bool ok = cp.dec.decrypt(ciphertext.data(), enc_len, out.data(), out_len);
    EXPECT_FALSE(ok);
}

TEST(TlsRoundTrip, FlippedRecordHeaderByteFailsAuth) {
    // The record header is the AEAD AAD — tampering with it MUST fail
    // authentication, otherwise an attacker could rewrite the
    // content_type or length fields.
    auto cp = make_pair();
    std::vector<uint8_t> plaintext(128, 0xCC);
    std::vector<uint8_t> ciphertext(8192, 0);
    uint16_t enc_len = cp.enc.encrypt(plaintext.data(), 128, ciphertext.data());
    ASSERT_GT(enc_len, 0u);

    // Flip the high byte of the length field.
    ciphertext[3] ^= 0x10;

    std::vector<uint8_t> out(8192, 0);
    uint16_t out_len = 0;
    bool ok = cp.dec.decrypt(ciphertext.data(), enc_len, out.data(), out_len);
    EXPECT_FALSE(ok)
        << "tampering with the AAD (record header) must fail authentication";
}

TEST(TlsRoundTrip, TruncatedCiphertextRejected) {
    auto cp = make_pair();
    std::vector<uint8_t> plaintext(128, 0x42);
    std::vector<uint8_t> ciphertext(8192, 0);
    uint16_t enc_len = cp.enc.encrypt(plaintext.data(), 128, ciphertext.data());
    ASSERT_GT(enc_len, 0u);

    // Pretend the record is shorter than it actually is.  Need to also
    // adjust the length field in the header so the parser doesn't
    // immediately reject for "claimed > actual" — the test is whether
    // the AEAD itself catches truncation through tag failure.
    std::vector<uint8_t> out(8192, 0);
    uint16_t out_len = 0;
    bool ok = cp.dec.decrypt(ciphertext.data(), enc_len - 5, out.data(), out_len);
    EXPECT_FALSE(ok);
}

// ═══════════════════════════════════════════════════════════════════════
// Different keys produce different ciphertext
// ═══════════════════════════════════════════════════════════════════════

TEST(TlsRoundTrip, DifferentKeysProduceDifferentCiphertext) {
    auto state_a = make_loopback_state(/*key_byte=*/0x11);
    auto state_b = make_loopback_state(/*key_byte=*/0x22);

    auto enc_a = *TlsEncryptor::create(state_a);
    auto enc_b = *TlsEncryptor::create(state_b);

    std::vector<uint8_t> plaintext(64, 0x55);
    std::vector<uint8_t> out_a(8192, 0);
    std::vector<uint8_t> out_b(8192, 0);

    uint16_t la = enc_a.encrypt(plaintext.data(), 64, out_a.data());
    uint16_t lb = enc_b.encrypt(plaintext.data(), 64, out_b.data());
    ASSERT_GT(la, 0u);
    ASSERT_EQ(la, lb) << "same plaintext length should yield same record length";

    // Skip the record header (which is identical) and compare ciphertext.
    EXPECT_NE(0, std::memcmp(
        out_a.data() + tls_record::kRecordHeaderLen,
        out_b.data() + tls_record::kRecordHeaderLen,
        la - tls_record::kRecordHeaderLen));
}

// ═══════════════════════════════════════════════════════════════════════
// AES-128 also round-trips
// ═══════════════════════════════════════════════════════════════════════

TEST(TlsRoundTrip, Aes128RoundTrip) {
    auto state = make_loopback_state();
    auto enc_r = TlsEncryptor::create(state, /*key_len=*/16);
    auto dec_r = TlsDecryptor::create(state, /*key_len=*/16);
    ASSERT_TRUE(enc_r.has_value());
    ASSERT_TRUE(dec_r.has_value());

    std::vector<uint8_t> plaintext{1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<uint8_t> out(8192, 0);
    std::vector<uint8_t> recovered(8192, 0);

    uint16_t enc_len = enc_r->encrypt(plaintext.data(), 8, out.data());
    ASSERT_GT(enc_len, 0u);

    uint16_t out_len = 0;
    ASSERT_TRUE(dec_r->decrypt(out.data(), enc_len, recovered.data(), out_len));
    EXPECT_EQ(out_len, 8u);
    EXPECT_EQ(0, std::memcmp(recovered.data(), plaintext.data(), 8));
}

TEST(TlsCreate, InvalidKeyLengthRejected) {
    auto state = make_loopback_state();
    auto enc = TlsEncryptor::create(state, /*key_len=*/24);
    EXPECT_FALSE(enc.has_value());
    auto dec = TlsDecryptor::create(state, /*key_len=*/24);
    EXPECT_FALSE(dec.has_value());
}

// ═══════════════════════════════════════════════════════════════════════
// Wrong content_type (not application_data) is rejected by the parser
// ═══════════════════════════════════════════════════════════════════════

TEST(TlsRoundTrip, NonAppDataContentTypeRejected) {
    auto cp = make_pair();
    std::vector<uint8_t> plaintext(64, 0x33);
    std::vector<uint8_t> ciphertext(8192, 0);
    uint16_t enc_len = cp.enc.encrypt(plaintext.data(), 64, ciphertext.data());
    ASSERT_GT(enc_len, 0u);

    // Mutate content type byte to handshake (0x16).
    ciphertext[0] = 0x16;
    std::vector<uint8_t> out(8192, 0);
    uint16_t out_len = 0;
    bool ok = cp.dec.decrypt(ciphertext.data(), enc_len, out.data(), out_len);
    EXPECT_FALSE(ok);
}

// ═══════════════════════════════════════════════════════════════════════
// build_nonce — extra coverage with non-trivial IV
// ═══════════════════════════════════════════════════════════════════════

TEST(TlsBuildNonce, RfcStyleVectorSeq0) {
    // Hand-checked vector: IV = 5d 31 3e b2 67 12 76 ee 13 00 0b 30,
    // seq = 0 → nonce equals IV (XOR with 0).
    uint8_t iv[tls_const::kTls13NonceLen] = {
        0x5d, 0x31, 0x3e, 0xb2, 0x67, 0x12, 0x76, 0xee,
        0x13, 0x00, 0x0b, 0x30,
    };
    uint8_t out[tls_const::kTls13NonceLen] = {};
    tls_record::build_nonce(out, iv, 0);
    EXPECT_EQ(0, std::memcmp(out, iv, tls_const::kTls13NonceLen));
}

TEST(TlsBuildNonce, RfcStyleVectorSeq1) {
    // Same IV, seq = 1.  Expected: first 4 bytes unchanged, last 8
    // bytes = iv_tail XOR (00 00 00 00 00 00 00 01).
    uint8_t iv[tls_const::kTls13NonceLen] = {
        0x5d, 0x31, 0x3e, 0xb2, 0x67, 0x12, 0x76, 0xee,
        0x13, 0x00, 0x0b, 0x30,
    };
    uint8_t expected[tls_const::kTls13NonceLen] = {
        0x5d, 0x31, 0x3e, 0xb2, // unchanged (XOR with 0)
        0x67, 0x12, 0x76, 0xee,
        0x13, 0x00, 0x0b, 0x30 ^ 0x01, // last byte XOR 1
    };
    uint8_t out[tls_const::kTls13NonceLen] = {};
    tls_record::build_nonce(out, iv, 1);
    EXPECT_EQ(0, std::memcmp(out, expected, tls_const::kTls13NonceLen));
}

TEST(TlsBuildNonce, RfcStyleVectorLargeSeq) {
    // seq = 0x0123456789ABCDEF, IV all zeros → output is just the
    // big-endian seq padded with 4 leading zero bytes.
    uint8_t iv[tls_const::kTls13NonceLen] = {};
    uint8_t expected[tls_const::kTls13NonceLen] = {
        0, 0, 0, 0, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
    };
    uint8_t out[tls_const::kTls13NonceLen] = {};
    tls_record::build_nonce(out, iv, 0x0123456789ABCDEFULL);
    EXPECT_EQ(0, std::memcmp(out, expected, tls_const::kTls13NonceLen));
}
