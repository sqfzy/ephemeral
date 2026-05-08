/// @file test_tls_record.cpp
/// Unit tests for the TLS 1.3 record layer: AES-GCM encrypt/decrypt, nonce
/// construction, record header parse/write, sequence-number accounting, and
/// AEAD round-trip / tamper-detection properties.
///
/// Merges the original baselines:
///   - eph-net/tests/test_tls_record.cpp (56 cases, record + TlsConfig)
///   - eph-transport/tests/test_tls_record_roundtrip.cpp (22 cases, round-trip)
/// into a single file targeting:
///   - eph/net/detail/tls_record.hpp
///   - eph/net/detail/tls_encryptor.hpp
///   - eph/net/detail/tls_decryptor.hpp
///   - eph/net/detail/tls_constants.hpp
///
/// TlsConfig-specific tests live in test_tls_config.cpp (Deliverable 3).

#include <array>
#include <cstdint>
#include <cstring>
#include <random>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "eph/net/detail/tls_constants.hpp"
#include "eph/net/detail/tls_decryptor.hpp"
#include "eph/net/detail/tls_encryptor.hpp"
#include "eph/net/detail/tls_record.hpp"

using namespace eph::net;

// ─────────────────────────────────────────────────────────────────────────────
// Compile-time invariants
// ─────────────────────────────────────────────────────────────────────────────

static_assert(tls_record::kRecordHeaderLen == 5);
static_assert(tls_record::kAuthTagLen == 16);
static_assert(tls_const::kMaxRecordPayload == 16384);
static_assert(tls_const::kTls13NonceLen == 12);
static_assert(tls_const::kAes256KeyLen == 32);

static_assert(sizeof(TlsKeyMaterial) == 128, "Must fit two cache lines");
static_assert(sizeof(TlsHotState) == 320, "Must fit five cache lines (4 key + 1 version)");

// Sequence number threshold ordering
static_assert(tls_record::kSequenceWarnThreshold < tls_record::kSequenceReconnectThreshold);
static_assert(tls_record::kSequenceReconnectThreshold < tls_record::kMaxSequenceNumber);
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
    std::memcpy(state.read.ki.key, state.write.ki.key, tls_const::kAes256KeyLen);
    std::memcpy(state.read.ki.iv,  state.write.ki.iv,  tls_const::kTls13NonceLen);
    state.write.seq = 0;
    state.read.seq  = 0;
    return state;
}

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

/// Paired encryptor/decryptor sharing the same key material.
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
// Encrypt / Decrypt roundtrip (parametrised by payload size)
// ─────────────────────────────────────────────────────────────────────────────

class TlsRoundtripSizes : public ::testing::TestWithParam<uint16_t> {};

TEST_P(TlsRoundtripSizes, EncryptDecryptRoundtrip) {
    uint16_t payload_size = GetParam();

    auto state = make_roundtrip_state();

    auto enc_result = TlsRecordCrypto::create(state);
    ASSERT_TRUE(enc_result.has_value()) << enc_result.error();

    // For decryptor, swap: read keys = write keys
    TlsHotState dec_state{};
    std::memcpy(dec_state.read.ki.key, state.write.ki.key, tls_const::kAes256KeyLen);
    std::memcpy(dec_state.read.ki.iv,  state.write.ki.iv,  tls_const::kTls13NonceLen);
    dec_state.read.seq = 0;

    auto dec_result = TlsRecordCrypto::create(dec_state);
    ASSERT_TRUE(dec_result.has_value()) << dec_result.error();

    std::vector<uint8_t> plaintext(payload_size + 1);
    fill_random(plaintext.data(), payload_size, 100);

    uint16_t enc_size = TlsRecordCrypto::encrypted_size(payload_size);
    std::vector<uint8_t> record(enc_size);
    uint16_t written = enc_result->encrypt(
        plaintext.data(), payload_size, record.data());
    ASSERT_GT(written, 0u);
    EXPECT_EQ(written, enc_size);

    EXPECT_EQ(record[0], tls_record::kContentTypeAppData);

    std::vector<uint8_t> decrypted(payload_size + 16);
    uint16_t dec_len;
    bool ok = dec_result->decrypt(
        record.data(), written, decrypted.data(), dec_len);
    ASSERT_TRUE(ok);
    EXPECT_EQ(dec_len, payload_size);

    EXPECT_EQ(std::memcmp(decrypted.data(), plaintext.data(), payload_size), 0);
}

INSTANTIATE_TEST_SUITE_P(
    PayloadSizes, TlsRoundtripSizes,
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
        std::vector<uint8_t> plaintext(33);
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

    bool ok = crypto->decrypt(record, 10, out, dec_len);
    EXPECT_FALSE(ok);
}

TEST(TlsRecord, DecryptPayloadSmallerThanAuthTagFails) {
    // Regression: without the bounds check, a short payload_len would underflow
    // max_out_len in EVP_AEAD_CTX_open.
    auto state = make_test_state();
    auto crypto = TlsRecordCrypto::create(state);
    ASSERT_TRUE(crypto.has_value());

    uint8_t record[64] = {};
    record[0] = 0x17;
    record[1] = 0x03;
    record[2] = 0x03;
    record[3] = 0x00;
    record[4] = 0x05;

    uint8_t out[64];
    uint16_t dec_len;
    bool ok = crypto->decrypt(record, 64, out, dec_len);
    EXPECT_FALSE(ok) << "Decrypting record with payload_len < auth_tag + 1 must fail";
}

TEST(TlsRecord, EncryptOversizedPayloadReturnsZero) {
    auto state = make_test_state();
    auto crypto = TlsRecordCrypto::create(state);
    ASSERT_TRUE(crypto.has_value());

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
    uint16_t max_ok = tls_const::kMaxRecordPayload + tls_record::kAuthTagLen + 1;
    tls_record::write_record_header(buf, 0x17, max_ok);

    uint8_t ct;
    uint16_t len;
    EXPECT_TRUE(tls_record::parse_record_header(buf, ct, len));
    EXPECT_EQ(len, max_ok);

    tls_record::write_record_header(buf, 0x17, max_ok + 1);
    EXPECT_FALSE(tls_record::parse_record_header(buf, ct, len));
}

TEST(TlsRecord, EncryptedSizeFormula) {
    EXPECT_EQ(TlsRecordCrypto::encrypted_size(0),     22);
    EXPECT_EQ(TlsRecordCrypto::encrypted_size(1),     23);
    EXPECT_EQ(TlsRecordCrypto::encrypted_size(100),  122);
    EXPECT_EQ(TlsRecordCrypto::encrypted_size(16384), 16406);
}

TEST(TlsRecord, EncryptPreservesPlaintextBuffer) {
    auto state = make_roundtrip_state();
    auto crypto = TlsRecordCrypto::create(state);
    ASSERT_TRUE(crypto.has_value());

    std::vector<uint8_t> plaintext(65, 0);
    for (int i = 0; i < 64; ++i) plaintext[i] = static_cast<uint8_t>(i);
    plaintext[64] = 0xFE; // sentinel (the +1 byte that encrypt temporarily modifies)

    std::vector<uint8_t> record(TlsRecordCrypto::encrypted_size(64));
    uint16_t written = crypto->encrypt(plaintext.data(), 64, record.data());
    ASSERT_GT(written, 0u);

    EXPECT_EQ(plaintext[64], 0xFE)
        << "encrypt() must restore the byte it temporarily overwrites";
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

    auto state2 = make_test_state(999);
    auto dec = TlsRecordCrypto::create(state2);
    ASSERT_TRUE(dec.has_value());

    std::vector<uint8_t> out(80);
    uint16_t dec_len;
    bool ok = dec->decrypt(record.data(), written, out.data(), dec_len);
    EXPECT_FALSE(ok) << "Decrypting with wrong key should fail authentication";
}

TEST(TlsRecord, CreateWithAes128KeyLength) {
    TlsHotState state{};
    fill_random(state.write.ki.key, 16, 42);
    fill_random(state.write.ki.iv, tls_const::kTls13NonceLen, 43);
    fill_random(state.read.ki.key, 16, 44);
    fill_random(state.read.ki.iv, tls_const::kTls13NonceLen, 45);

    auto crypto = TlsRecordCrypto::create(state, 16);
    ASSERT_TRUE(crypto.has_value()) << crypto.error();

    std::vector<uint8_t> plaintext(33, 0xAA);
    uint16_t enc_size = TlsRecordCrypto::encrypted_size(32);
    std::vector<uint8_t> record(enc_size);
    uint16_t written = crypto->encrypt(plaintext.data(), 32, record.data());
    ASSERT_GT(written, 0u);

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
    auto result = TlsRecordCrypto::create(state, 24);
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

    auto bad_state = make_test_state(999);
    auto dec = TlsRecordCrypto::create(bad_state);
    ASSERT_TRUE(dec.has_value());

    constexpr uint16_t kOutSize = kPlainLen + 1;
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

    std::vector<uint8_t> pt(17, 0);
    std::vector<uint8_t> rec(TlsRecordCrypto::encrypted_size(16));
    (void)enc->encrypt(pt.data(), 16, rec.data());
    EXPECT_EQ(enc->write_seq(), 101u);

    TlsHotState dec_state{};
    std::memcpy(dec_state.read.ki.key, state.write.ki.key, tls_const::kAes256KeyLen);
    std::memcpy(dec_state.read.ki.iv, state.write.ki.iv, tls_const::kTls13NonceLen);
    dec_state.read.seq = 100;
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

    EXPECT_NE(std::memcmp(nonce_max, nonce_max_minus1, 12), 0);

    // First 4 bytes unchanged
    EXPECT_EQ(std::memcmp(nonce_max, iv, 4), 0);
}

TEST(TlsRecord, EncryptDecryptAtHighSequence) {
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
    auto state = make_roundtrip_state();
    state.write.seq = tls_record::kMaxSequenceNumber;
    auto enc = TlsRecordCrypto::create(state);
    ASSERT_TRUE(enc.has_value());

    std::vector<uint8_t> pt(17, 0xAA);
    std::vector<uint8_t> record(TlsRecordCrypto::encrypted_size(16));
    uint16_t written = enc->encrypt(pt.data(), 16, record.data());
    EXPECT_EQ(written, 0u)
        << "Encrypt must fail when write_seq == kMaxSequenceNumber";
    EXPECT_EQ(enc->write_seq(), tls_record::kMaxSequenceNumber);
}

TEST(TlsRecord, DecryptExactlyAtMaxSequenceReturnsZero) {
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
    EXPECT_EQ(dec->read_seq(), tls_record::kMaxSequenceNumber);
}

TEST(TlsRecord, LastValidEncryptSucceeds) {
    auto state = make_roundtrip_state();
    state.write.seq = tls_record::kMaxSequenceNumber - 1;
    auto enc = TlsRecordCrypto::create(state);
    ASSERT_TRUE(enc.has_value());

    std::vector<uint8_t> pt(17, 0xCC);
    std::vector<uint8_t> record(TlsRecordCrypto::encrypted_size(16));
    uint16_t written = enc->encrypt(pt.data(), 16, record.data());
    EXPECT_GT(written, 0u);
    EXPECT_EQ(enc->write_seq(), tls_record::kMaxSequenceNumber);

    std::vector<uint8_t> record2(TlsRecordCrypto::encrypted_size(16));
    uint16_t written2 = enc->encrypt(pt.data(), 16, record2.data());
    EXPECT_EQ(written2, 0u) << "Encrypt past limit must fail";
}

TEST(TlsRecord, SequenceCounterIncrements) {
    auto state = make_roundtrip_state();
    auto crypto = TlsRecordCrypto::create(state);
    ASSERT_TRUE(crypto.has_value());

    EXPECT_EQ(crypto->write_seq(), 0u);
    EXPECT_EQ(crypto->read_seq(), 0u);

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

    *result1 = std::move(*result2);

    EXPECT_EQ(result1->write_seq(), 0u);
    EXPECT_EQ(result1->read_seq(), 0u);
}

// MoveAssign overwriting a live (non-zero seq) crypto: validates that the
// pre-existing EVP_AEAD_CTX in the destination is properly cleaned up
// rather than leaked, and that the moved-from source can be destroyed
// without double-free. Audit-2026-04-01 #34 flagged this — fix relies on
// `if (init_) EVP_AEAD_CTX_cleanup(&ctx_);` BEFORE bitwise-copy and
// `OPENSSL_cleanse(&other.ctx_); other.init_ = false;` after.
TEST(TlsRecord, MoveAssignOverLiveCryptoIsSafe) {
    // Step 1: build a live crypto, advance its sequence counters by
    // running a roundtrip so it's clearly not in default-constructed
    // state. Then build a fresh one to move-assign in.
    auto state1 = make_loopback_state(0xA5, 0x5A);
    auto rt1 = TlsRecordCrypto::create(state1);
    ASSERT_TRUE(rt1.has_value());

    std::vector<uint8_t> pt(33, 0x42);
    uint16_t enc_size = TlsRecordCrypto::encrypted_size(32);
    std::vector<uint8_t> rec(enc_size);
    uint16_t written = rt1->encrypt(pt.data(), 32, rec.data());
    ASSERT_GT(written, 0u);
    EXPECT_EQ(rt1->write_seq(), 1u) << "post-encrypt seq must advance";

    // Step 2: create the donor with completely different keys (different
    // seed). After the move, rt1 must be able to encrypt with the new keys
    // — i.e. no residual state from the destination's prior config.
    auto state2 = make_loopback_state(0xC3, 0x3C);
    auto rt2 = TlsRecordCrypto::create(state2);
    ASSERT_TRUE(rt2.has_value());

    *rt1 = std::move(*rt2);

    EXPECT_EQ(rt1->write_seq(), 0u)
        << "moved-into seq must reset to 0 (donor's value)";

    // Step 3: encrypt with the new state and decrypt with a fresh
    // crypto built from the SAME state2. If move-assign leaked the donor's
    // ctx_ (i.e. the destination still holds state1's key) decryption
    // would fail with auth-tag mismatch.
    auto verifier = TlsRecordCrypto::create(state2);
    ASSERT_TRUE(verifier.has_value());

    std::vector<uint8_t> rec2(enc_size);
    uint16_t w2 = rt1->encrypt(pt.data(), 32, rec2.data());
    ASSERT_GT(w2, 0u);

    uint8_t out[64];
    uint16_t dec_len = 0;
    bool ok = verifier->decrypt(rec2.data(), w2, out, dec_len);
    EXPECT_TRUE(ok)
        << "decrypt must succeed with state2 keys — proves move-assign "
           "cleaned up the old ctx and installed the donor's";
    EXPECT_EQ(dec_len, 32u);
    EXPECT_EQ(0, std::memcmp(out, pt.data(), 32));

    // Step 4: rt2 (the moved-from) must still be safely destroyable
    // when this scope ends — `OPENSSL_cleanse(&other.ctx_)` + `init_=false`
    // means the dtor's `if (init_) EVP_AEAD_CTX_cleanup` is a no-op.
    // ASan/leak-san would scream if not.
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
// TLS sequence warn threshold and limit constants
// ─────────────────────────────────────────────────────────────────────────────

TEST(TlsRecord, SequenceWarnThresholdIs90Percent) {
    double ratio = static_cast<double>(tls_record::kSequenceWarnThreshold) /
                   static_cast<double>(tls_record::kMaxSequenceNumber);
    EXPECT_GE(ratio, 0.89);
    EXPECT_LE(ratio, 0.91);
    EXPECT_LT(tls_record::kSequenceWarnThreshold, tls_record::kMaxSequenceNumber);
}

TEST(TlsRecord, EncryptAtWarnThresholdStillSucceeds) {
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

    std::vector<uint8_t> pt(17, 0xAB);
    uint16_t enc_size = TlsRecordCrypto::encrypted_size(16);
    std::vector<uint8_t> record(enc_size);
    uint16_t written = enc->encrypt(pt.data(), 16, record.data());
    EXPECT_GT(written, 0u) << "Encrypt should succeed at warn threshold";

    std::vector<uint8_t> out(32);
    uint16_t dec_len;
    bool ok = dec->decrypt(record.data(), written, out.data(), dec_len);
    EXPECT_TRUE(ok);
    EXPECT_EQ(dec_len, 16);
}

TEST(TlsRecord, DecryptAtMaxSequenceNumberFails) {
    auto state = make_roundtrip_state();
    auto enc = TlsRecordCrypto::create(state);
    ASSERT_TRUE(enc.has_value());

    std::vector<uint8_t> pt(17, 0xCC);
    uint16_t enc_size = TlsRecordCrypto::encrypted_size(16);
    std::vector<uint8_t> record(enc_size);
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
    EXPECT_FALSE(ok) << "Decrypt should fail at max sequence number";
}

// ─────────────────────────────────────────────────────────────────────────────
// Decrypt error path coverage
// ─────────────────────────────────────────────────────────────────────────────

TEST(TlsRecord, DecryptRecordTooShortForHeaderAndTag) {
    auto state = make_roundtrip_state();
    auto crypto = TlsRecordCrypto::create(state);
    ASSERT_TRUE(crypto.has_value());

    uint8_t out[64];
    uint16_t dec_len;

    uint8_t short_record[20] = {};
    short_record[0] = 0x17;
    EXPECT_FALSE(crypto->decrypt(short_record, 20, out, dec_len));

    EXPECT_FALSE(crypto->decrypt(short_record, 0, out, dec_len));

    uint8_t minimal_record[21] = {};
    minimal_record[0] = 0x17;
    minimal_record[1] = 0x03; minimal_record[2] = 0x03;
    minimal_record[3] = 0x00; minimal_record[4] = 0x10; // payload_len = 16 = kAuthTagLen
    EXPECT_FALSE(crypto->decrypt(minimal_record, 21, out, dec_len));
}

TEST(TlsRecord, DecryptRecordHeaderPayloadExceedsRecordLen) {
    auto state = make_roundtrip_state();
    auto crypto = TlsRecordCrypto::create(state);
    ASSERT_TRUE(crypto.has_value());

    uint8_t record[25] = {};
    record[0] = 0x17;
    record[1] = 0x03; record[2] = 0x03;
    record[3] = 0x00; record[4] = 100;

    uint8_t out[256];
    uint16_t dec_len;
    EXPECT_FALSE(crypto->decrypt(record, 25, out, dec_len))
        << "Decrypt should fail when header payload_len exceeds actual record_len";
}

TEST(TlsRecord, DecryptWrongContentTypeFails) {
    auto state = make_roundtrip_state();
    auto crypto = TlsRecordCrypto::create(state);
    ASSERT_TRUE(crypto.has_value());

    uint8_t record[64] = {};
    record[0] = 0x14; // ChangeCipherSpec, not AppData
    record[1] = 0x03; record[2] = 0x03;
    record[3] = 0x00; record[4] = 0x20;

    uint8_t out[64];
    uint16_t dec_len;
    EXPECT_FALSE(crypto->decrypt(record, 64, out, dec_len))
        << "Decrypt should reject non-AppData content type";
}

TEST(TlsRecord, DecryptPayloadExactlyAuthTagSizeFails) {
    auto state = make_roundtrip_state();
    auto crypto = TlsRecordCrypto::create(state);
    ASSERT_TRUE(crypto.has_value());

    constexpr uint16_t payload = tls_record::kAuthTagLen;
    constexpr uint16_t record_len = tls_record::kRecordHeaderLen + payload;
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
    auto state = make_roundtrip_state();
    auto crypto = TlsRecordCrypto::create(state);
    ASSERT_TRUE(crypto.has_value());

    uint16_t enc_size = TlsRecordCrypto::encrypted_size(0);
    std::vector<uint8_t> record(enc_size);
    uint16_t written = crypto->encrypt(nullptr, 0, record.data());
    EXPECT_GT(written, 0u) << "Encrypting zero-length payload should succeed";

    auto dec_state = make_roundtrip_state();
    auto dec = TlsRecordCrypto::create(dec_state);
    ASSERT_TRUE(dec.has_value());

    std::vector<uint8_t> out(16);
    uint16_t dec_len;
    bool ok = dec->decrypt(record.data(), written, out.data(), dec_len);
    EXPECT_TRUE(ok);
    EXPECT_EQ(dec_len, 0u);
}

// =========================================================================
// END-TO-END ROUND-TRIP COVERAGE (from baseline test_tls_record_roundtrip.cpp)
//
// The above tests exercise TlsRecordCrypto (the composite). These tests
// exercise the split TlsEncryptor/TlsDecryptor path directly, which is the
// primary surface for the kDirectTx mode (single-threaded owner).
// =========================================================================

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

// ─────────────────────────────────────────────────────────────────────────────
// Sequence number advancement (multi-record session via split ctx)
// ─────────────────────────────────────────────────────────────────────────────

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
    // Replay / skip protection: if decryptor's seq diverges from encryptor's,
    // the AEAD nonce mismatches and decryption fails authentication.
    auto cp = make_pair();
    std::vector<uint8_t> out(8192, 0);
    std::vector<uint8_t> recovered(8192, 0);

    std::vector<uint8_t> p1{1, 2, 3, 4};
    std::vector<uint8_t> p2{5, 6, 7, 8};
    std::vector<uint8_t> p3{9, 10, 11, 12};

    uint16_t e1 = cp.enc.encrypt(p1.data(), 4, out.data());
    ASSERT_GT(e1, 0u);
    uint16_t d1 = 0;
    ASSERT_TRUE(cp.dec.decrypt(out.data(), e1, recovered.data(), d1));

    std::vector<uint8_t> tmp(8192, 0);
    (void)cp.enc.encrypt(p2.data(), 4, tmp.data());

    uint16_t e3 = cp.enc.encrypt(p3.data(), 4, out.data());
    ASSERT_GT(e3, 0u);
    uint16_t d3 = 0;
    EXPECT_FALSE(cp.dec.decrypt(out.data(), e3, recovered.data(), d3))
        << "decrypt with mismatched seq must fail (replay/skip protection)";
}

// ─────────────────────────────────────────────────────────────────────────────
// Tamper detection — single-bit flip in ciphertext / tag / header
// ─────────────────────────────────────────────────────────────────────────────

TEST(TlsRoundTrip, FlippedCiphertextByteFailsAuth) {
    auto cp = make_pair();
    std::vector<uint8_t> plaintext(128, 0xCC);
    std::vector<uint8_t> ciphertext(8192, 0);
    uint16_t enc_len = cp.enc.encrypt(plaintext.data(), 128, ciphertext.data());
    ASSERT_GT(enc_len, 0u);

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

    ciphertext[enc_len - 1] ^= 0x80;

    std::vector<uint8_t> out(8192, 0);
    uint16_t out_len = 0;
    bool ok = cp.dec.decrypt(ciphertext.data(), enc_len, out.data(), out_len);
    EXPECT_FALSE(ok);
}

TEST(TlsRoundTrip, FlippedRecordHeaderByteFailsAuth) {
    // The record header is the AEAD AAD — tampering must fail authentication.
    auto cp = make_pair();
    std::vector<uint8_t> plaintext(128, 0xCC);
    std::vector<uint8_t> ciphertext(8192, 0);
    uint16_t enc_len = cp.enc.encrypt(plaintext.data(), 128, ciphertext.data());
    ASSERT_GT(enc_len, 0u);

    // Flip the high byte of the length field
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

    std::vector<uint8_t> out(8192, 0);
    uint16_t out_len = 0;
    bool ok = cp.dec.decrypt(ciphertext.data(), enc_len - 5, out.data(), out_len);
    EXPECT_FALSE(ok);
}

// ─────────────────────────────────────────────────────────────────────────────
// Different keys produce different ciphertext
// ─────────────────────────────────────────────────────────────────────────────

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
    ASSERT_EQ(la, lb);

    EXPECT_NE(0, std::memcmp(
        out_a.data() + tls_record::kRecordHeaderLen,
        out_b.data() + tls_record::kRecordHeaderLen,
        la - tls_record::kRecordHeaderLen));
}

// ─────────────────────────────────────────────────────────────────────────────
// AES-128 round-trip
// ─────────────────────────────────────────────────────────────────────────────

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

// ─────────────────────────────────────────────────────────────────────────────
// Wrong content_type rejected by the parser
// ─────────────────────────────────────────────────────────────────────────────

TEST(TlsRoundTrip, NonAppDataContentTypeRejected) {
    auto cp = make_pair();
    std::vector<uint8_t> plaintext(64, 0x33);
    std::vector<uint8_t> ciphertext(8192, 0);
    uint16_t enc_len = cp.enc.encrypt(plaintext.data(), 64, ciphertext.data());
    ASSERT_GT(enc_len, 0u);

    ciphertext[0] = 0x16; // Handshake, not AppData
    std::vector<uint8_t> out(8192, 0);
    uint16_t out_len = 0;
    bool ok = cp.dec.decrypt(ciphertext.data(), enc_len, out.data(), out_len);
    EXPECT_FALSE(ok);
}

// ─────────────────────────────────────────────────────────────────────────────
// build_nonce — extra coverage with hand-checked vectors
// ─────────────────────────────────────────────────────────────────────────────

TEST(TlsBuildNonce, RfcStyleVectorSeq0) {
    uint8_t iv[tls_const::kTls13NonceLen] = {
        0x5d, 0x31, 0x3e, 0xb2, 0x67, 0x12, 0x76, 0xee,
        0x13, 0x00, 0x0b, 0x30,
    };
    uint8_t out[tls_const::kTls13NonceLen] = {};
    tls_record::build_nonce(out, iv, 0);
    EXPECT_EQ(0, std::memcmp(out, iv, tls_const::kTls13NonceLen));
}

TEST(TlsBuildNonce, RfcStyleVectorSeq1) {
    uint8_t iv[tls_const::kTls13NonceLen] = {
        0x5d, 0x31, 0x3e, 0xb2, 0x67, 0x12, 0x76, 0xee,
        0x13, 0x00, 0x0b, 0x30,
    };
    uint8_t expected[tls_const::kTls13NonceLen] = {
        0x5d, 0x31, 0x3e, 0xb2,
        0x67, 0x12, 0x76, 0xee,
        0x13, 0x00, 0x0b, 0x30 ^ 0x01,
    };
    uint8_t out[tls_const::kTls13NonceLen] = {};
    tls_record::build_nonce(out, iv, 1);
    EXPECT_EQ(0, std::memcmp(out, expected, tls_const::kTls13NonceLen));
}

TEST(TlsBuildNonce, RfcStyleVectorLargeSeq) {
    // seq = 0x0123456789ABCDEF, IV all zeros → output is just the BE seq
    uint8_t iv[tls_const::kTls13NonceLen] = {};
    uint8_t expected[tls_const::kTls13NonceLen] = {
        0, 0, 0, 0, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
    };
    uint8_t out[tls_const::kTls13NonceLen] = {};
    tls_record::build_nonce(out, iv, 0x0123456789ABCDEFULL);
    EXPECT_EQ(0, std::memcmp(out, expected, tls_const::kTls13NonceLen));
}

// ─────────────────────────────────────────────────────────────────────────────
// TlsConfig smoke tests (from baseline test_tls_record.cpp)
//
// These overlap with test_tls_config.cpp by design — they pin the fact that
// including tls_record.hpp alone is sufficient to exercise the TlsConfig API
// (TlsConfig lives in the same detail layer and transitively comes in via
// tls_constants.hpp).  Full TlsConfig coverage is in test_tls_config.cpp.
// ─────────────────────────────────────────────────────────────────────────────

TEST(TlsRecordConfigSmoke, ValidateDefaultConfigPasses) {
    TlsConfig cfg{};
    EXPECT_TRUE(cfg.validate().has_value());
}

TEST(TlsRecordConfigSmoke, ValidateZeroHandshakeTimeoutFails) {
    TlsConfig cfg{.handshake_timeout = std::chrono::milliseconds{0}};
    auto err = cfg.validate();
    ASSERT_FALSE(err.has_value());
    EXPECT_NE(std::string_view{err.error().detail}.find("handshake_timeout"),
              std::string_view::npos);
}

TEST(TlsRecordConfigSmoke, ValidateNegativeHandshakeTimeoutFails) {
    TlsConfig cfg{.handshake_timeout = std::chrono::milliseconds{-1}};
    auto err = cfg.validate();
    EXPECT_FALSE(err.has_value());
}

TEST(TlsRecordConfigSmoke, ValidateBothCertAndKeySetPasses) {
    TlsConfig cfg{.client_cert_path = "/cert.pem", .client_key_path = "/key.pem"};
    EXPECT_TRUE(cfg.validate().has_value());
}

TEST(TlsRecordConfigSmoke, ValidateCertWithoutKeyFails) {
    TlsConfig cfg{.client_cert_path = "/cert.pem"};
    auto err = cfg.validate();
    ASSERT_FALSE(err.has_value());
    EXPECT_NE(std::string_view{err.error().detail}.find("client_cert_path"),
              std::string_view::npos);
}

TEST(TlsRecordConfigSmoke, ValidateKeyWithoutCertFails) {
    TlsConfig cfg{.client_key_path = "/key.pem"};
    auto err = cfg.validate();
    EXPECT_FALSE(err.has_value());
}

TEST(TlsRecordConfigSmoke, EqualityDefaultBehavior) {
    TlsConfig a{};
    TlsConfig b{};
    EXPECT_EQ(a, b);
}

TEST(TlsRecordConfigSmoke, EqualityDetectsDifference) {
    TlsConfig a{.hostname = "host1"};
    TlsConfig b{.hostname = "host2"};
    EXPECT_NE(a, b);
}

TEST(TlsRecordConfigSmoke, ToJsonContainsAllFields) {
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
    // client_key_path is redacted in JSON output for security
    EXPECT_NE(json.find("\"has_client_key\":true"), std::string::npos);
}

TEST(TlsRecordConfigSmoke, DumpContainsHostname) {
    TlsConfig cfg{.hostname = "example.com"};
    auto d = cfg.dump();
    EXPECT_NE(d.find("example.com"), std::string::npos);
    EXPECT_NE(d.find("TlsConfig"), std::string::npos);
}

TEST(TlsRecordConfigSmoke, FormatterProducesNonEmpty) {
    TlsConfig cfg{.hostname = "test.local"};
    auto s = std::format("{}", cfg);
    EXPECT_FALSE(s.empty());
    EXPECT_NE(s.find("test.local"), std::string::npos);
}

TEST(TlsRecordConfigSmoke, WarningsSecureDefaultNoWarnings) {
    TlsConfig cfg{.hostname = "example.com"};
    auto w = cfg.warnings();
    EXPECT_TRUE(w.empty()) << "Unexpected warning: " << (w.empty() ? "" : w[0]);
}

TEST(TlsRecordConfigSmoke, WarningsVerifyPeerDisabledWarns) {
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

TEST(TlsRecordConfigSmoke, WarningsEmptyHostnameWarns) {
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

TEST(TlsRecordConfigSmoke, WarningsShortHandshakeTimeoutWarns) {
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

TEST(TlsRecordConfigSmoke, WarningsLongHandshakeTimeoutWarns) {
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

TEST(TlsRecordConfigSmoke, WarningsCaCertWithVerifyPeerDisabledWarns) {
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
