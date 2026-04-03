#pragma once

/// @file transport_errors.hpp
/// Transport-layer error types — SendError, ConnectionError, ConnectionErrorInfo.
///
/// Extracted to eph-core so that downstream modules (eph-fix, eph-itch, eph-dpdk)
/// can use these types without pulling in eph-net's TLS/WebSocket dependencies.

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include "eph/core/detail/json_escape.hpp"
#include "eph/core/error_traits.hpp"

namespace eph::net {

// ---------------------------------------------------------------------------
// Connection error types
// ---------------------------------------------------------------------------

/// @brief Categorizes connection failures for programmatic error handling.
///
/// Enables callers to distinguish between different failure modes
/// (e.g., retry on transient TCP errors but abort on TLS certificate rejection).
enum class ConnectionError : uint8_t {
    kInvalidConfig,      ///< TransportConfig validation failed
    kFactoryFailed,      ///< TcpFactory returned an error
    kTcpNotEstablished,  ///< Factory returned a non-established TCP session
    kTlsSessionFailed,   ///< TLS session creation failed
    kTlsHandshakeFailed, ///< TLS handshake failed (cert verification, protocol mismatch)
    kTlsKeyExportFailed, ///< TLS AEAD key export failed
    kWsUpgradeFailed,    ///< WebSocket HTTP upgrade failed (parse error, timeout)
    kWsUpgradeRejected,  ///< Server rejected upgrade (non-101 status code)
    kWsAcceptInvalid,    ///< Sec-WebSocket-Accept validation failed
};

/// @brief Return a human-readable name for a ConnectionError value.
/// @param e  The ConnectionError value to convert.
/// @return A string_view of the error name (e.g., "INVALID_CONFIG", "TLS_HANDSHAKE_FAILED").
[[nodiscard]] constexpr std::string_view connection_error_name(ConnectionError e) noexcept {
    switch (e) {
        case ConnectionError::kInvalidConfig:      return "INVALID_CONFIG";
        case ConnectionError::kFactoryFailed:      return "FACTORY_FAILED";
        case ConnectionError::kTcpNotEstablished:  return "TCP_NOT_ESTABLISHED";
        case ConnectionError::kTlsSessionFailed:   return "TLS_SESSION_FAILED";
        case ConnectionError::kTlsHandshakeFailed: return "TLS_HANDSHAKE_FAILED";
        case ConnectionError::kTlsKeyExportFailed: return "TLS_KEY_EXPORT_FAILED";
        case ConnectionError::kWsUpgradeFailed:    return "WS_UPGRADE_FAILED";
        case ConnectionError::kWsUpgradeRejected:  return "WS_UPGRADE_REJECTED";
        case ConnectionError::kWsAcceptInvalid:    return "WS_ACCEPT_INVALID";
    }
    return "UNKNOWN";
}

/// @brief ADL alias for ErrorEnum concept satisfaction.
/// @param e  The ConnectionError value to convert.
/// @return A string_view of the error name.
[[nodiscard]] constexpr std::string_view error_name(ConnectionError e) noexcept {
    return connection_error_name(e);
}

/// @brief Structured connection error with typed category and detail message.
///
/// Replaces opaque error strings from Transport::create(), enabling
/// callers to match on error category for retry/abort decisions.
struct ConnectionErrorInfo {
    ConnectionError code;       ///< Error category (for programmatic matching)
    std::string     detail;     ///< Human-readable detail (for logging)

    /// @brief Full error message combining category name and detail.
    /// @return A string in the format "[CATEGORY_NAME] detail text".
    [[nodiscard]] std::string message() const {
        return std::format("[{}] {}", connection_error_name(code), detail);
    }

    /// @brief JSON-formatted error info for monitoring system integration.
    /// @return A JSON object string with "code", "detail", and optionally "http_status" fields.
    [[nodiscard]] std::string to_json() const {
        // Only include http_status when it's meaningful
        auto http_str = http_status.has_value()
            ? std::format(",\"http_status\":{}", *http_status)
            : std::string{};
        return std::format(
            "{{\"code\":\"{}\",\"detail\":\"{}\"{}}}",
            eph::core::detail::json_escape(connection_error_name(code)),
            eph::core::detail::json_escape(detail),
            http_str);
    }

    /// HTTP status code from server rejection.
    /// Only populated when code == kWsUpgradeRejected; nullopt otherwise.
    std::optional<int> http_status{};

    /// @brief Defaulted equality -- all fields must match exactly.
    [[nodiscard]] friend bool operator==(const ConnectionErrorInfo&,
                                         const ConnectionErrorInfo&) = default;
};

// ---------------------------------------------------------------------------
// Send result
// ---------------------------------------------------------------------------

/// @brief Result type for Transport::send() and related methods.
///
/// Replaces raw errno return codes with a type-safe enum that
/// enables exhaustive switch checking at compile time.
enum class SendError : int8_t {
    kOk             =  0,   ///< Message enqueued successfully
    kMessageTooLarge = -1,  ///< Payload exceeds MaxPayload
    kNotConnected    = -2,  ///< Transport not running
    kQueueFull       = -3,  ///< TX queue is full (transient backpressure)
    kInvalidUtf8     = -4,  ///< Text frame payload is not valid UTF-8 (RFC 6455 §5.6)
    kInvalidCloseCode = -5, ///< Close status code is not valid per RFC 6455 §7.4
    kNullData         = -6, ///< data pointer is null but len > 0
    kEncryptFailed    = -7, ///< TLS encryption failed (direct send path)
    kTcpSendFailed    = -8, ///< TCP send failed (direct send path)
};

/// @brief Return a human-readable name for a SendError value.
/// @param e  The SendError value to convert.
/// @return A string_view of the error name (e.g., "OK", "QUEUE_FULL").
[[nodiscard]] constexpr std::string_view send_error_name(SendError e) noexcept {
    switch (e) {
        case SendError::kOk:              return "OK";
        case SendError::kMessageTooLarge: return "MESSAGE_TOO_LARGE";
        case SendError::kNotConnected:    return "NOT_CONNECTED";
        case SendError::kQueueFull:       return "QUEUE_FULL";
        case SendError::kInvalidUtf8:     return "INVALID_UTF8";
        case SendError::kInvalidCloseCode: return "INVALID_CLOSE_CODE";
        case SendError::kNullData:         return "NULL_DATA";
        case SendError::kEncryptFailed:    return "ENCRYPT_FAILED";
        case SendError::kTcpSendFailed:    return "TCP_SEND_FAILED";
    }
    return "UNKNOWN";
}

/// @brief ADL alias for ErrorEnum concept satisfaction.
/// @param e  The SendError value to convert.
/// @return A string_view of the error name.
[[nodiscard]] constexpr std::string_view error_name(SendError e) noexcept {
    return send_error_name(e);
}

/// @brief Returns true when an error occurred (non-kOk).
///
/// Enables the idiom:
///   if (!send(...)) handle_error();
/// This inverts the usual boolean convention so that the "not-ok" case
/// reads naturally as a failure check in if-statements.
///
/// @param e  The SendError value to check.
/// @return true if e != kOk (i.e., an error occurred).
constexpr bool operator!(SendError e) noexcept {
    return e != SendError::kOk;
}

} // namespace eph::net

// ---------------------------------------------------------------------------
// std::formatter specializations
// ---------------------------------------------------------------------------

/// @brief std::formatter specialization for SendError (via ErrorEnumFormatter).
template <>
struct std::formatter<eph::net::SendError>
    : eph::net::ErrorEnumFormatter<eph::net::SendError> {};

/// @brief std::formatter specialization for ConnectionError (via ErrorEnumFormatter).
template <>
struct std::formatter<eph::net::ConnectionError>
    : eph::net::ErrorEnumFormatter<eph::net::ConnectionError> {};

/// @brief std::formatter specialization for ConnectionErrorInfo.
///
/// Formats using ConnectionErrorInfo::message(), producing "[CATEGORY] detail".
template <>
struct std::formatter<eph::net::ConnectionErrorInfo> : std::formatter<std::string> {
    /// @brief Format the ConnectionErrorInfo as its full message string.
    /// @param e    The ConnectionErrorInfo to format.
    /// @param ctx  The format context to write into.
    /// @return Iterator past the end of the formatted output.
    auto format(const eph::net::ConnectionErrorInfo& e, auto& ctx) const {
        return std::formatter<std::string>::format(e.message(), ctx);
    }
};
