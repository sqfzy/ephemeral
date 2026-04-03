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
