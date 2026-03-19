/// @file test_tls_record.cpp
/// Unit tests for TLS record layer encryption/decryption (aws-lc AEAD API).

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "eph/dpdk/tls_record.hpp"

using namespace eph::dpdk;

// ─────────────────────────────────────────────────────────────────────────────
// Compile-time validation
// ─────────────────────────────────────────────────────────────────────────────

static_assert(tls_const::kRecordHeaderLen == 5);
static_assert(tls_const::kAuthTagLen == 16);
static_assert(tls_const::kMaxRecordPayload == 16384);
static_assert(tls_const::kTls13NonceLen == 12);
static_assert(tls_const::kAes256KeyLen == 32);

static_assert(sizeof(TlsKeyMaterial) == 64, "Must fit one cache line");
static_assert(sizeof(TlsHotState) == 128, "Must fit two cache lines");

static_assert(TlsRecordCrypto::encrypted_size(0)   == 5 + 0 + 1 + 16);
static_assert(TlsRecordCrypto::encrypted_size(256) == 5 + 256 + 1 + 16);

// ─────────────────────────────────────────────────────────────────────────────
// Test helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

void fill_random(uint8_t* buf, size_t len, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<uint16_t> dist(0, 255);
    for (size_t i = 0; i < len; ++i) {
        buf[i] = static_cast<uint8_t>(dist(rng));
    }
}

/// Create a TlsHotState with deterministic random keys for testing.
TlsHotState make_test_state(uint32_t seed = 42) {
    TlsHotState state{};
    fill_random(state.write.key, tls_const::kAes256KeyLen, seed);
    fill_random(state.write.iv,  tls_const::kTls13NonceLen, seed + 1);
    fill_random(state.read.key,  tls_const::kAes256KeyLen, seed + 2);
    fill_random(state.read.iv,   tls_const::kTls13NonceLen, seed + 3);
    state.write.seq = 0;
    state.read.seq  = 0;
    return state;
}

/// Create a matched encrypt/decrypt pair (same keys for write and read).
TlsHotState make_roundtrip_state(uint32_t seed = 42) {
    TlsHotState state{};
    fill_random(state.write.key, tls_const::kAes256KeyLen, seed);
    fill_random(state.write.iv,  tls_const::kTls13NonceLen, seed + 1);
    // Read uses same key/IV as write — simulates decrypting our own output
    std::memcpy(state.read.key, state.write.key, tls_const::kAes256KeyLen);
    std::memcpy(state.read.iv,  state.write.iv,  tls_const::kTls13NonceLen);
    state.write.seq = 0;
    state.read.seq  = 0;
    return state;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// TlsRecordCrypto creation
// ─────────────────────────────────────────────────────────────────────────────

TEST(TlsRecord, CreateSucceeds) {
    auto state = make_test_state();
    auto result = TlsRecordCrypto::create(state);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->write_seq(), 0u);
    EXPECT_EQ(result->read_seq(), 0u);
}

TEST(TlsRecord, MoveConstruct) {
    auto state = make_test_state();
    auto result = TlsRecordCrypto::create(state);
    ASSERT_TRUE(result.has_value());

    TlsRecordCrypto moved = std::move(*result);
    EXPECT_EQ(moved.write_seq(), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Encrypt / Decrypt roundtrip
// ─────────────────────────────────────────────────────────────────────────────

class TlsRoundtrip : public ::testing::TestWithParam<uint16_t> {};

TEST_P(TlsRoundtrip, EncryptDecryptRoundtrip) {
    uint16_t payload_size = GetParam();

    auto state = make_roundtrip_state();

    // Need separate encryptor and decryptor (different seq counters)
    auto enc_result = TlsRecordCrypto::create(state);
    ASSERT_TRUE(enc_result.has_value()) << enc_result.error();

    // For decryptor, swap: read keys = write keys
    TlsHotState dec_state{};
    std::memcpy(dec_state.read.key, state.write.key, tls_const::kAes256KeyLen);
    std::memcpy(dec_state.read.iv,  state.write.iv,  tls_const::kTls13NonceLen);
    dec_state.read.seq = 0;

    auto dec_result = TlsRecordCrypto::create(dec_state);
    ASSERT_TRUE(dec_result.has_value()) << dec_result.error();

    // Prepare plaintext (+1 byte for encrypt's temporary content type append)
    std::vector<uint8_t> plaintext(payload_size + 1);
    fill_random(plaintext.data(), payload_size, 100);

    // Encrypt
    uint16_t enc_size = TlsRecordCrypto::encrypted_size(payload_size);
    std::vector<uint8_t> record(enc_size);
    uint16_t written = enc_result->encrypt(
        plaintext.data(), payload_size, record.data());
    ASSERT_GT(written, 0u);
    EXPECT_EQ(written, enc_size);

    // Verify TLS record header
    EXPECT_EQ(record[0], tls_record::kContentTypeAppData);

    // Decrypt
    std::vector<uint8_t> decrypted(payload_size + 16);
    uint16_t dec_len;
    bool ok = dec_result->decrypt(
        record.data(), written, decrypted.data(), dec_len);
    ASSERT_TRUE(ok);
    EXPECT_EQ(dec_len, payload_size);

    // Verify content matches
    EXPECT_EQ(std::memcmp(decrypted.data(), plaintext.data(), payload_size), 0);
}

INSTANTIATE_TEST_SUITE_P(
    PayloadSizes, TlsRoundtrip,
    ::testing::Values(0, 1, 15, 16, 64, 128, 256, 512, 1024, 1460, 4096));

// ─────────────────────────────────────────────────────────────────────────────
// Sequence number tracking
// ─────────────────────────────────────────────────────────────────────────────

TEST(TlsRecord, SequenceIncrements) {
    auto state = make_roundtrip_state();
    auto crypto = TlsRecordCrypto::create(state);
    ASSERT_TRUE(crypto.has_value());

    std::vector<uint8_t> plaintext(64 + 1, 0xAA);
    uint16_t enc_size = TlsRecordCrypto::encrypted_size(64);
    std::vector<uint8_t> record(enc_size);

    for (uint64_t i = 0; i < 10; ++i) {
        EXPECT_EQ(crypto->write_seq(), i);
        uint16_t written = crypto->encrypt(
            plaintext.data(), 64, record.data());
        EXPECT_GT(written, 0u);
    }
    EXPECT_EQ(crypto->write_seq(), 10u);
}

TEST(TlsRecord, MultipleRecordsDecryptInOrder) {
    auto state = make_roundtrip_state();

    auto enc = TlsRecordCrypto::create(state);
    ASSERT_TRUE(enc.has_value());

    TlsHotState dec_state{};
    std::memcpy(dec_state.read.key, state.write.key, tls_const::kAes256KeyLen);
    std::memcpy(dec_state.read.iv,  state.write.iv,  tls_const::kTls13NonceLen);
    auto dec = TlsRecordCrypto::create(dec_state);
    ASSERT_TRUE(dec.has_value());

    constexpr int kNumRecords = 50;
    uint16_t enc_size = TlsRecordCrypto::encrypted_size(32);

    for (int i = 0; i < kNumRecords; ++i) {
        std::vector<uint8_t> plaintext(33); // +1 for content type
        std::memset(plaintext.data(), static_cast<uint8_t>(i), 32);

        std::vector<uint8_t> record(enc_size);
        uint16_t written = enc->encrypt(
            plaintext.data(), 32, record.data());
        ASSERT_GT(written, 0u);

        std::vector<uint8_t> decrypted(48);
        uint16_t dec_len;
        bool ok = dec->decrypt(record.data(), written, decrypted.data(), dec_len);
        ASSERT_TRUE(ok) << "Failed at record " << i;
        ASSERT_EQ(dec_len, 32);

        // Verify content
        for (int j = 0; j < 32; ++j) {
            EXPECT_EQ(decrypted[j], static_cast<uint8_t>(i))
                << "Mismatch at record " << i << " byte " << j;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Error cases
// ─────────────────────────────────────────────────────────────────────────────

TEST(TlsRecord, DecryptTamperedRecordFails) {
    auto state = make_roundtrip_state();

    auto enc = TlsRecordCrypto::create(state);
    ASSERT_TRUE(enc.has_value());

    TlsHotState dec_state{};
    std::memcpy(dec_state.read.key, state.write.key, tls_const::kAes256KeyLen);
    std::memcpy(dec_state.read.iv,  state.write.iv,  tls_const::kTls13NonceLen);
    auto dec = TlsRecordCrypto::create(dec_state);
    ASSERT_TRUE(dec.has_value());

    std::vector<uint8_t> plaintext(65, 0x42);
    uint16_t enc_size = TlsRecordCrypto::encrypted_size(64);
    std::vector<uint8_t> record(enc_size);
    uint16_t written = enc->encrypt(plaintext.data(), 64, record.data());
    ASSERT_GT(written, 0u);

    // Tamper with ciphertext (flip a bit in the middle)
    record[tls_record::kRecordHeaderLen + 10] ^= 0x01;

    std::vector<uint8_t> decrypted(80);
    uint16_t dec_len;
    bool ok = dec->decrypt(record.data(), written, decrypted.data(), dec_len);
    EXPECT_FALSE(ok) << "Tampered record should fail authentication";
}

TEST(TlsRecord, DecryptTruncatedRecordFails) {
    uint8_t record[10] = {};
    uint16_t dec_len;
    uint8_t out[64];

    auto state = make_test_state();
    auto crypto = TlsRecordCrypto::create(state);
    ASSERT_TRUE(crypto.has_value());

    // Too short (< header + tag)
    bool ok = crypto->decrypt(record, 10, out, dec_len);
    EXPECT_FALSE(ok);
}

TEST(TlsRecord, EncryptOversizedPayloadReturnsZero) {
    auto state = make_test_state();
    auto crypto = TlsRecordCrypto::create(state);
    ASSERT_TRUE(crypto.has_value());

    // payload_len > kMaxRecordPayload
    std::vector<uint8_t> big(tls_const::kMaxRecordPayload + 2);
    std::vector<uint8_t> out(tls_const::kMaxRecordPayload + 100);
    uint16_t written = crypto->encrypt(
        big.data(), tls_const::kMaxRecordPayload + 1, out.data());
    EXPECT_EQ(written, 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Nonce construction
// ─────────────────────────────────────────────────────────────────────────────

TEST(TlsRecord, NonceChangesWithSequence) {
    uint8_t iv[12];
    fill_random(iv, 12, 99);

    uint8_t nonce0[12], nonce1[12], nonce2[12];
    tls_record::build_nonce(nonce0, iv, 0);
    tls_record::build_nonce(nonce1, iv, 1);
    tls_record::build_nonce(nonce2, iv, 2);

    // First 4 bytes are identical (IV prefix)
    EXPECT_EQ(std::memcmp(nonce0, nonce1, 4), 0);
    EXPECT_EQ(std::memcmp(nonce1, nonce2, 4), 0);

    // Full nonces differ
    EXPECT_NE(std::memcmp(nonce0, nonce1, 12), 0);
    EXPECT_NE(std::memcmp(nonce1, nonce2, 12), 0);
    EXPECT_NE(std::memcmp(nonce0, nonce2, 12), 0);
}

TEST(TlsRecord, NonceAtSequenceZeroEqualsIv) {
    uint8_t iv[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    uint8_t nonce[12];
    tls_record::build_nonce(nonce, iv, 0);

    // nonce = iv XOR 0 = iv
    EXPECT_EQ(std::memcmp(nonce, iv, 12), 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Record header parse/write roundtrip
// ─────────────────────────────────────────────────────────────────────────────

TEST(TlsRecord, RecordHeaderRoundtrip) {
    uint8_t buf[5];
    tls_record::write_record_header(buf, 0x17, 300);

    uint8_t ct;
    uint16_t len;
    bool ok = tls_record::parse_record_header(buf, ct, len);
    EXPECT_TRUE(ok);
    EXPECT_EQ(ct, 0x17);
    EXPECT_EQ(len, 300);
}

TEST(TlsRecord, RecordHeaderRejectsOversized) {
    uint8_t buf[5];
    uint16_t huge = tls_const::kMaxRecordPayload + tls_record::kAuthTagLen + 2;
    tls_record::write_record_header(buf, 0x17, huge);

    uint8_t ct;
    uint16_t len;
    bool ok = tls_record::parse_record_header(buf, ct, len);
    EXPECT_FALSE(ok);
}

TEST(TlsRecord, RecordHeaderRejectsWrongContentType) {
    uint8_t buf[5];
    // content_type = 0x14 (ChangeCipherSpec) — not application_data
    tls_record::write_record_header(buf, 0x14, 100);

    uint8_t ct;
    uint16_t len;
    bool ok = tls_record::parse_record_header(buf, ct, len);
    EXPECT_FALSE(ok) << "Non-application_data content type should be rejected";
}
