/// @file test_tls_record.cpp
/// Unit tests for TLS record layer encryption/decryption (aws-lc AEAD API).

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "eph/transport/tls_record.hpp"

using namespace eph::net;

// ─────────────────────────────────────────────────────────────────────────────
// Compile-time validation
// ─────────────────────────────────────────────────────────────────────────────

static_assert(tls_const::kRecordHeaderLen == 5);
static_assert(tls_const::kAuthTagLen == 16);
static_assert(tls_const::kMaxRecordPayload == 16384);
static_assert(tls_const::kTls13NonceLen == 12);
static_assert(tls_const::kAes256KeyLen == 32);

static_assert(sizeof(TlsKeyMaterial) == 128, "Must fit two cache lines");
static_assert(sizeof(TlsHotState) == 256, "Must fit four cache lines");

// Sequence number threshold ordering: warn < reconnect < max
static_assert(tls_record::kSequenceWarnThreshold < tls_record::kSequenceReconnectThreshold);
static_assert(tls_record::kSequenceReconnectThreshold < tls_record::kMaxSequenceNumber);
// Warn at 90%, reconnect at 95%
static_assert(tls_record::kSequenceWarnThreshold == tls_record::kMaxSequenceNumber * 9 / 10);
static_assert(tls_record::kSequenceReconnectThreshold == tls_record::kMaxSequenceNumber * 95 / 100);

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
    fill_random(state.write.ki.key, tls_const::kAes256KeyLen, seed);
    fill_random(state.write.ki.iv,  tls_const::kTls13NonceLen, seed + 1);
    fill_random(state.read.ki.key,  tls_const::kAes256KeyLen, seed + 2);
    fill_random(state.read.ki.iv,   tls_const::kTls13NonceLen, seed + 3);
    state.write.seq = 0;
    state.read.seq  = 0;
    return state;
}

/// Create a matched encrypt/decrypt pair (same keys for write and read).
TlsHotState make_roundtrip_state(uint32_t seed = 42) {
    TlsHotState state{};
    fill_random(state.write.ki.key, tls_const::kAes256KeyLen, seed);
    fill_random(state.write.ki.iv,  tls_const::kTls13NonceLen, seed + 1);
    // Read uses same key/IV as write — simulates decrypting our own output
    std::memcpy(state.read.ki.key, state.write.ki.key, tls_const::kAes256KeyLen);
    std::memcpy(state.read.ki.iv,  state.write.ki.iv,  tls_const::kTls13NonceLen);
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
    std::memcpy(dec_state.read.ki.key, state.write.ki.key, tls_const::kAes256KeyLen);
    std::memcpy(dec_state.read.ki.iv,  state.write.ki.iv,  tls_const::kTls13NonceLen);
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
    std::memcpy(dec_state.read.ki.key, state.write.ki.key, tls_const::kAes256KeyLen);
    std::memcpy(dec_state.read.ki.iv,  state.write.ki.iv,  tls_const::kTls13NonceLen);
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
    std::memcpy(dec_state.read.ki.key, state.write.ki.key, tls_const::kAes256KeyLen);
    std::memcpy(dec_state.read.ki.iv,  state.write.ki.iv,  tls_const::kTls13NonceLen);
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

TEST(TlsRecord, DecryptPayloadSmallerThanAuthTagFails) {
    // Craft a record header that claims a payload_len smaller than the
    // auth tag (16 bytes). Without the bounds check, this would underflow
    // the max_out_len computation in EVP_AEAD_CTX_open.
    auto state = make_test_state();
    auto crypto = TlsRecordCrypto::create(state);
    ASSERT_TRUE(crypto.has_value());

    // Build a fake record: valid header but payload_len = 5 (< 17 = tag + content type)
    uint8_t record[64] = {};
    record[0] = 0x17; // kContentTypeAppData
    record[1] = 0x03;
    record[2] = 0x03;
    record[3] = 0x00;
    record[4] = 0x05; // payload_len = 5, less than kAuthTagLen + 1

    uint8_t out[64];
    uint16_t dec_len;
    bool ok = crypto->decrypt(record, 64, out, dec_len);
    EXPECT_FALSE(ok) << "Decrypting record with payload_len < auth_tag + 1 must fail";
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
    tls_record::write_record_header(buf, 0x14, 100);

    uint8_t ct;
    uint16_t len;
    bool ok = tls_record::parse_record_header(buf, ct, len);
    EXPECT_FALSE(ok) << "Non-application_data content type should be rejected";
}

// ─────────────────────────────────────────────────────────────────────────────
// Boundary tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(TlsRecord, RecordHeaderBoundaryLength) {
    uint8_t buf[5];
    // Exactly at max allowed: kMaxRecordPayload + kAuthTagLen + 1
    uint16_t max_ok = tls_const::kMaxRecordPayload + tls_record::kAuthTagLen + 1;
    tls_record::write_record_header(buf, 0x17, max_ok);

    uint8_t ct;
    uint16_t len;
    EXPECT_TRUE(tls_record::parse_record_header(buf, ct, len));
    EXPECT_EQ(len, max_ok);

    // One byte over max
    tls_record::write_record_header(buf, 0x17, max_ok + 1);
    EXPECT_FALSE(tls_record::parse_record_header(buf, ct, len));
}

TEST(TlsRecord, EncryptedSizeFormula) {
    // encrypted_size(n) = 5 + n + 1 + 16
    EXPECT_EQ(TlsRecordCrypto::encrypted_size(0),     22);
    EXPECT_EQ(TlsRecordCrypto::encrypted_size(1),     23);
    EXPECT_EQ(TlsRecordCrypto::encrypted_size(100),  122);
    EXPECT_EQ(TlsRecordCrypto::encrypted_size(16384), 16406);
}

TEST(TlsRecord, EncryptPreservesPlaintextBuffer) {
    auto state = make_roundtrip_state();
    auto crypto = TlsRecordCrypto::create(state);
    ASSERT_TRUE(crypto.has_value());

    // Fill plaintext with known pattern + sentinel byte at end
    std::vector<uint8_t> plaintext(65, 0);
    for (int i = 0; i < 64; ++i) plaintext[i] = static_cast<uint8_t>(i);
    plaintext[64] = 0xFE; // sentinel (the +1 byte that encrypt temporarily modifies)

    std::vector<uint8_t> record(TlsRecordCrypto::encrypted_size(64));
    uint16_t written = crypto->encrypt(plaintext.data(), 64, record.data());
    ASSERT_GT(written, 0u);

    // Verify sentinel byte was restored
    EXPECT_EQ(plaintext[64], 0xFE)
        << "encrypt() must restore the byte it temporarily overwrites";
    // Verify plaintext wasn't corrupted
    for (int i = 0; i < 64; ++i) {
        EXPECT_EQ(plaintext[i], static_cast<uint8_t>(i)) << "at index " << i;
    }
}

TEST(TlsRecord, DecryptWithWrongKeyFails) {
    auto state1 = make_roundtrip_state(42);
    auto enc = TlsRecordCrypto::create(state1);
    ASSERT_TRUE(enc.has_value());

    std::vector<uint8_t> plaintext(65, 0x42);
    uint16_t enc_size = TlsRecordCrypto::encrypted_size(64);
    std::vector<uint8_t> record(enc_size);
    uint16_t written = enc->encrypt(plaintext.data(), 64, record.data());
    ASSERT_GT(written, 0u);

    // Create decryptor with DIFFERENT keys
    auto state2 = make_test_state(999);
    auto dec = TlsRecordCrypto::create(state2);
    ASSERT_TRUE(dec.has_value());

    std::vector<uint8_t> out(80);
    uint16_t dec_len;
    bool ok = dec->decrypt(record.data(), written, out.data(), dec_len);
    EXPECT_FALSE(ok) << "Decrypting with wrong key should fail authentication";
}

TEST(TlsRecord, CreateWithAes128KeyLength) {
    // AES-128 uses 16-byte key
    TlsHotState state{};
    fill_random(state.write.ki.key, 16, 42);
    fill_random(state.write.ki.iv, tls_const::kTls13NonceLen, 43);
    fill_random(state.read.ki.key, 16, 44);
    fill_random(state.read.ki.iv, tls_const::kTls13NonceLen, 45);

    auto crypto = TlsRecordCrypto::create(state, 16);
    ASSERT_TRUE(crypto.has_value()) << crypto.error();

    // Encrypt + decrypt roundtrip
    std::vector<uint8_t> plaintext(33, 0xAA);
    uint16_t enc_size = TlsRecordCrypto::encrypted_size(32);
    std::vector<uint8_t> record(enc_size);
    uint16_t written = crypto->encrypt(plaintext.data(), 32, record.data());
    ASSERT_GT(written, 0u);

    // Decrypt with same key (AES-128)
    TlsHotState dec_state{};
    std::memcpy(dec_state.read.ki.key, state.write.ki.key, 16);
    std::memcpy(dec_state.read.ki.iv, state.write.ki.iv, tls_const::kTls13NonceLen);
    auto dec = TlsRecordCrypto::create(dec_state, 16);
    ASSERT_TRUE(dec.has_value());

    std::vector<uint8_t> out(48);
    uint16_t dec_len;
    bool ok = dec->decrypt(record.data(), written, out.data(), dec_len);
    ASSERT_TRUE(ok);
    EXPECT_EQ(dec_len, 32);
}

TEST(TlsRecord, CreateWithInvalidKeyLengthFails) {
    TlsHotState state{};
    auto result = TlsRecordCrypto::create(state, 24); // Not 16 or 32
    EXPECT_FALSE(result.has_value());
}

TEST(TlsRecord, DecryptWithWrongSeqFails) {
    auto state = make_roundtrip_state();

    auto enc = TlsRecordCrypto::create(state);
    ASSERT_TRUE(enc.has_value());

    std::vector<uint8_t> pt(33, 0x55);
    uint16_t enc_size = TlsRecordCrypto::encrypted_size(32);
    std::vector<uint8_t> record(enc_size);
    uint16_t written = enc->encrypt(pt.data(), 32, record.data());
    ASSERT_GT(written, 0u);

    // Decryptor with WRONG initial seq (1 instead of 0)
    TlsHotState dec_state{};
    std::memcpy(dec_state.read.ki.key, state.write.ki.key, tls_const::kAes256KeyLen);
    std::memcpy(dec_state.read.ki.iv,  state.write.ki.iv,  tls_const::kTls13NonceLen);
    dec_state.read.seq = 1; // Mismatch: enc used seq=0

    auto dec = TlsRecordCrypto::create(dec_state);
    ASSERT_TRUE(dec.has_value());

    uint8_t out[48]; uint16_t dec_len;
    bool ok = dec->decrypt(record.data(), written, out, dec_len);
    EXPECT_FALSE(ok) << "Decrypt with wrong sequence number should fail (nonce mismatch)";
}

// Regression: max_out_len must not exceed output buffer capacity.
// Previously max_out_len included the 16-byte auth tag, causing potential
// buffer overrun on the decryption failure path when out buffer was tight.
TEST(TlsRecord, DecryptFailureDoesNotOverrunTightBuffer) {
    auto state = make_roundtrip_state();
    auto enc = TlsRecordCrypto::create(state);
    ASSERT_TRUE(enc.has_value());

    constexpr uint16_t kPlainLen = 32;
    std::vector<uint8_t> pt(kPlainLen + 1, 0x55);
    uint16_t enc_size = TlsRecordCrypto::encrypted_size(kPlainLen);
    std::vector<uint8_t> record(enc_size);
    uint16_t written = enc->encrypt(pt.data(), kPlainLen, record.data());
    ASSERT_GT(written, 0u);

    // Decryptor with wrong key — forces decryption failure
    auto bad_state = make_test_state(999);
    auto dec = TlsRecordCrypto::create(bad_state);
    ASSERT_TRUE(dec.has_value());

    // Output buffer sized exactly for plaintext + content type (no room for tag).
    // Before the fix, max_out_len = payload_len (49) would exceed this buffer (33).
    constexpr uint16_t kOutSize = kPlainLen + 1; // plaintext + content type byte
    uint8_t out[kOutSize];
    uint16_t dec_len;
    bool ok = dec->decrypt(record.data(), written, out, dec_len);
    EXPECT_FALSE(ok) << "Decryption with wrong key must fail without overrunning buffer";
}

TEST(TlsRecord, NonZeroInitialSequence) {
    auto state = make_roundtrip_state();
    state.write.seq = 100;
    state.read.seq  = 200;

    auto enc = TlsRecordCrypto::create(state);
    ASSERT_TRUE(enc.has_value());

    EXPECT_EQ(enc->write_seq(), 100u);
    EXPECT_EQ(enc->read_seq(), 200u);

    // Encrypt one record — seq should advance
    std::vector<uint8_t> pt(17, 0);
    std::vector<uint8_t> rec(TlsRecordCrypto::encrypted_size(16));
    enc->encrypt(pt.data(), 16, rec.data());
    EXPECT_EQ(enc->write_seq(), 101u);

    // Decryptor with matching seq can decrypt
    TlsHotState dec_state{};
    std::memcpy(dec_state.read.ki.key, state.write.ki.key, tls_const::kAes256KeyLen);
    std::memcpy(dec_state.read.ki.iv, state.write.ki.iv, tls_const::kTls13NonceLen);
    dec_state.read.seq = 100; // Must match encrypt's initial seq
    auto dec = TlsRecordCrypto::create(dec_state);
    ASSERT_TRUE(dec.has_value());

    uint16_t dec_len;
    uint8_t out[32];
    bool ok = dec->decrypt(rec.data(), TlsRecordCrypto::encrypted_size(16),
                            out, dec_len);
    EXPECT_TRUE(ok) << "Decryption with matching non-zero initial seq should work";
    EXPECT_EQ(dec_len, 16);
}

// ─────────────────────────────────────────────────────────────────────────────
// Sequence number edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST(TlsRecord, NonceAtMaxSequenceNumber) {
    uint8_t iv[12];
    fill_random(iv, 12, 77);

    uint8_t nonce_max[12], nonce_max_minus1[12];
    tls_record::build_nonce(nonce_max, iv, UINT64_MAX);
    tls_record::build_nonce(nonce_max_minus1, iv, UINT64_MAX - 1);

    // Nonces should differ
    EXPECT_NE(std::memcmp(nonce_max, nonce_max_minus1, 12), 0);

    // First 4 bytes unchanged (seq only affects last 8)
    EXPECT_EQ(std::memcmp(nonce_max, iv, 4), 0);
}

TEST(TlsRecord, EncryptDecryptAtHighSequence) {
    // Test near (but below) the kMaxSequenceNumber limit
    auto state = make_roundtrip_state();
    state.write.seq = tls_record::kMaxSequenceNumber - 3;
    state.read.seq  = tls_record::kMaxSequenceNumber - 3;

    auto enc = TlsRecordCrypto::create(state);
    ASSERT_TRUE(enc.has_value());

    TlsHotState dec_state{};
    std::memcpy(dec_state.read.ki.key, state.write.ki.key, tls_const::kAes256KeyLen);
    std::memcpy(dec_state.read.ki.iv,  state.write.ki.iv,  tls_const::kTls13NonceLen);
    dec_state.read.seq = tls_record::kMaxSequenceNumber - 3;
    auto dec = TlsRecordCrypto::create(dec_state);
    ASSERT_TRUE(dec.has_value());

    // Encrypt 3 records near the sequence limit
    for (int i = 0; i < 3; ++i) {
        std::vector<uint8_t> pt(33, static_cast<uint8_t>(i));
        uint16_t enc_size = TlsRecordCrypto::encrypted_size(32);
        std::vector<uint8_t> record(enc_size);

        uint16_t written = enc->encrypt(pt.data(), 32, record.data());
        ASSERT_GT(written, 0u) << "Failed at record " << i;

        std::vector<uint8_t> out(48);
        uint16_t dec_len;
        bool ok = dec->decrypt(record.data(), written, out.data(), dec_len);
        ASSERT_TRUE(ok) << "Decrypt failed at record " << i;
        EXPECT_EQ(dec_len, 32);
    }

    // The 4th encrypt should fail — seq == kMaxSequenceNumber
    std::vector<uint8_t> pt(33, 0xFF);
    uint16_t enc_size = TlsRecordCrypto::encrypted_size(32);
    std::vector<uint8_t> record(enc_size);
    uint16_t written = enc->encrypt(pt.data(), 32, record.data());
    EXPECT_EQ(written, 0u) << "Should fail at sequence limit";
}

TEST(TlsRecord, EncryptExactlyAtMaxSequenceReturnsZero) {
    // Verify that encrypt at exactly kMaxSequenceNumber fails immediately
    auto state = make_roundtrip_state();
    state.write.seq = tls_record::kMaxSequenceNumber;
    auto enc = TlsRecordCrypto::create(state);
    ASSERT_TRUE(enc.has_value());

    std::vector<uint8_t> pt(17, 0xAA);
    std::vector<uint8_t> record(TlsRecordCrypto::encrypted_size(16));
    uint16_t written = enc->encrypt(pt.data(), 16, record.data());
    EXPECT_EQ(written, 0u)
        << "Encrypt must fail when write_seq == kMaxSequenceNumber";
    // Sequence should not advance on failure
    EXPECT_EQ(enc->write_seq(), tls_record::kMaxSequenceNumber);
}

TEST(TlsRecord, DecryptExactlyAtMaxSequenceReturnsZero) {
    // Encrypt a valid record at seq=0, then try to decrypt with read_seq at limit
    auto state = make_roundtrip_state();
    auto enc = TlsRecordCrypto::create(state);
    ASSERT_TRUE(enc.has_value());

    std::vector<uint8_t> pt(17, 0xBB);
    std::vector<uint8_t> record(TlsRecordCrypto::encrypted_size(16));
    uint16_t written = enc->encrypt(pt.data(), 16, record.data());
    ASSERT_GT(written, 0u);

    TlsHotState dec_state{};
    std::memcpy(dec_state.read.ki.key, state.write.ki.key, tls_const::kAes256KeyLen);
    std::memcpy(dec_state.read.ki.iv,  state.write.ki.iv,  tls_const::kTls13NonceLen);
    dec_state.read.seq = tls_record::kMaxSequenceNumber;
    auto dec = TlsRecordCrypto::create(dec_state);
    ASSERT_TRUE(dec.has_value());

    std::vector<uint8_t> out(32);
    uint16_t dec_len;
    bool ok = dec->decrypt(record.data(), written, out.data(), dec_len);
    EXPECT_FALSE(ok) << "Decrypt must fail when read_seq == kMaxSequenceNumber";
    // Sequence should not advance on failure
    EXPECT_EQ(dec->read_seq(), tls_record::kMaxSequenceNumber);
}

TEST(TlsRecord, LastValidEncryptSucceeds) {
    // The last valid encrypt is at write_seq == kMaxSequenceNumber - 1
    auto state = make_roundtrip_state();
    state.write.seq = tls_record::kMaxSequenceNumber - 1;
    auto enc = TlsRecordCrypto::create(state);
    ASSERT_TRUE(enc.has_value());

    std::vector<uint8_t> pt(17, 0xCC);
    std::vector<uint8_t> record(TlsRecordCrypto::encrypted_size(16));
    uint16_t written = enc->encrypt(pt.data(), 16, record.data());
    EXPECT_GT(written, 0u) << "Last valid encrypt must succeed";
    EXPECT_EQ(enc->write_seq(), tls_record::kMaxSequenceNumber)
        << "Sequence should advance to kMaxSequenceNumber after last valid encrypt";

    // Next encrypt should now fail
    std::vector<uint8_t> record2(TlsRecordCrypto::encrypted_size(16));
    uint16_t written2 = enc->encrypt(pt.data(), 16, record2.data());
    EXPECT_EQ(written2, 0u) << "Encrypt past limit must fail";
}

TEST(TlsRecord, SequenceCounterIncrements) {
    // Verify write_seq and read_seq increment correctly after each operation
    auto state = make_roundtrip_state();
    auto crypto = TlsRecordCrypto::create(state);
    ASSERT_TRUE(crypto.has_value());

    EXPECT_EQ(crypto->write_seq(), 0u);
    EXPECT_EQ(crypto->read_seq(), 0u);

    // Encrypt 5 records, verify write_seq increments
    for (uint64_t i = 0; i < 5; ++i) {
        std::vector<uint8_t> pt(17, static_cast<uint8_t>(i));
        std::vector<uint8_t> record(TlsRecordCrypto::encrypted_size(16));
        uint16_t written = crypto->encrypt(pt.data(), 16, record.data());
        ASSERT_GT(written, 0u);
        EXPECT_EQ(crypto->write_seq(), i + 1);
    }
    EXPECT_EQ(crypto->read_seq(), 0u) << "Read seq should not change from encrypts";
}

TEST(TlsRecord, MoveAssign) {
    auto state = make_test_state();
    auto result1 = TlsRecordCrypto::create(state);
    ASSERT_TRUE(result1.has_value());

    auto state2 = make_test_state(99);
    auto result2 = TlsRecordCrypto::create(state2);
    ASSERT_TRUE(result2.has_value());

    // Move assign
    *result1 = std::move(*result2);

    // result1 should now have result2's seq counters
    EXPECT_EQ(result1->write_seq(), 0u);
    EXPECT_EQ(result1->read_seq(), 0u);
}

TEST(TlsRecord, DecryptCorruptedHeader) {
    auto state = make_roundtrip_state();
    auto enc = TlsRecordCrypto::create(state);
    ASSERT_TRUE(enc.has_value());

    std::vector<uint8_t> pt(33, 0x42);
    uint16_t enc_size = TlsRecordCrypto::encrypted_size(32);
    std::vector<uint8_t> record(enc_size);
    uint16_t written = enc->encrypt(pt.data(), 32, record.data());
    ASSERT_GT(written, 0u);

    TlsHotState dec_state{};
    std::memcpy(dec_state.read.ki.key, state.write.ki.key, tls_const::kAes256KeyLen);
    std::memcpy(dec_state.read.ki.iv,  state.write.ki.iv,  tls_const::kTls13NonceLen);
    auto dec = TlsRecordCrypto::create(dec_state);
    ASSERT_TRUE(dec.has_value());

    // Corrupt content type (change from 0x17 to 0x14)
    record[0] = 0x14;
    uint8_t out[64]; uint16_t dec_len;
    bool ok = dec->decrypt(record.data(), written, out, dec_len);
    EXPECT_FALSE(ok) << "Corrupted content type should fail";
}

// ─────────────────────────────────────────────────────────────────────────────
// TlsConfig::validate()
// ─────────────────────────────────────────────────────────────────────────────

TEST(TlsConfig, ValidateDefaultConfigPasses) {
    TlsConfig cfg{};
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(TlsConfig, ValidateZeroHandshakeTimeoutFails) {
    TlsConfig cfg{.handshake_timeout = std::chrono::milliseconds{0}};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("handshake_timeout"), std::string_view::npos);
}

TEST(TlsConfig, ValidateNegativeHandshakeTimeoutFails) {
    TlsConfig cfg{.handshake_timeout = std::chrono::milliseconds{-1}};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
}

TEST(TlsConfig, ValidateBothCertAndKeySetPasses) {
    TlsConfig cfg{.client_cert_path = "/cert.pem", .client_key_path = "/key.pem"};
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(TlsConfig, ValidateCertWithoutKeyFails) {
    TlsConfig cfg{.client_cert_path = "/cert.pem"};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("client_cert_path"), std::string_view::npos);
}

TEST(TlsConfig, ValidateKeyWithoutCertFails) {
    TlsConfig cfg{.client_key_path = "/key.pem"};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
}

// TlsConfig observability operators

TEST(TlsConfig, EqualityDefaultBehavior) {
    TlsConfig a{};
    TlsConfig b{};
    EXPECT_EQ(a, b);
}

TEST(TlsConfig, EqualityDetectsDifference) {
    TlsConfig a{.hostname = "host1"};
    TlsConfig b{.hostname = "host2"};
    EXPECT_NE(a, b);
}

TEST(TlsConfig, ToJsonContainsAllFields) {
    TlsConfig cfg{
        .hostname = "example.com",
        .ca_cert_path = "/ca.pem",
        .verify_peer = false,
        .handshake_timeout = std::chrono::milliseconds{3000},
        .client_cert_path = "/client.pem",
        .client_key_path = "/key.pem",
    };
    auto json = cfg.to_json();
    EXPECT_NE(json.find("\"hostname\":\"example.com\""), std::string::npos);
    EXPECT_NE(json.find("\"ca_cert_path\":\"/ca.pem\""), std::string::npos);
    EXPECT_NE(json.find("\"verify_peer\":false"), std::string::npos);
    EXPECT_NE(json.find("\"handshake_timeout_ms\":3000"), std::string::npos);
    EXPECT_NE(json.find("\"client_cert_path\":\"/client.pem\""), std::string::npos);
    EXPECT_NE(json.find("\"client_key_path\":\"/key.pem\""), std::string::npos);
}

TEST(TlsConfig, DumpContainsHostname) {
    TlsConfig cfg{.hostname = "example.com"};
    auto d = cfg.dump();
    EXPECT_NE(d.find("example.com"), std::string::npos);
    EXPECT_NE(d.find("TlsConfig"), std::string::npos);
}

TEST(TlsConfig, FormatterProducesNonEmpty) {
    TlsConfig cfg{.hostname = "test.local"};
    auto s = std::format("{}", cfg);
    EXPECT_FALSE(s.empty());
    EXPECT_NE(s.find("test.local"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// TLS sequence warn threshold and limit constants
// ─────────────────────────────────────────────────────────────────────────────

TEST(TlsRecord, SequenceWarnThresholdIs90Percent) {
    // Verify the warn threshold is approximately 90% of the max
    double ratio = static_cast<double>(tls_record::kSequenceWarnThreshold) /
                   static_cast<double>(tls_record::kMaxSequenceNumber);
    EXPECT_GE(ratio, 0.89);
    EXPECT_LE(ratio, 0.91);
    EXPECT_LT(tls_record::kSequenceWarnThreshold, tls_record::kMaxSequenceNumber);
}

TEST(TlsRecord, EncryptAtWarnThresholdStillSucceeds) {
    // Encryption at the warn threshold should succeed (only kMaxSequenceNumber fails)
    auto state = make_roundtrip_state();
    state.write.seq = tls_record::kSequenceWarnThreshold;
    state.read.seq  = tls_record::kSequenceWarnThreshold;

    auto enc = TlsRecordCrypto::create(state);
    ASSERT_TRUE(enc.has_value());

    TlsHotState dec_state{};
    std::memcpy(dec_state.read.ki.key, state.write.ki.key, tls_const::kAes256KeyLen);
    std::memcpy(dec_state.read.ki.iv,  state.write.ki.iv,  tls_const::kTls13NonceLen);
    dec_state.read.seq = tls_record::kSequenceWarnThreshold;
    auto dec = TlsRecordCrypto::create(dec_state);
    ASSERT_TRUE(dec.has_value());

    // Encrypt should succeed (warn threshold is not the hard limit)
    std::vector<uint8_t> pt(17, 0xAB);
    uint16_t enc_size = TlsRecordCrypto::encrypted_size(16);
    std::vector<uint8_t> record(enc_size);
    uint16_t written = enc->encrypt(pt.data(), 16, record.data());
    EXPECT_GT(written, 0u) << "Encrypt should succeed at warn threshold";

    // Verify roundtrip
    std::vector<uint8_t> out(32);
    uint16_t dec_len;
    bool ok = dec->decrypt(record.data(), written, out.data(), dec_len);
    EXPECT_TRUE(ok);
    EXPECT_EQ(dec_len, 16);
}

TEST(TlsRecord, DecryptAtMaxSequenceNumberFails) {
    // Decrypt should fail when read sequence reaches the max
    auto state = make_roundtrip_state();
    auto enc = TlsRecordCrypto::create(state);
    ASSERT_TRUE(enc.has_value());

    // Encrypt a valid record at seq=0
    std::vector<uint8_t> pt(17, 0xCC);
    uint16_t enc_size = TlsRecordCrypto::encrypted_size(16);
    std::vector<uint8_t> record(enc_size);
    uint16_t written = enc->encrypt(pt.data(), 16, record.data());
    ASSERT_GT(written, 0u);

    // Create a decryptor with seq at the limit
    TlsHotState dec_state{};
    std::memcpy(dec_state.read.ki.key, state.write.ki.key, tls_const::kAes256KeyLen);
    std::memcpy(dec_state.read.ki.iv,  state.write.ki.iv,  tls_const::kTls13NonceLen);
    dec_state.read.seq = tls_record::kMaxSequenceNumber;
    auto dec = TlsRecordCrypto::create(dec_state);
    ASSERT_TRUE(dec.has_value());

    // Decrypt should fail at the sequence limit
    std::vector<uint8_t> out(32);
    uint16_t dec_len;
    bool ok = dec->decrypt(record.data(), written, out.data(), dec_len);
    EXPECT_FALSE(ok) << "Decrypt should fail at max sequence number";
}

// ─────────────────────────────────────────────────────────────────────────────
// Decrypt error path coverage — malformed record scenarios
// ─────────────────────────────────────────────────────────────────────────────

TEST(TlsRecord, DecryptRecordTooShortForHeaderAndTag) {
    // Record shorter than header(5) + auth_tag(16) = 21 bytes minimum.
    // This exercises the first early-return path in decrypt().
    auto state = make_roundtrip_state();
    auto crypto = TlsRecordCrypto::create(state);
    ASSERT_TRUE(crypto.has_value());

    uint8_t out[64];
    uint16_t dec_len;

    // Exactly 20 bytes = header(5) + tag(16) - 1 → too short
    uint8_t short_record[20] = {};
    short_record[0] = 0x17; // AppData
    EXPECT_FALSE(crypto->decrypt(short_record, 20, out, dec_len));

    // Zero-length record
    EXPECT_FALSE(crypto->decrypt(short_record, 0, out, dec_len));

    // Exactly 21 bytes = header(5) + tag(16) → just enough for header parsing
    // but payload_len in header must also be valid
    uint8_t minimal_record[21] = {};
    minimal_record[0] = 0x17;
    minimal_record[1] = 0x03; minimal_record[2] = 0x03;
    minimal_record[3] = 0x00; minimal_record[4] = 0x10; // payload_len = 16 = kAuthTagLen
    // payload_len(16) < kAuthTagLen + 1(17) → should fail at payload size check
    EXPECT_FALSE(crypto->decrypt(minimal_record, 21, out, dec_len));
}

TEST(TlsRecord, DecryptRecordHeaderPayloadExceedsRecordLen) {
    // Header claims a payload_len larger than the actual record data.
    // This exercises the truncation check path in decrypt().
    auto state = make_roundtrip_state();
    auto crypto = TlsRecordCrypto::create(state);
    ASSERT_TRUE(crypto.has_value());

    // Header says payload_len = 100, but we only provide header(5) + 20 bytes
    uint8_t record[25] = {};
    record[0] = 0x17; // AppData
    record[1] = 0x03; record[2] = 0x03;
    record[3] = 0x00; record[4] = 100; // payload_len = 100

    uint8_t out[256];
    uint16_t dec_len;
    EXPECT_FALSE(crypto->decrypt(record, 25, out, dec_len))
        << "Decrypt should fail when header payload_len exceeds actual record_len";
}

TEST(TlsRecord, DecryptWrongContentTypeFails) {
    // Record with non-AppData content type should be rejected by parse_record_header.
    auto state = make_roundtrip_state();
    auto crypto = TlsRecordCrypto::create(state);
    ASSERT_TRUE(crypto.has_value());

    uint8_t record[64] = {};
    record[0] = 0x14; // ChangeCipherSpec, not AppData (0x17)
    record[1] = 0x03; record[2] = 0x03;
    record[3] = 0x00; record[4] = 0x20; // payload_len = 32

    uint8_t out[64];
    uint16_t dec_len;
    EXPECT_FALSE(crypto->decrypt(record, 64, out, dec_len))
        << "Decrypt should reject non-AppData content type";
}

TEST(TlsRecord, DecryptPayloadExactlyAuthTagSizeFails) {
    // Payload of exactly kAuthTagLen bytes (no room for inner content type byte).
    // payload_len must be > kAuthTagLen to hold at least the content type.
    auto state = make_roundtrip_state();
    auto crypto = TlsRecordCrypto::create(state);
    ASSERT_TRUE(crypto.has_value());

    constexpr uint16_t payload = tls_record::kAuthTagLen; // 16
    constexpr uint16_t record_len = tls_record::kRecordHeaderLen + payload; // 21
    uint8_t record[record_len] = {};
    record[0] = 0x17;
    record[1] = 0x03; record[2] = 0x03;
    record[3] = static_cast<uint8_t>(payload >> 8);
    record[4] = static_cast<uint8_t>(payload & 0xFF);

    uint8_t out[64];
    uint16_t dec_len;
    EXPECT_FALSE(crypto->decrypt(record, record_len, out, dec_len))
        << "Decrypt should fail when payload = auth_tag (no content type byte)";
}

TEST(TlsRecord, EncryptZeroLengthNullPlaintext) {
    // Encrypt with plaintext_len=0 and plaintext=nullptr is valid (TLS padding).
    auto state = make_roundtrip_state();
    auto crypto = TlsRecordCrypto::create(state);
    ASSERT_TRUE(crypto.has_value());

    uint16_t enc_size = TlsRecordCrypto::encrypted_size(0);
    std::vector<uint8_t> record(enc_size);
    uint16_t written = crypto->encrypt(nullptr, 0, record.data());
    EXPECT_GT(written, 0u) << "Encrypting zero-length payload should succeed";

    // Verify it can be decrypted back
    auto dec_state = make_roundtrip_state();
    auto dec = TlsRecordCrypto::create(dec_state);
    ASSERT_TRUE(dec.has_value());

    std::vector<uint8_t> out(16);
    uint16_t dec_len;
    bool ok = dec->decrypt(record.data(), written, out.data(), dec_len);
    EXPECT_TRUE(ok);
    EXPECT_EQ(dec_len, 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// TlsConfig::warnings()
// ─────────────────────────────────────────────────────────────────────────────

TEST(TlsConfigWarnings, SecureDefaultNoWarnings) {
    TlsConfig cfg{.hostname = "example.com"};
    auto w = cfg.warnings();
    EXPECT_TRUE(w.empty()) << "Unexpected warning: " << (w.empty() ? "" : w[0]);
}

TEST(TlsConfigWarnings, VerifyPeerDisabledWarns) {
    TlsConfig cfg{.hostname = "example.com", .verify_peer = false};
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("MITM") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected MITM warning when verify_peer=false";
}

TEST(TlsConfigWarnings, EmptyHostnameWarns) {
    TlsConfig cfg{};
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("SNI") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected SNI warning for empty hostname";
}

TEST(TlsConfigWarnings, ShortHandshakeTimeoutWarns) {
    TlsConfig cfg{.hostname = "example.com",
                  .handshake_timeout = std::chrono::milliseconds{500}};
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("500ms") != std::string::npos &&
            msg.find("short") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected short timeout warning";
}

TEST(TlsConfigWarnings, LongHandshakeTimeoutWarns) {
    TlsConfig cfg{.hostname = "example.com",
                  .handshake_timeout = std::chrono::milliseconds{60000}};
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("60000ms") != std::string::npos &&
            msg.find("long") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected long timeout warning";
}

TEST(TlsConfigWarnings, CaCertWithVerifyPeerDisabledWarns) {
    TlsConfig cfg{.hostname = "example.com",
                  .ca_cert_path = "/path/to/ca.pem",
                  .verify_peer = false};
    auto w = cfg.warnings();
    bool found_mitm = false;
    bool found_ca = false;
    for (const auto& msg : w) {
        if (msg.find("MITM") != std::string::npos) found_mitm = true;
        if (msg.find("ca_cert_path") != std::string::npos &&
            msg.find("verify_peer=false") != std::string::npos) found_ca = true;
    }
    EXPECT_TRUE(found_mitm) << "Expected MITM warning";
    EXPECT_TRUE(found_ca) << "Expected CA cert ignored warning";
}
