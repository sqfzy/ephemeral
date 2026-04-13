/// @file test_error.cpp
/// Unit tests for eph::core::Error and eph::core::ErrorInfo.

#include <cstring>
#include <expected>
#include <type_traits>

#include <gtest/gtest.h>

#include "eph/core/error.hpp"

using eph::core::Error;
using eph::core::ErrorInfo;
using eph::core::error_name;

// ============================================================================
// Enum layout
// ============================================================================

TEST(Error, IsUint8Underlying) {
    // Error must be a tight 1-byte enum — std::expected<T, ErrorInfo> size
    // contracts depend on it, and we rely on this elsewhere in the stack.
    static_assert(std::is_same_v<std::underlying_type_t<Error>, uint8_t>);
}

TEST(Error, OkIsZero) {
    // Ok == 0 lets callers do `if (auto e = ...) ...` with std::expected.
    EXPECT_EQ(static_cast<uint8_t>(Error::Ok), 0u);
}

// ============================================================================
// error_name — every enum value must produce a non-empty, non-UNKNOWN name
// ============================================================================

// Helper macro — keeps each value on its own line for easy diff review when
// new enum values are added.
#define EXPECT_NAMED(val, expected)                                \
    do {                                                          \
        const char* n = error_name(Error::val);                   \
        ASSERT_NE(n, nullptr);                                    \
        EXPECT_STREQ(n, expected);                                \
    } while (0)

TEST(ErrorName, CoversEveryEnumValue) {
    EXPECT_NAMED(Ok,                 "OK");
    EXPECT_NAMED(ConnectFailed,      "CONNECT_FAILED");
    EXPECT_NAMED(Disconnected,       "DISCONNECTED");
    EXPECT_NAMED(Timeout,            "TIMEOUT");
    EXPECT_NAMED(NotAttached,        "NOT_ATTACHED");
    EXPECT_NAMED(TlsHandshakeFailed, "TLS_HANDSHAKE_FAILED");
    EXPECT_NAMED(TlsRecordBad,       "TLS_RECORD_BAD");
    EXPECT_NAMED(TlsCipherFailed,    "TLS_CIPHER_FAILED");
    EXPECT_NAMED(WsHandshakeFailed,  "WS_HANDSHAKE_FAILED");
    EXPECT_NAMED(WsFrameBad,         "WS_FRAME_BAD");
    EXPECT_NAMED(WsCloseReceived,    "WS_CLOSE_RECEIVED");
    EXPECT_NAMED(CodecNeedMoreData,  "CODEC_NEED_MORE_DATA");
    EXPECT_NAMED(CodecBad,           "CODEC_BAD");
    EXPECT_NAMED(CodecOverflow,      "CODEC_OVERFLOW");
    EXPECT_NAMED(WouldBlock,         "WOULD_BLOCK");
    EXPECT_NAMED(NoData,             "NO_DATA");
    EXPECT_NAMED(BufferFull,         "BUFFER_FULL");
    EXPECT_NAMED(InvalidConfig,      "INVALID_CONFIG");
    EXPECT_NAMED(OutOfMemory,        "OUT_OF_MEMORY");
    // HTTP CONNECT proxy error triad.
    EXPECT_NAMED(ProxyConnectFailed,  "PROXY_CONNECT_FAILED");
    EXPECT_NAMED(ProxyHandshakeFailed,"PROXY_HANDSHAKE_FAILED");
    EXPECT_NAMED(ProxyAuthRequired,   "PROXY_AUTH_REQUIRED");
}

// ============================================================================
// Proxy error triad — these get their own TESTs so coverage counters see them
// distinctly from the big sweep above.
// ============================================================================

TEST(ErrorNameProxy, ProxyConnectFailedHasStableName) {
    EXPECT_STREQ(error_name(Error::ProxyConnectFailed), "PROXY_CONNECT_FAILED");
}

TEST(ErrorNameProxy, ProxyHandshakeFailedHasStableName) {
    EXPECT_STREQ(error_name(Error::ProxyHandshakeFailed), "PROXY_HANDSHAKE_FAILED");
}

TEST(ErrorNameProxy, ProxyAuthRequiredHasStableName) {
    EXPECT_STREQ(error_name(Error::ProxyAuthRequired), "PROXY_AUTH_REQUIRED");
}

#undef EXPECT_NAMED

TEST(ErrorName, UnknownValueReturnsUnknown) {
    // Reinterpret a value outside the enum range — error_name must not crash
    // and should return the sentinel "UNKNOWN".
    auto bogus = static_cast<Error>(0xFE);
    EXPECT_STREQ(error_name(bogus), "UNKNOWN");
}

TEST(ErrorName, IsConstexpr) {
    // Compile-time usable — a canary that forces constexpr evaluation.
    constexpr const char* n = error_name(Error::Timeout);
    static_assert(n != nullptr);
    EXPECT_STREQ(n, "TIMEOUT");
}

// ============================================================================
// ErrorInfo — construction and semantics
// ============================================================================

TEST(ErrorInfo, DefaultDetailIsEmptyString) {
    // Constructing without detail must still yield a valid, non-null pointer.
    constexpr ErrorInfo ei{Error::Disconnected};
    EXPECT_EQ(ei.code, Error::Disconnected);
    ASSERT_NE(ei.detail, nullptr);
    EXPECT_STREQ(ei.detail, "");
}

TEST(ErrorInfo, NullptrDetailCoercedToEmptyString) {
    // Defensive: a nullptr passed as detail must not produce a dangling read
    // in downstream loggers. We promise detail is never nullptr.
    ErrorInfo ei{Error::CodecBad, nullptr};
    ASSERT_NE(ei.detail, nullptr);
    EXPECT_STREQ(ei.detail, "");
}

TEST(ErrorInfo, StoresStringLiteralDetail) {
    constexpr ErrorInfo ei{Error::ConnectFailed, "TCP RST from peer"};
    EXPECT_EQ(ei.code, Error::ConnectFailed);
    EXPECT_STREQ(ei.detail, "TCP RST from peer");
}

TEST(ErrorInfo, EqualityMatchesOnCodeAndPointer) {
    // Two ErrorInfos constructed from the same literal share the same pointer
    // (string literal de-duplication) and should compare equal.
    constexpr ErrorInfo a{Error::Timeout, "x"};
    constexpr ErrorInfo b{Error::Timeout, "x"};
    EXPECT_TRUE(a == b);

    constexpr ErrorInfo c{Error::Disconnected, "x"};
    EXPECT_FALSE(a == c);
}

TEST(ErrorInfo, IsTriviallyCopyable) {
    // Fail-fast check that we stay allocation-free and ABI-stable.
    static_assert(std::is_trivially_copyable_v<ErrorInfo>);
    static_assert(std::is_nothrow_copy_constructible_v<ErrorInfo>);
}

TEST(ErrorInfo, UsableAsExpectedError) {
    // Smoke test: wrapping in std::expected must compile and round-trip.
    auto make = [](bool fail) -> std::expected<int, ErrorInfo> {
        if (fail) return std::unexpected(ErrorInfo{Error::InvalidConfig, "bad port"});
        return 42;
    };
    auto ok = make(false);
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(*ok, 42);

    auto err = make(true);
    ASSERT_FALSE(err.has_value());
    EXPECT_EQ(err.error().code, Error::InvalidConfig);
    EXPECT_STREQ(err.error().detail, "bad port");
}
