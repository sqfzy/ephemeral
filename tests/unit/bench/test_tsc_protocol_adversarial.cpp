/// @file test_tsc_protocol_adversarial.cpp
/// Adversarial / boundary tests for the bench tsc_protocol JSON parser.
///
/// The pre-existing test_tsc_protocol.cpp covers happy-path parse_T,
/// parse_T_recv, parse_T_send and the T_recv/T_send disambiguation.
/// This file fills in the boundary cases the original tests skipped:
///
/// * Whitespace handling (tab, newline, multiple spaces)
/// * Negative / non-numeric values
/// * uint64_t overflow boundary
/// * T appearing inside an escaped string value (false positive risk)
/// * Empty / single-character buffers
/// * Multiple T occurrences (which one wins?)

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "core/tsc_protocol.hpp"

using namespace bench;

// ═══════════════════════════════════════════════════════════════════════
// Whitespace variants after the colon
// ═══════════════════════════════════════════════════════════════════════

TEST(TscParseAdv, ParseTHandlesSpaceAfterColon) {
    std::string json = R"({"T": 42})";
    EXPECT_EQ(tsc::parse_T(reinterpret_cast<const uint8_t*>(json.data()),
                            json.size()), 42u);
}

TEST(TscParseAdv, ParseTMultipleSpacesAfterColon) {
    std::string json = R"({"T":     42})";
    EXPECT_EQ(tsc::parse_T(reinterpret_cast<const uint8_t*>(json.data()),
                            json.size()), 42u);
}

TEST(TscParseAdv, ParseTTabAfterColon) {
    // The parser only consumes ASCII space characters — tab is not
    // recognized as whitespace.  Pin the current behavior so any
    // future broadening is intentional.
    std::string json = "{\"T\":\t42}";
    EXPECT_EQ(tsc::parse_T(reinterpret_cast<const uint8_t*>(json.data()),
                            json.size()), 0u)
        << "current parser does not skip tab whitespace; pinned";
}

// ═══════════════════════════════════════════════════════════════════════
// Numeric edge cases
// ═══════════════════════════════════════════════════════════════════════

TEST(TscParseAdv, ParseTNegativeReturnsZero) {
    // '-' is not a digit, parser stops immediately, val stays 0.
    std::string json = R"({"T":-1})";
    EXPECT_EQ(tsc::parse_T(reinterpret_cast<const uint8_t*>(json.data()),
                            json.size()), 0u);
}

TEST(TscParseAdv, ParseTLeadingZeroAccepted) {
    std::string json = R"({"T":007})";
    EXPECT_EQ(tsc::parse_T(reinterpret_cast<const uint8_t*>(json.data()),
                            json.size()), 7u);
}

TEST(TscParseAdv, ParseTDecimalStopsAtDot) {
    // val = 1, parser hits '.', stops.
    std::string json = R"({"T":1.5})";
    EXPECT_EQ(tsc::parse_T(reinterpret_cast<const uint8_t*>(json.data()),
                            json.size()), 1u);
}

TEST(TscParseAdv, ParseTUint64MaxValue) {
    // 2^64 - 1 = 18446744073709551615
    std::string json = R"({"T":18446744073709551615})";
    EXPECT_EQ(tsc::parse_T(reinterpret_cast<const uint8_t*>(json.data()),
                            json.size()),
              18446744073709551615ULL);
}

TEST(TscParseAdv, ParseTOverflowsSilently) {
    // 2^64 + 1 — the parser does (val * 10 + digit) without overflow
    // check.  Pin the current silent-overflow behavior so any future
    // hardening is intentional.
    std::string json = R"({"T":18446744073709551616})";
    auto v = tsc::parse_T(reinterpret_cast<const uint8_t*>(json.data()),
                           json.size());
    // Result is well-defined (uint64_t wraps), but garbage compared
    // to the input.  Just verify it doesn't crash and returns *some*
    // uint64_t value.
    (void)v;
}

TEST(TscParseAdv, ParseTValueZero) {
    std::string json = R"({"T":0})";
    EXPECT_EQ(tsc::parse_T(reinterpret_cast<const uint8_t*>(json.data()),
                            json.size()), 0u);
}

// ═══════════════════════════════════════════════════════════════════════
// Value position / context
// ═══════════════════════════════════════════════════════════════════════

TEST(TscParseAdv, ParseTAtBufferStartWithoutBraceReturnsZero) {
    // The parser only matches `{"T":` or `,"T":`.  A bare `"T":1` at
    // the start of the buffer doesn't match either prefix.
    std::string json = R"("T":42)";
    EXPECT_EQ(tsc::parse_T(reinterpret_cast<const uint8_t*>(json.data()),
                            json.size()), 0u);
}

TEST(TscParseAdv, ParseTInArrayOfObjectsMatchesFirst) {
    // [{"T":111},{"T":222}]
    // The parser finds `{"T":` first → returns 111.
    std::string json = R"([{"T":111},{"T":222}])";
    EXPECT_EQ(tsc::parse_T(reinterpret_cast<const uint8_t*>(json.data()),
                            json.size()), 111u);
}

TEST(TscParseAdv, ParseTAfterCommaMatchesFirstOccurrence) {
    // {"a":1,"T":50,"T":60}  — parse_T should return the first
    // because find() returns the first match.
    std::string json = R"({"a":1,"T":50,"T":60})";
    EXPECT_EQ(tsc::parse_T(reinterpret_cast<const uint8_t*>(json.data()),
                            json.size()), 50u);
}

// ═══════════════════════════════════════════════════════════════════════
// String-value false positives
// ═══════════════════════════════════════════════════════════════════════

TEST(TscParseAdv, ParseTSafeAgainstEscapedQuotesInString) {
    // Adversarial JSON: a string field whose VALUE contains the
    // ESCAPED bytes `\",\"T\":99` (i.e. valid JSON that decodes to
    // a string containing `","T":99`).  The substring parser
    // searches for the literal bytes `,"T":` — but the on-wire
    // bytes are `,\"T\":` which contains backslashes between the
    // comma and quote, so the substring pattern fails to match.
    //
    // This is the saving grace of the substring approach: any valid
    // JSON encoder MUST escape internal quotes, and the escape
    // characters break the search pattern.  Pin this safety
    // invariant.
    std::string json = R"({"name":"foo,\"T\":99","real":1})";
    EXPECT_EQ(tsc::parse_T(reinterpret_cast<const uint8_t*>(json.data()),
                            json.size()), 0u)
        << "JSON-escaped quote in string value must NOT trigger false "
           "positive — the backslash breaks the substring pattern";
}

// ═══════════════════════════════════════════════════════════════════════
// T_recv / T_send key matching
// ═══════════════════════════════════════════════════════════════════════

TEST(TscParseAdv, ParseTRecvWithoutColonReturnsZero) {
    // T_recv must be followed by ':' immediately (the search key
    // is `"T_recv":`).
    std::string json = R"({"T_recv ":99})";
    EXPECT_EQ(tsc::parse_T_recv(reinterpret_cast<const uint8_t*>(json.data()),
                                 json.size()), 0u);
}

TEST(TscParseAdv, ParseTRecvWithSpaceAfterColon) {
    std::string json = R"({"T_recv": 99})";
    EXPECT_EQ(tsc::parse_T_recv(reinterpret_cast<const uint8_t*>(json.data()),
                                 json.size()), 99u);
}

TEST(TscParseAdv, ParseTSendCaseSensitive) {
    // The key search is case-sensitive.  Lowercase t_send must NOT match.
    std::string json = R"({"t_send":99})";
    EXPECT_EQ(tsc::parse_T_send(reinterpret_cast<const uint8_t*>(json.data()),
                                 json.size()), 0u);
}

// ═══════════════════════════════════════════════════════════════════════
// Buffer boundaries
// ═══════════════════════════════════════════════════════════════════════

TEST(TscParseAdv, EmptyBufferReturnsZero) {
    std::string json;
    EXPECT_EQ(tsc::parse_T(reinterpret_cast<const uint8_t*>(json.data()),
                            json.size()), 0u);
    EXPECT_EQ(tsc::parse_T_recv(reinterpret_cast<const uint8_t*>(json.data()),
                                 json.size()), 0u);
    EXPECT_EQ(tsc::parse_T_send(reinterpret_cast<const uint8_t*>(json.data()),
                                 json.size()), 0u);
}

TEST(TscParseAdv, KeyAtEndWithNoValue) {
    // `{"T":` at the end of buffer with no value bytes — parser stops
    // immediately, returns 0.
    std::string json = R"({"T":)";
    EXPECT_EQ(tsc::parse_T(reinterpret_cast<const uint8_t*>(json.data()),
                            json.size()), 0u);
}

TEST(TscParseAdv, KeyAtEndWithSpaceOnly) {
    std::string json = R"({"T": )";
    EXPECT_EQ(tsc::parse_T(reinterpret_cast<const uint8_t*>(json.data()),
                            json.size()), 0u);
}

// ═══════════════════════════════════════════════════════════════════════
// parse_uint64_after — direct primitive coverage
// ═══════════════════════════════════════════════════════════════════════

TEST(TscParseAdv, ParseUint64AfterMissingKeyReturnsZero) {
    EXPECT_EQ(tsc::detail::parse_uint64_after(
        "{\"x\":1}", "\"y\":"), 0u);
}

TEST(TscParseAdv, ParseUint64AfterPartialNumberAtEnd) {
    // Buffer ends mid-number — parser stops at end, returns
    // accumulated value.
    EXPECT_EQ(tsc::detail::parse_uint64_after(
        "{\"k\":12345", "\"k\":"), 12345u);
}

TEST(TscParseAdv, ParseUint64AfterImmediateNonDigitReturnsZero) {
    EXPECT_EQ(tsc::detail::parse_uint64_after(
        "{\"k\":abc}", "\"k\":"), 0u);
}
