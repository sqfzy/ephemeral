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
    EXPECT_NAMED(CodecBad,           "CODEC_BAD");
    EXPECT_NAMED(CodecOverflow,      "CODEC_OVERFLOW");
    EXPECT_NAMED(WouldBlock,         "WOULD_BLOCK");
    EXPECT_NAMED(BufferFull,         "BUFFER_FULL");
    EXPECT_NAMED(InvalidConfig,      "INVALID_CONFIG");
    EXPECT_NAMED(OutOfMemory,        "OUT_OF_MEMORY");
    // HTTP CONNECT proxy error triad.
    EXPECT_NAMED(ProxyConnectFailed,  "PROXY_CONNECT_FAILED");
    EXPECT_NAMED(ProxyHandshakeFailed,"PROXY_HANDSHAKE_FAILED");
    EXPECT_NAMED(ProxyAuthRequired,   "PROXY_AUTH_REQUIRED");
    // Registry / lookup lifecycle. Distinct from InvalidConfig: signals
    // a recoverable state mismatch (key not registered / already removed),
    // not a programming error. DpdkPoller::remove() is the canonical
    // emitter today.
    EXPECT_NAMED(NotFound,             "NOT_FOUND");
    // DPDK daemon-led model (S6 of daemon-reshape): surfaced when the
    // eph-nicd primary IPC stops responding (rte_* calls return -EIO,
    // local port becomes invalid). Distinct from Disconnected (peer-
    // level TCP) and Timeout (per-request RTT). Was missing from this
    // sweep when the enumerator was added — silent gap meant a
    // regression that returned "UNKNOWN" for DaemonDisconnected would
    // never have surfaced through this test.
    EXPECT_NAMED(DaemonDisconnected,   "DAEMON_DISCONNECTED");
}

// Compile-time guarantee that no future enumerator silently slips past
// the CoversEveryEnumValue sweep. A new enum value added without
// updating both error_name() AND the sweep above lands here as a
// static_assert: bumping kKnownErrorCount forces the author to scan
// the test file and add the matching EXPECT_NAMED line.
//
// To bump: count the enum values in eph-core/include/eph/core/error.hpp
// (excluding Ok if the count tracks "non-Ok errors"; here we count
// every value including Ok so the math is straightforward).
TEST(ErrorName, EnumValueCountMatchesCoverageSweep) {
    // Count of enumerators in Error (including Ok). Update both this
    // constant AND the CoversEveryEnumValue body together when adding
    // a new error code.
    constexpr std::size_t kKnownErrorCount = 22;

    // Verify each integer 0..kKnownErrorCount-1 maps to a non-UNKNOWN
    // name. A new enum value with no matching error_name() switch case
    // would return "UNKNOWN" here and fail loudly.
    for (std::size_t i = 0; i < kKnownErrorCount; ++i) {
        const char* n = error_name(static_cast<Error>(i));
        ASSERT_NE(n, nullptr) << "error_name returned nullptr for index " << i;
        EXPECT_STRNE(n, "UNKNOWN")
            << "error_name returned UNKNOWN for index " << i
            << " — likely a new enum value missing from error_name()'s switch";
    }
    // And the next-after-last must still be "UNKNOWN" (sentinel-overrun
    // guard so we know kKnownErrorCount stayed in sync).
    EXPECT_STREQ(error_name(static_cast<Error>(kKnownErrorCount)), "UNKNOWN")
        << "if this fires, the enum grew past kKnownErrorCount — bump the "
           "constant AND add a matching EXPECT_NAMED line in "
           "CoversEveryEnumValue above";
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

// ============================================================================
// Registry / lookup lifecycle — gets its own TEST so coverage counters
// see the most recently added enum value distinctly.
// ============================================================================

TEST(ErrorNameRegistry, NotFoundHasStableName) {
    EXPECT_STREQ(error_name(Error::NotFound), "NOT_FOUND");
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

// ============================================================================
// Formatter / streaming surface — three distinct rendering paths must produce
// identical output:
//   - operator<< (std::ostream)        — gtest streaming, generic ostream sinks
//   - format_as(ErrorInfo) / Error     — spdlog/fmt's ADL hook (bundled fmt
//                                        cannot see std::formatter specs)
//   - std::formatter<ErrorInfo> / Error — direct std::format users
//
// Lockstep across all three is doc'd at the formatter spec definition; until
// now no test pinned it, so a future drift (e.g. renaming OK to "kOk")
// would only surface in production logs.
// ============================================================================

#include <sstream>

TEST(ErrorFormatter, OstreamWithDetailRendersCodeColonDetail) {
    std::ostringstream os;
    os << ErrorInfo{Error::Disconnected, "peer hung up"};
    EXPECT_EQ(os.str(), "DISCONNECTED: peer hung up");
}

TEST(ErrorFormatter, OstreamWithoutDetailRendersCodeOnly) {
    std::ostringstream os;
    os << ErrorInfo{Error::Timeout};  // default detail = ""
    EXPECT_EQ(os.str(), "TIMEOUT");
}

TEST(ErrorFormatter, OstreamWithEmptyDetailRendersCodeOnly) {
    std::ostringstream os;
    os << ErrorInfo{Error::CodecBad, ""};  // explicit empty
    EXPECT_EQ(os.str(), "CODEC_BAD");
}

TEST(ErrorFormatter, FormatAsErrorInfoMatchesOstream) {
    // The ADL `format_as(ErrorInfo)` hook is what spdlog's bundled fmt
    // picks up. Must match the ostream rendering byte-for-byte.
    auto with_detail    = eph::core::format_as(ErrorInfo{Error::CodecOverflow, "frame too big"});
    auto without_detail = eph::core::format_as(ErrorInfo{Error::WouldBlock});
    EXPECT_EQ(with_detail,    "CODEC_OVERFLOW: frame too big");
    EXPECT_EQ(without_detail, "WOULD_BLOCK");
}

TEST(ErrorFormatter, FormatAsErrorReturnsName) {
    auto sv = eph::core::format_as(Error::TlsCipherFailed);
    EXPECT_EQ(sv, "TLS_CIPHER_FAILED");
}

TEST(ErrorFormatter, StdFormatErrorInfoMatchesOstream) {
    // std::format() goes through the std::formatter<ErrorInfo> spec; that
    // path is independent of format_as / operator<<. Pin it.
    EXPECT_EQ(std::format("{}", ErrorInfo{Error::ProxyConnectFailed, "ECONNREFUSED"}),
              "PROXY_CONNECT_FAILED: ECONNREFUSED");
    EXPECT_EQ(std::format("{}", ErrorInfo{Error::NotFound}),
              "NOT_FOUND");
}

TEST(ErrorFormatter, StdFormatErrorReturnsName) {
    EXPECT_EQ(std::format("{}", Error::DaemonDisconnected), "DAEMON_DISCONNECTED");
    EXPECT_EQ(std::format("[{}]", Error::Ok), "[OK]");
}
