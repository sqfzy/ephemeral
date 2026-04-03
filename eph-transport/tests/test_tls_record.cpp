/// @file test_tls_record.cpp
/// Tests for TLS record layer utilities: nonce building, header write/parse.

#include <array>
#include <cstring>

#include <gtest/gtest.h>

#include "eph/transport/detail/tls_constants.hpp"

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

TEST(TlsRecordConstants, MaxSequenceNumberIs2Pow24) {
    EXPECT_EQ(tls_record::kMaxSequenceNumber, 1ULL << 24);
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
