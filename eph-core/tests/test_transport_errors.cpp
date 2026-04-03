/// @file test_transport_errors.cpp
/// Unit tests for transport error types: SendError, ConnectionError, ConnectionErrorInfo.

#include <format>
#include <optional>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "eph/core/transport_errors.hpp"

using namespace eph::net;

// ============================================================================
// SendError
// ============================================================================

TEST(SendError, AllNamesAreNonEmpty) {
    // Every enum value should have a human-readable name.
    EXPECT_FALSE(send_error_name(SendError::kOk).empty());
    EXPECT_FALSE(send_error_name(SendError::kMessageTooLarge).empty());
    EXPECT_FALSE(send_error_name(SendError::kNotConnected).empty());
    EXPECT_FALSE(send_error_name(SendError::kQueueFull).empty());
    EXPECT_FALSE(send_error_name(SendError::kInvalidUtf8).empty());
    EXPECT_FALSE(send_error_name(SendError::kInvalidCloseCode).empty());
    EXPECT_FALSE(send_error_name(SendError::kNullData).empty());
    EXPECT_FALSE(send_error_name(SendError::kEncryptFailed).empty());
    EXPECT_FALSE(send_error_name(SendError::kTcpSendFailed).empty());
}

TEST(SendError, OkNameIsOK) {
    EXPECT_EQ(send_error_name(SendError::kOk), "OK");
}

TEST(SendError, NegationOperatorOkIsFalse) {
    // !kOk should be false (no error)
    EXPECT_FALSE(!SendError::kOk);
}

TEST(SendError, NegationOperatorErrorIsTrue) {
    EXPECT_TRUE(!SendError::kMessageTooLarge);
    EXPECT_TRUE(!SendError::kNotConnected);
    EXPECT_TRUE(!SendError::kQueueFull);
    EXPECT_TRUE(!SendError::kInvalidUtf8);
    EXPECT_TRUE(!SendError::kInvalidCloseCode);
    EXPECT_TRUE(!SendError::kNullData);
}

TEST(SendError, FormatProducesName) {
    auto s = std::format("{}", SendError::kQueueFull);
    EXPECT_EQ(s, "QUEUE_FULL");
}

TEST(SendError, ErrorNameAdlSatisfiesConcept) {
    // Compile-time check that SendError satisfies ErrorEnum.
    static_assert(eph::core::ErrorEnum<SendError>);
}

// ============================================================================
// ConnectionError
// ============================================================================

TEST(ConnectionError, AllNamesAreNonEmpty) {
    EXPECT_FALSE(connection_error_name(ConnectionError::kInvalidConfig).empty());
    EXPECT_FALSE(connection_error_name(ConnectionError::kFactoryFailed).empty());
    EXPECT_FALSE(connection_error_name(ConnectionError::kTcpNotEstablished).empty());
    EXPECT_FALSE(connection_error_name(ConnectionError::kTlsSessionFailed).empty());
    EXPECT_FALSE(connection_error_name(ConnectionError::kTlsHandshakeFailed).empty());
    EXPECT_FALSE(connection_error_name(ConnectionError::kTlsKeyExportFailed).empty());
    EXPECT_FALSE(connection_error_name(ConnectionError::kWsUpgradeFailed).empty());
    EXPECT_FALSE(connection_error_name(ConnectionError::kWsUpgradeRejected).empty());
    EXPECT_FALSE(connection_error_name(ConnectionError::kWsAcceptInvalid).empty());
}

TEST(ConnectionError, FormatProducesName) {
    auto s = std::format("{}", ConnectionError::kTlsHandshakeFailed);
    EXPECT_EQ(s, "TLS_HANDSHAKE_FAILED");
}

TEST(ConnectionError, ErrorNameAdlSatisfiesConcept) {
    static_assert(eph::core::ErrorEnum<ConnectionError>);
}

// ============================================================================
// ConnectionErrorInfo
// ============================================================================

TEST(ConnectionErrorInfo, MessageCombinesCodeAndDetail) {
    ConnectionErrorInfo info{ConnectionError::kFactoryFailed, "timeout after 5s"};
    auto msg = info.message();
    EXPECT_NE(msg.find("FACTORY_FAILED"), std::string::npos);
    EXPECT_NE(msg.find("timeout after 5s"), std::string::npos);
}

TEST(ConnectionErrorInfo, ToJsonProducesValidStructure) {
    ConnectionErrorInfo info{ConnectionError::kTlsHandshakeFailed, "cert expired"};
    auto json = info.to_json();
    EXPECT_NE(json.find("\"code\":\"TLS_HANDSHAKE_FAILED\""), std::string::npos);
    EXPECT_NE(json.find("\"detail\":\"cert expired\""), std::string::npos);
    // No http_status when not set
    EXPECT_EQ(json.find("http_status"), std::string::npos);
}

TEST(ConnectionErrorInfo, ToJsonIncludesHttpStatusWhenSet) {
    ConnectionErrorInfo info{
        ConnectionError::kWsUpgradeRejected,
        "server returned 403",
        403
    };
    auto json = info.to_json();
    EXPECT_NE(json.find("\"http_status\":403"), std::string::npos);
}

TEST(ConnectionErrorInfo, ToJsonEscapesSpecialCharacters) {
    // Detail with quotes and backslashes should be escaped
    ConnectionErrorInfo info{ConnectionError::kFactoryFailed, "msg with \"quotes\" and \\back"};
    auto json = info.to_json();
    // The escaped JSON should not contain raw unescaped quotes in the detail
    // (the json_escape function handles this)
    EXPECT_NE(json.find("\\\"quotes\\\""), std::string::npos);
}

TEST(ConnectionErrorInfo, EqualityOperator) {
    ConnectionErrorInfo a{ConnectionError::kFactoryFailed, "x"};
    ConnectionErrorInfo b{ConnectionError::kFactoryFailed, "x"};
    ConnectionErrorInfo c{ConnectionError::kInvalidConfig, "x"};
    ConnectionErrorInfo d{ConnectionError::kFactoryFailed, "y"};

    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_NE(a, d);
}

TEST(ConnectionErrorInfo, EqualityWithHttpStatus) {
    ConnectionErrorInfo a{ConnectionError::kWsUpgradeRejected, "rejected", 403};
    ConnectionErrorInfo b{ConnectionError::kWsUpgradeRejected, "rejected", 403};
    ConnectionErrorInfo c{ConnectionError::kWsUpgradeRejected, "rejected", 500};
    ConnectionErrorInfo d{ConnectionError::kWsUpgradeRejected, "rejected"};

    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_NE(a, d);
}

TEST(ConnectionErrorInfo, FormatProducesMessage) {
    ConnectionErrorInfo info{ConnectionError::kInvalidConfig, "missing host"};
    auto s = std::format("{}", info);
    EXPECT_EQ(s, info.message());
}

TEST(ConnectionErrorInfo, EmptyDetail) {
    ConnectionErrorInfo info{ConnectionError::kTcpNotEstablished, ""};
    auto msg = info.message();
    EXPECT_NE(msg.find("TCP_NOT_ESTABLISHED"), std::string::npos);
}
