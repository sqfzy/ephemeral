/// @file test_tls_record.cpp
/// Tests for TLS record layer utilities: nonce building, header write/parse.

#include <array>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "eph/transport/detail/tls_constants.hpp"
#include "eph/transport/detail/tls_record.hpp"

using namespace eph::net;

// =======================================================================
// tls_record::build_nonce
// =======================================================================

TEST(TlsRecordBuildNonce, ZeroSequenceReturnsIvUnchanged) {
    uint8_t iv[tls_const::kTls13NonceLen] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0A, 0x0B, 0x0C};
    uint8_t out[tls_const::kTls13NonceLen] = {};
    tls_record::build_nonce(out, iv, 0);
    // XOR with zero should yield IV unchanged
    EXPECT_EQ(std::memcmp(out, iv, tls_const::kTls13NonceLen), 0);
}

TEST(TlsRecordBuildNonce, NonZeroSequenceXorsCorrectly) {
    // IV: all zeros -> nonce = big-endian seq padded to 12 bytes
    uint8_t iv[tls_const::kTls13NonceLen] = {};
    uint8_t out[tls_const::kTls13NonceLen] = {};
    tls_record::build_nonce(out, iv, 1);

    // First 4 bytes: IV[0..3] XOR 0 = 0
    EXPECT_EQ(out[0], 0);
    EXPECT_EQ(out[1], 0);
    EXPECT_EQ(out[2], 0);
    EXPECT_EQ(out[3], 0);
    // Last byte should be 1 (big-endian seq=1, last byte = 0x01)
    EXPECT_EQ(out[11], 1);
}

TEST(TlsRecordBuildNonce, DifferentSequencesProduceDifferentNonces) {
    uint8_t iv[tls_const::kTls13NonceLen] = {
        0xAA, 0xBB, 0xCC, 0xDD, 0x11, 0x22, 0x33, 0x44,
        0x55, 0x66, 0x77, 0x88};
    uint8_t out1[tls_const::kTls13NonceLen] = {};
    uint8_t out2[tls_const::kTls13NonceLen] = {};
    tls_record::build_nonce(out1, iv, 1);
    tls_record::build_nonce(out2, iv, 2);
    EXPECT_NE(std::memcmp(out1, out2, tls_const::kTls13NonceLen), 0);
}

// =======================================================================
// tls_record::write_record_header / parse_record_header
// =======================================================================

TEST(TlsRecordHeader, WriteAndParseRoundtrip) {
    uint8_t buf[tls_record::kRecordHeaderLen] = {};
    uint16_t payload_len = 1024;
    tls_record::write_record_header(buf, tls_record::kContentTypeAppData, payload_len);

    uint8_t ct = 0;
    uint16_t parsed_len = 0;
    bool ok = tls_record::parse_record_header(buf, ct, parsed_len);

    EXPECT_TRUE(ok);
    EXPECT_EQ(ct, tls_record::kContentTypeAppData);
    EXPECT_EQ(parsed_len, payload_len);
}

TEST(TlsRecordHeader, WriteCorrectFormat) {
    uint8_t buf[tls_record::kRecordHeaderLen] = {};
    tls_record::write_record_header(buf, tls_record::kContentTypeAppData, 256);

    // Byte 0: content type
    EXPECT_EQ(buf[0], 0x17);
    // Bytes 1-2: legacy version (0x0303)
    EXPECT_EQ(buf[1], 0x03);
    EXPECT_EQ(buf[2], 0x03);
    // Bytes 3-4: payload length big-endian (256 = 0x0100)
    EXPECT_EQ(buf[3], 0x01);
    EXPECT_EQ(buf[4], 0x00);
}

TEST(TlsRecordHeader, ParseRejectsNonAppDataContentType) {
    uint8_t buf[tls_record::kRecordHeaderLen] = {
        0x15, 0x03, 0x03, 0x00, 0x10};  // content_type = 0x15 (alert)
    uint8_t ct = 0;
    uint16_t len = 0;
    bool ok = tls_record::parse_record_header(buf, ct, len);
    EXPECT_FALSE(ok);
}

TEST(TlsRecordHeader, ParseRejectsOversizedPayload) {
    // Max valid: kMaxRecordPayload + kAuthTagLen + 1
    uint16_t max_valid = tls_const::kMaxRecordPayload +
                         tls_record::kAuthTagLen + 1;
    uint16_t too_large = max_valid + 1;
    uint8_t buf[tls_record::kRecordHeaderLen] = {
        tls_record::kContentTypeAppData, 0x03, 0x03,
        static_cast<uint8_t>(too_large >> 8),
        static_cast<uint8_t>(too_large & 0xFF)};
    uint8_t ct = 0;
    uint16_t len = 0;
    bool ok = tls_record::parse_record_header(buf, ct, len);
    EXPECT_FALSE(ok);
}

TEST(TlsRecordHeader, ParseAcceptsMaxValidPayload) {
    uint16_t max_valid = tls_const::kMaxRecordPayload +
                         tls_record::kAuthTagLen + 1;
    uint8_t buf[tls_record::kRecordHeaderLen] = {
        tls_record::kContentTypeAppData, 0x03, 0x03,
        static_cast<uint8_t>(max_valid >> 8),
        static_cast<uint8_t>(max_valid & 0xFF)};
    uint8_t ct = 0;
    uint16_t len = 0;
    bool ok = tls_record::parse_record_header(buf, ct, len);
    EXPECT_TRUE(ok);
    EXPECT_EQ(len, max_valid);
}

// =======================================================================
// Constants sanity checks
// =======================================================================

TEST(TlsRecordConstants, SequenceLimitsAreOrdered) {
    EXPECT_LT(tls_record::kSequenceWarnThreshold,
              tls_record::kSequenceReconnectThreshold);
    EXPECT_LT(tls_record::kSequenceReconnectThreshold,
              tls_record::kMaxSequenceNumber);
}

TEST(TlsRecordConstants, WarnThresholdIs90Percent) {
    // 90% of 2^24
    EXPECT_EQ(tls_record::kSequenceWarnThreshold,
              tls_record::kMaxSequenceNumber * 9 / 10);
}

TEST(TlsRecordConstants, ReconnectThresholdIs95Percent) {
    EXPECT_EQ(tls_record::kSequenceReconnectThreshold,
              tls_record::kMaxSequenceNumber * 95 / 100);
}

TEST(TlsRecordConstants, MaxSequenceNumberIs2Pow32) {
    EXPECT_EQ(tls_record::kMaxSequenceNumber, 1ULL << 32);
}

TEST(TlsRecordConstants, AuthTagLenIs16) {
    EXPECT_EQ(tls_record::kAuthTagLen, 16);
}

TEST(TlsRecordConstants, RecordHeaderLenIs5) {
    EXPECT_EQ(tls_record::kRecordHeaderLen, 5);
}

// =======================================================================
// build_nonce — additional edge cases
// =======================================================================

TEST(TlsRecordBuildNonce, LargeSequenceNumberXorsCorrectly) {
    // Test with a sequence number that uses upper bytes
    uint8_t iv[tls_const::kTls13NonceLen] = {};
    uint8_t out[tls_const::kTls13NonceLen] = {};
    // seq = 0x0102030405060708
    tls_record::build_nonce(out, iv, 0x0102030405060708ULL);

    // With zero IV, out should equal big-endian seq right-aligned in 12 bytes
    // First 4 bytes are IV[0..3] = 0, and the XOR is with the high 4 bytes
    // of the 8-byte big-endian seq, which is placed at offset 4.
    // Big-endian of 0x0102030405060708: 01 02 03 04 05 06 07 08
    // This gets XORed with iv[4..11] which is all zeros.
    EXPECT_EQ(out[4], 0x01);
    EXPECT_EQ(out[5], 0x02);
    EXPECT_EQ(out[6], 0x03);
    EXPECT_EQ(out[7], 0x04);
    EXPECT_EQ(out[8], 0x05);
    EXPECT_EQ(out[9], 0x06);
    EXPECT_EQ(out[10], 0x07);
    EXPECT_EQ(out[11], 0x08);
}

TEST(TlsRecordBuildNonce, XorWithNonZeroIvAndSeq) {
    uint8_t iv[tls_const::kTls13NonceLen] = {
        0xFF, 0xFF, 0xFF, 0xFF,  // first 4 bytes copied directly
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF};
    uint8_t out[tls_const::kTls13NonceLen] = {};
    tls_record::build_nonce(out, iv, 1);

    // First 4 bytes: copied from IV
    EXPECT_EQ(out[0], 0xFF);
    EXPECT_EQ(out[1], 0xFF);
    EXPECT_EQ(out[2], 0xFF);
    EXPECT_EQ(out[3], 0xFF);
    // Last byte: iv[11] XOR seq_be[7] = 0xFF XOR 0x01 = 0xFE
    EXPECT_EQ(out[11], 0xFE);
}

// =======================================================================
// parse_record_header — additional edge cases
// =======================================================================

TEST(TlsRecordHeader, ParseZeroPayloadLength) {
    uint8_t buf[tls_record::kRecordHeaderLen] = {
        tls_record::kContentTypeAppData, 0x03, 0x03, 0x00, 0x00};
    uint8_t ct = 0;
    uint16_t len = 0;
    bool ok = tls_record::parse_record_header(buf, ct, len);
    EXPECT_TRUE(ok);
    EXPECT_EQ(len, 0);
}

TEST(TlsRecordHeader, WriteAndParseSmallPayload) {
    uint8_t buf[tls_record::kRecordHeaderLen] = {};
    tls_record::write_record_header(buf, tls_record::kContentTypeAppData, 1);
    uint8_t ct = 0;
    uint16_t len = 0;
    bool ok = tls_record::parse_record_header(buf, ct, len);
    EXPECT_TRUE(ok);
    EXPECT_EQ(len, 1);
}

TEST(TlsRecordHeader, ParseChangeTlsContentType) {
    // Change cipher spec (0x14) should be rejected
    uint8_t buf[tls_record::kRecordHeaderLen] = {
        0x14, 0x03, 0x03, 0x00, 0x01};
    uint8_t ct = 0;
    uint16_t len = 0;
    bool ok = tls_record::parse_record_header(buf, ct, len);
    EXPECT_FALSE(ok);
    EXPECT_EQ(ct, 0x14);
}

TEST(TlsRecordHeader, ParseHandshakeContentType) {
    // Handshake (0x16) should be rejected
    uint8_t buf[tls_record::kRecordHeaderLen] = {
        0x16, 0x03, 0x03, 0x00, 0x01};
    uint8_t ct = 0;
    uint16_t len = 0;
    bool ok = tls_record::parse_record_header(buf, ct, len);
    EXPECT_FALSE(ok);
    EXPECT_EQ(ct, 0x16);
}

// =======================================================================
// TlsRecordCrypto — encrypt/decrypt roundtrip
// =======================================================================

namespace {

/// Create a TlsHotState with deterministic key material for testing.
TlsHotState make_test_hot_state() {
    TlsHotState state{};
    // Fill with deterministic test keys
    for (size_t i = 0; i < tls_const::kAes256KeyLen; ++i) {
        state.write.ki.key[i] = static_cast<uint8_t>(i);
        state.read.ki.key[i] = static_cast<uint8_t>(i + 0x80);
    }
    for (size_t i = 0; i < tls_const::kTls13NonceLen; ++i) {
        state.write.ki.iv[i] = static_cast<uint8_t>(i + 0x40);
        state.read.ki.iv[i] = static_cast<uint8_t>(i + 0xC0);
    }
    state.write.seq = 0;
    state.read.seq = 0;
    return state;
}

/// Create a matching pair: encryptor uses write keys, decryptor uses write keys
/// (same direction) so we can roundtrip in a single test.
TlsHotState make_roundtrip_hot_state() {
    TlsHotState state{};
    for (size_t i = 0; i < tls_const::kAes256KeyLen; ++i) {
        state.write.ki.key[i] = static_cast<uint8_t>(i + 1);
        state.read.ki.key[i] = static_cast<uint8_t>(i + 1);  // same as write
    }
    for (size_t i = 0; i < tls_const::kTls13NonceLen; ++i) {
        state.write.ki.iv[i] = static_cast<uint8_t>(i + 0x10);
        state.read.ki.iv[i] = static_cast<uint8_t>(i + 0x10);  // same as write
    }
    state.write.seq = 0;
    state.read.seq = 0;
    return state;
}

} // namespace

TEST(TlsRecordCrypto, CreateWithValidKeys) {
    auto state = make_test_hot_state();
    auto result = TlsRecordCrypto::create(state);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->write_seq(), 0u);
    EXPECT_EQ(result->read_seq(), 0u);
}

TEST(TlsRecordCrypto, CreateWithUnsupportedKeyLenFails) {
    auto state = make_test_hot_state();
    auto result = TlsRecordCrypto::create(state, 24);  // invalid key len
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("Unsupported"), std::string::npos);
}

TEST(TlsRecordCrypto, EncryptDecryptRoundtrip) {
    auto state = make_roundtrip_hot_state();
    auto crypto = TlsRecordCrypto::create(state);
    ASSERT_TRUE(crypto.has_value());

    // Encrypt a short message
    uint8_t plaintext[] = "Hello, TLS 1.3!";
    constexpr uint16_t plaintext_len = sizeof(plaintext) - 1;

    uint8_t record[TlsEncryptor::encrypted_size(plaintext_len)];
    uint16_t enc_len = crypto->encrypt(plaintext, plaintext_len, record);
    ASSERT_GT(enc_len, 0u);
    EXPECT_EQ(enc_len, TlsEncryptor::encrypted_size(plaintext_len));
    EXPECT_EQ(crypto->write_seq(), 1u);

    // Decrypt the record
    uint8_t decrypted[plaintext_len + 1]{};
    uint16_t dec_len = 0;
    bool ok = crypto->decrypt(record, enc_len, decrypted, dec_len);
    ASSERT_TRUE(ok);
    EXPECT_EQ(dec_len, plaintext_len);
    EXPECT_EQ(std::memcmp(decrypted, plaintext, plaintext_len), 0);
    EXPECT_EQ(crypto->read_seq(), 1u);
}

TEST(TlsRecordCrypto, MultipleRecordsRoundtrip) {
    auto state = make_roundtrip_hot_state();
    auto crypto = TlsRecordCrypto::create(state);
    ASSERT_TRUE(crypto.has_value());

    for (int i = 0; i < 10; ++i) {
        std::string msg = "Message #" + std::to_string(i);
        auto pl = reinterpret_cast<const uint8_t*>(msg.data());
        auto pl_len = static_cast<uint16_t>(msg.size());

        uint8_t record[TlsEncryptor::encrypted_size(512)];
        uint16_t enc_len = crypto->encrypt(pl, pl_len, record);
        ASSERT_GT(enc_len, 0u);

        uint8_t decrypted[512]{};
        uint16_t dec_len = 0;
        bool ok = crypto->decrypt(record, enc_len, decrypted, dec_len);
        ASSERT_TRUE(ok);
        EXPECT_EQ(dec_len, pl_len);
        EXPECT_EQ(std::memcmp(decrypted, pl, pl_len), 0);
    }
    EXPECT_EQ(crypto->write_seq(), 10u);
    EXPECT_EQ(crypto->read_seq(), 10u);
}

TEST(TlsRecordCrypto, EncryptedSizeConstexpr) {
    // Verify the constexpr size calculation
    constexpr auto size_0 = TlsEncryptor::encrypted_size(0);
    // header(5) + content_type(1) + tag(16) = 22
    EXPECT_EQ(size_0, tls_record::kRecordHeaderLen + 1 + tls_record::kAuthTagLen);

    constexpr auto size_100 = TlsEncryptor::encrypted_size(100);
    EXPECT_EQ(size_100, tls_record::kRecordHeaderLen + 101 + tls_record::kAuthTagLen);
}

TEST(TlsRecordCrypto, DecryptTruncatedRecordFails) {
    auto state = make_roundtrip_hot_state();
    auto crypto = TlsRecordCrypto::create(state);
    ASSERT_TRUE(crypto.has_value());

    uint8_t plaintext[] = "test data";
    uint8_t record[TlsEncryptor::encrypted_size(sizeof(plaintext))];
    uint16_t enc_len = crypto->encrypt(plaintext, sizeof(plaintext), record);
    ASSERT_GT(enc_len, 0u);

    // Try to decrypt with truncated record
    uint8_t decrypted[64]{};
    uint16_t dec_len = 0;
    bool ok = crypto->decrypt(record, tls_record::kRecordHeaderLen, decrypted, dec_len);
    EXPECT_FALSE(ok);
}

TEST(TlsRecordCrypto, DecryptTamperedCiphertextFails) {
    auto state = make_roundtrip_hot_state();
    auto crypto = TlsRecordCrypto::create(state);
    ASSERT_TRUE(crypto.has_value());

    uint8_t plaintext[] = "authentic data";
    uint8_t record[TlsEncryptor::encrypted_size(sizeof(plaintext))];
    uint16_t enc_len = crypto->encrypt(plaintext, sizeof(plaintext), record);
    ASSERT_GT(enc_len, 0u);

    // Tamper with ciphertext (flip a bit in the encrypted payload)
    record[tls_record::kRecordHeaderLen + 2] ^= 0xFF;

    uint8_t decrypted[64]{};
    uint16_t dec_len = 0;
    bool ok = crypto->decrypt(record, enc_len, decrypted, dec_len);
    EXPECT_FALSE(ok);  // AEAD authentication should fail
}

TEST(TlsRecordCrypto, EncryptNullPointerWithNonZeroLenReturnsZero) {
    auto state = make_roundtrip_hot_state();
    auto crypto = TlsRecordCrypto::create(state);
    ASSERT_TRUE(crypto.has_value());

    uint8_t record[128];
    uint16_t enc_len = crypto->encrypt(nullptr, 10, record);
    EXPECT_EQ(enc_len, 0u);
}

TEST(TlsRecordCrypto, DecryptNullRecordFails) {
    auto state = make_roundtrip_hot_state();
    auto crypto = TlsRecordCrypto::create(state);
    ASSERT_TRUE(crypto.has_value());

    uint8_t out[64];
    uint16_t out_len = 0;
    bool ok = crypto->decrypt(nullptr, 100, out, out_len);
    EXPECT_FALSE(ok);
}

TEST(TlsRecordCrypto, DecryptNullOutputFails) {
    auto state = make_roundtrip_hot_state();
    auto crypto = TlsRecordCrypto::create(state);
    ASSERT_TRUE(crypto.has_value());

    uint8_t plaintext[] = "test";
    uint8_t record[TlsEncryptor::encrypted_size(sizeof(plaintext))];
    uint16_t enc_len = crypto->encrypt(plaintext, sizeof(plaintext), record);
    ASSERT_GT(enc_len, 0u);

    uint16_t out_len = 0;
    bool ok = crypto->decrypt(record, enc_len, nullptr, out_len);
    EXPECT_FALSE(ok);
}

TEST(TlsRecordCrypto, CreateWith128BitKey) {
    TlsHotState state{};
    // Only first 16 bytes need to be valid for AES-128
    for (size_t i = 0; i < 16; ++i) {
        state.write.ki.key[i] = static_cast<uint8_t>(i + 1);
        state.read.ki.key[i] = static_cast<uint8_t>(i + 1);
    }
    for (size_t i = 0; i < tls_const::kTls13NonceLen; ++i) {
        state.write.ki.iv[i] = static_cast<uint8_t>(i + 0x20);
        state.read.ki.iv[i] = static_cast<uint8_t>(i + 0x20);
    }
    auto crypto = TlsRecordCrypto::create(state, 16);  // AES-128
    ASSERT_TRUE(crypto.has_value());

    // Roundtrip
    uint8_t pt[] = "AES-128 test";
    uint8_t record[TlsEncryptor::encrypted_size(sizeof(pt))];
    uint16_t enc_len = crypto->encrypt(pt, sizeof(pt), record);
    ASSERT_GT(enc_len, 0u);

    uint8_t dec[sizeof(pt) + 1]{};
    uint16_t dec_len = 0;
    EXPECT_TRUE(crypto->decrypt(record, enc_len, dec, dec_len));
    EXPECT_EQ(dec_len, sizeof(pt));
    EXPECT_EQ(std::memcmp(dec, pt, sizeof(pt)), 0);
}

TEST(TlsEncryptor, MoveConstructor) {
    auto state = make_roundtrip_hot_state();
    auto enc_result = TlsEncryptor::create(state);
    ASSERT_TRUE(enc_result.has_value());

    auto enc2 = std::move(*enc_result);
    // enc2 should work, original should be invalidated
    uint8_t pt[] = "move test";
    uint8_t record[TlsEncryptor::encrypted_size(sizeof(pt))];
    uint16_t len = enc2.encrypt(pt, sizeof(pt), record);
    EXPECT_GT(len, 0u);
    EXPECT_EQ(enc2.write_seq(), 1u);
}

TEST(TlsDecryptor, MoveConstructor) {
    auto state = make_roundtrip_hot_state();
    auto dec_result = TlsDecryptor::create(state);
    ASSERT_TRUE(dec_result.has_value());

    auto dec2 = std::move(*dec_result);
    EXPECT_EQ(dec2.read_seq(), 0u);
}

// =======================================================================
// TlsRecordCrypto — boundary payload sizes
// =======================================================================

TEST(TlsRecordCrypto, EncryptDecryptSingleByte) {
    auto state = make_roundtrip_hot_state();
    auto crypto = TlsRecordCrypto::create(state);
    ASSERT_TRUE(crypto.has_value());

    uint8_t pt = 0x42;
    uint8_t record[TlsEncryptor::encrypted_size(1)];
    uint16_t enc_len = crypto->encrypt(&pt, 1, record);
    ASSERT_GT(enc_len, 0u);

    uint8_t dec[1]{};
    uint16_t dec_len = 0;
    EXPECT_TRUE(crypto->decrypt(record, enc_len, dec, dec_len));
    EXPECT_EQ(dec_len, 1u);
    EXPECT_EQ(dec[0], 0x42);
}

TEST(TlsRecordCrypto, EncryptDecryptMaxRecordPayload) {
    auto state = make_roundtrip_hot_state();
    auto crypto = TlsRecordCrypto::create(state);
    ASSERT_TRUE(crypto.has_value());

    // TLS max plaintext fragment = 16384 bytes
    std::vector<uint8_t> pt(tls_const::kMaxRecordPayload, 0xAB);
    std::vector<uint8_t> record(TlsEncryptor::encrypted_size(tls_const::kMaxRecordPayload));
    uint16_t enc_len = crypto->encrypt(pt.data(),
        static_cast<uint16_t>(pt.size()), record.data());
    ASSERT_GT(enc_len, 0u);

    std::vector<uint8_t> dec(tls_const::kMaxRecordPayload);
    uint16_t dec_len = 0;
    EXPECT_TRUE(crypto->decrypt(record.data(), enc_len, dec.data(), dec_len));
    EXPECT_EQ(dec_len, tls_const::kMaxRecordPayload);
    EXPECT_EQ(std::memcmp(dec.data(), pt.data(), pt.size()), 0);
}

TEST(TlsRecordCrypto, EncryptZeroLengthPayload) {
    auto state = make_roundtrip_hot_state();
    auto crypto = TlsRecordCrypto::create(state);
    ASSERT_TRUE(crypto.has_value());

    uint8_t record[TlsEncryptor::encrypted_size(0)];
    uint16_t enc_len = crypto->encrypt(nullptr, 0, record);
    // Zero-length encrypt should succeed (TLS inner content type byte only)
    ASSERT_GT(enc_len, 0u);

    uint8_t dec[1]{};
    uint16_t dec_len = 0;
    EXPECT_TRUE(crypto->decrypt(record, enc_len, dec, dec_len));
    EXPECT_EQ(dec_len, 0u);
}

TEST(TlsRecordCrypto, DecryptWithWrongKeyFails) {
    auto state = make_roundtrip_hot_state();

    // Encrypt with write keys
    auto enc = TlsEncryptor::create(state);
    ASSERT_TRUE(enc.has_value());
    uint8_t pt[] = "secret data";
    uint8_t record[TlsEncryptor::encrypted_size(sizeof(pt))];
    uint16_t enc_len = enc->encrypt(pt, sizeof(pt), record);
    ASSERT_GT(enc_len, 0u);

    // Create decryptor with different keys
    TlsHotState bad_state{};
    for (size_t i = 0; i < tls_const::kAes256KeyLen; ++i) {
        bad_state.read.ki.key[i] = static_cast<uint8_t>(i + 100); // different key
    }
    for (size_t i = 0; i < tls_const::kTls13NonceLen; ++i) {
        bad_state.read.ki.iv[i] = state.read.ki.iv[i]; // same IV
    }
    auto dec = TlsDecryptor::create(bad_state);
    ASSERT_TRUE(dec.has_value());

    uint8_t decrypted[sizeof(pt)]{};
    uint16_t dec_len = 0;
    EXPECT_FALSE(dec->decrypt(record, enc_len, decrypted, dec_len));
}

TEST(TlsRecordCrypto, SequenceIncrementIsCorrect) {
    auto state = make_roundtrip_hot_state();
    auto enc = TlsEncryptor::create(state);
    ASSERT_TRUE(enc.has_value());

    EXPECT_EQ(enc->write_seq(), 0u);

    uint8_t pt[] = "test";
    uint8_t record[TlsEncryptor::encrypted_size(sizeof(pt))];

    enc->encrypt(pt, sizeof(pt), record);
    EXPECT_EQ(enc->write_seq(), 1u);

    enc->encrypt(pt, sizeof(pt), record);
    EXPECT_EQ(enc->write_seq(), 2u);

    enc->encrypt(pt, sizeof(pt), record);
    EXPECT_EQ(enc->write_seq(), 3u);
}
