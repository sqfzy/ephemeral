/// @file test_framers.cpp
/// Tests for WsFramer and RawFramer — the MessageFramer concept adapters.
/// Covers encode/decode roundtrips, error mapping, edge cases, and
/// static_assert concept satisfaction.

#include <array>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "eph/transport/ws_framer.hpp"
#include "eph/transport/raw_framer.hpp"
#include "eph/transport/transport_types.hpp"
#include "eph/transport/detail/websocket.hpp"
#include "eph/transport/detail/message_types.hpp"

using namespace eph::net;

// =======================================================================
// WsFramer — encode/decode roundtrip
// =======================================================================

TEST(WsFramer, EncodeDecodeRoundtrip) {
    WsFramer framer;
    uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t buf[128];
    size_t encoded_len = framer.encode(buf, payload, sizeof(payload),
                                        ws::opcode::kBinary);
    ASSERT_GT(encoded_len, sizeof(payload));  // header + masked payload

    auto result = framer.decode(buf, encoded_len);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload_len, sizeof(payload));
    EXPECT_EQ(result->msg_type, ws::opcode::kBinary);
    EXPECT_FALSE(result->is_control);
    EXPECT_EQ(result->total_len, encoded_len);
}

TEST(WsFramer, TextFrameRoundtrip) {
    WsFramer framer;
    uint8_t payload[] = "hello";
    uint8_t buf[128];
    size_t len = framer.encode(buf, payload, 5, ws::opcode::kText);
    ASSERT_GT(len, 0u);

    auto result = framer.decode(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->msg_type, ws::opcode::kText);
    EXPECT_EQ(result->payload_len, 5u);
}

TEST(WsFramer, EmptyPayloadRoundtrip) {
    WsFramer framer;
    uint8_t buf[32];
    size_t len = framer.encode(buf, nullptr, 0, ws::opcode::kBinary);
    ASSERT_GT(len, 0u);

    auto result = framer.decode(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload_len, 0u);
}

TEST(WsFramer, ControlFrameDetected) {
    WsFramer framer;
    // Build a ping frame and decode it through WsFramer
    uint8_t buf[32];
    size_t len = ws::build_ping_frame(buf);
    ASSERT_GT(len, 0u);

    auto result = framer.decode(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_control);
    EXPECT_EQ(result->msg_type, ws::opcode::kPing);
}

TEST(WsFramer, DecodeIncompleteReturnsError) {
    WsFramer framer;
    uint8_t data[] = {0x82};  // just FIN+opcode byte
    auto result = framer.decode(data, 1);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FrameError::kIncomplete);
}

TEST(WsFramer, DecodeReservedBitsReturnsInvalidFormat) {
    WsFramer framer;
    // RSV1 set: 0x40 | 0x82 = 0xC2
    uint8_t data[] = {0xC2, 0x00};
    auto result = framer.decode(data, sizeof(data));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FrameError::kInvalidFormat);
}

TEST(WsFramer, DecodeReservedOpcodeReturnsInvalidFormat) {
    WsFramer framer;
    // Opcode 0x03 (reserved)
    uint8_t data[] = {0x83, 0x00};
    auto result = framer.decode(data, sizeof(data));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FrameError::kInvalidFormat);
}

TEST(WsFramer, DecodeFragmentedControlReturnsInvalidFormat) {
    WsFramer framer;
    // Ping with FIN=0 (fragmented control)
    uint8_t data[] = {0x09, 0x00};
    auto result = framer.decode(data, sizeof(data));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FrameError::kInvalidFormat);
}

TEST(WsFramer, MaxOverheadIs14) {
    EXPECT_EQ(WsFramer::max_overhead(), 14u);
    EXPECT_EQ(WsFramer::max_overhead(), ws::kMaxFrameHeaderLen);
}

// =======================================================================
// RawFramer — pass-through encode/decode
// =======================================================================

TEST(RawFramer, EncodeDecodeRoundtrip) {
    RawFramer framer;
    uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t buf[128];
    size_t encoded_len = framer.encode(buf, payload, sizeof(payload), 0);
    EXPECT_EQ(encoded_len, sizeof(payload));  // no overhead

    auto result = framer.decode(buf, encoded_len);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload_len, sizeof(payload));
    EXPECT_EQ(result->total_len, sizeof(payload));
    EXPECT_FALSE(result->is_control);
    // Payload should be identical (pass-through)
    EXPECT_EQ(std::memcmp(result->payload, payload, sizeof(payload)), 0);
}

TEST(RawFramer, EncodeZeroLengthReturnsZero) {
    RawFramer framer;
    uint8_t buf[16];
    EXPECT_EQ(framer.encode(buf, nullptr, 0, 0), 0u);
}

TEST(RawFramer, EncodeNullDataReturnsZero) {
    RawFramer framer;
    uint8_t buf[16];
    EXPECT_EQ(framer.encode(buf, nullptr, 10, 0), 0u);
}

TEST(RawFramer, EncodeNullOutReturnsZero) {
    RawFramer framer;
    uint8_t data[] = {1, 2, 3};
    EXPECT_EQ(framer.encode(nullptr, data, sizeof(data), 0), 0u);
}

TEST(RawFramer, DecodeZeroLengthReturnsIncomplete) {
    RawFramer framer;
    uint8_t data[] = {0x01};
    auto result = framer.decode(data, 0);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FrameError::kIncomplete);
}

TEST(RawFramer, MaxOverheadIsZero) {
    EXPECT_EQ(RawFramer::max_overhead(), 0u);
}

// =======================================================================
// Static assertions — concept satisfaction
// =======================================================================

static_assert(MessageFramer<WsFramer>, "WsFramer must satisfy MessageFramer concept");
static_assert(MessageFramer<RawFramer>, "RawFramer must satisfy MessageFramer concept");

// =======================================================================
// TxMessage / RxMessage — static property checks
// =======================================================================

TEST(MessageTypes, TxMessageIsTrivial) {
    using TxMsg = detail::TxMessage<512>;
    // Must be trivially copyable for SPSC queue
    static_assert(std::is_trivially_copyable_v<TxMsg>);
    // Must be cache-line aligned
    static_assert(alignof(TxMsg) == eph::utils::CACHE_LINE_SIZE);
}

TEST(MessageTypes, RxMessageIsTrivial) {
    using RxMsg = detail::RxMessage<512>;
    static_assert(std::is_trivially_copyable_v<RxMsg>);
    static_assert(alignof(RxMsg) == eph::utils::CACHE_LINE_SIZE);
}

TEST(MessageTypes, DefaultOpcodeIsBinary) {
    detail::TxMessage<64> tx{};
    EXPECT_EQ(tx.opcode, ws::opcode::kBinary);
    detail::RxMessage<64> rx{};
    EXPECT_EQ(rx.opcode, ws::opcode::kBinary);
}

TEST(MessageTypes, MaxPayloadStaticAsserts) {
    // Verify the static asserts compile for valid sizes
    using Small = detail::TxMessage<64>;
    using Max = detail::TxMessage<16384>;  // kMaxRecordPayload
    EXPECT_EQ(Small{}.len, 0);
    EXPECT_EQ(Max{}.len, 0);
}

// =======================================================================
// ConnectionErrorInfo
// =======================================================================

TEST(ConnectionErrorInfo, MessageFormatIncludesCodeName) {
    ConnectionErrorInfo err{ConnectionError::kTlsHandshakeFailed, "cert expired"};
    auto msg = err.message();
    EXPECT_NE(msg.find("TLS_HANDSHAKE_FAILED"), std::string::npos);
    EXPECT_NE(msg.find("cert expired"), std::string::npos);
}

TEST(ConnectionErrorInfo, ToJsonContainsFields) {
    ConnectionErrorInfo err{ConnectionError::kFactoryFailed, "timeout"};
    auto j = err.to_json();
    EXPECT_NE(j.find("\"code\":\"FACTORY_FAILED\""), std::string::npos);
    EXPECT_NE(j.find("\"detail\":\"timeout\""), std::string::npos);
}

TEST(ConnectionErrorInfo, HttpStatusIncludedWhenPresent) {
    ConnectionErrorInfo err{ConnectionError::kWsUpgradeRejected, "403"};
    err.http_status = 403;
    auto j = err.to_json();
    EXPECT_NE(j.find("\"http_status\":403"), std::string::npos);
}

TEST(ConnectionErrorInfo, HttpStatusOmittedWhenAbsent) {
    ConnectionErrorInfo err{ConnectionError::kFactoryFailed, "test"};
    auto j = err.to_json();
    EXPECT_EQ(j.find("http_status"), std::string::npos);
}

TEST(ConnectionErrorInfo, EqualityOperator) {
    ConnectionErrorInfo a{ConnectionError::kFactoryFailed, "test"};
    ConnectionErrorInfo b{ConnectionError::kFactoryFailed, "test"};
    EXPECT_EQ(a, b);
    b.detail = "other";
    EXPECT_NE(a, b);
}

// =======================================================================
// SendError
// =======================================================================

TEST(SendError, AllEnumValuesHaveNames) {
    EXPECT_EQ(send_error_name(SendError::kOk), "OK");
    EXPECT_EQ(send_error_name(SendError::kMessageTooLarge), "MESSAGE_TOO_LARGE");
    EXPECT_EQ(send_error_name(SendError::kNotConnected), "NOT_CONNECTED");
    EXPECT_EQ(send_error_name(SendError::kQueueFull), "QUEUE_FULL");
    EXPECT_EQ(send_error_name(SendError::kInvalidUtf8), "INVALID_UTF8");
    EXPECT_EQ(send_error_name(SendError::kInvalidCloseCode), "INVALID_CLOSE_CODE");
    EXPECT_EQ(send_error_name(SendError::kNullData), "NULL_DATA");
    EXPECT_EQ(send_error_name(SendError::kEncryptFailed), "ENCRYPT_FAILED");
    EXPECT_EQ(send_error_name(SendError::kTcpSendFailed), "TCP_SEND_FAILED");
}

TEST(SendError, NegationOperator) {
    EXPECT_FALSE(!SendError::kOk);
    EXPECT_TRUE(!SendError::kQueueFull);
    EXPECT_TRUE(!SendError::kNotConnected);
}

TEST(SendError, FormatAsString) {
    auto s = std::format("{}", SendError::kQueueFull);
    EXPECT_EQ(s, "QUEUE_FULL");
}

// =======================================================================
// ConnectionError
// =======================================================================

TEST(ConnectionError, AllEnumValuesHaveNames) {
    EXPECT_EQ(connection_error_name(ConnectionError::kInvalidConfig), "INVALID_CONFIG");
    EXPECT_EQ(connection_error_name(ConnectionError::kFactoryFailed), "FACTORY_FAILED");
    EXPECT_EQ(connection_error_name(ConnectionError::kTcpNotEstablished), "TCP_NOT_ESTABLISHED");
    EXPECT_EQ(connection_error_name(ConnectionError::kTlsSessionFailed), "TLS_SESSION_FAILED");
    EXPECT_EQ(connection_error_name(ConnectionError::kTlsHandshakeFailed), "TLS_HANDSHAKE_FAILED");
    EXPECT_EQ(connection_error_name(ConnectionError::kTlsKeyExportFailed), "TLS_KEY_EXPORT_FAILED");
    EXPECT_EQ(connection_error_name(ConnectionError::kWsUpgradeFailed), "WS_UPGRADE_FAILED");
    EXPECT_EQ(connection_error_name(ConnectionError::kWsUpgradeRejected), "WS_UPGRADE_REJECTED");
    EXPECT_EQ(connection_error_name(ConnectionError::kWsAcceptInvalid), "WS_ACCEPT_INVALID");
}

TEST(ConnectionError, FormatAsString) {
    auto s = std::format("{}", ConnectionError::kTlsHandshakeFailed);
    EXPECT_EQ(s, "TLS_HANDSHAKE_FAILED");
}
