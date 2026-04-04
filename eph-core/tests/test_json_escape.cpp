/// @file test_json_escape.cpp
/// Unit tests for eph::core::detail::json_escape() — RFC 8259 S7 escaping.

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "eph/core/detail/json_escape.hpp"

using eph::core::detail::json_escape;

// ----------------------------------------------------------------------------
// Empty and plain ASCII (fast path)
// ----------------------------------------------------------------------------

TEST(JsonEscape, EmptyStringReturnsEmpty) {
    EXPECT_EQ(json_escape(""), "");
}

TEST(JsonEscape, NormalAsciiPassedThrough) {
    const std::string input = "Hello, World! 123 abc";
    EXPECT_EQ(json_escape(input), input);
}

// ----------------------------------------------------------------------------
// Quote and backslash escaping
// ----------------------------------------------------------------------------

TEST(JsonEscape, DoubleQuoteEscaped) {
    EXPECT_EQ(json_escape("say \"hello\""), "say \\\"hello\\\"");
}

TEST(JsonEscape, BackslashEscaped) {
    EXPECT_EQ(json_escape("a\\b"), "a\\\\b");
}

// ----------------------------------------------------------------------------
// Control characters
// ----------------------------------------------------------------------------

TEST(JsonEscape, NewlineEscaped) {
    EXPECT_EQ(json_escape("line1\nline2"), "line1\\nline2");
}

TEST(JsonEscape, CarriageReturnEscaped) {
    EXPECT_EQ(json_escape("a\rb"), "a\\rb");
}

TEST(JsonEscape, TabEscaped) {
    EXPECT_EQ(json_escape("a\tb"), "a\\tb");
}

TEST(JsonEscape, BackspaceEscaped) {
    EXPECT_EQ(json_escape("a\bb"), "a\\bb");
}

TEST(JsonEscape, FormFeedEscaped) {
    EXPECT_EQ(json_escape("a\fb"), "a\\fb");
}

// ----------------------------------------------------------------------------
// Null byte
// ----------------------------------------------------------------------------

TEST(JsonEscape, NullByteEscaped) {
    std::string_view input("a\0b", 3);
    EXPECT_EQ(json_escape(input), "a\\u0000b");
}

// ----------------------------------------------------------------------------
// DEL character (0x7F) is escaped as \u007f
// ----------------------------------------------------------------------------

TEST(JsonEscape, DelCharEscaped) {
    std::string_view input("a\x7F" "b", 3);
    EXPECT_EQ(json_escape(input), "a\\u007fb");
}

// ----------------------------------------------------------------------------
// Other control chars (U+0001 .. U+001F excluding named ones)
// ----------------------------------------------------------------------------

TEST(JsonEscape, GenericControlCharEscaped) {
    // SOH (0x01)
    std::string_view input("\x01", 1);
    EXPECT_EQ(json_escape(input), "\\u0001");
}

// ----------------------------------------------------------------------------
// Valid UTF-8 multibyte sequences passed through unchanged
// ----------------------------------------------------------------------------

TEST(JsonEscape, TwoByteUtf8PassedThrough) {
    // U+00E9 (e with acute) = 0xC3 0xA9
    std::string input = "\xC3\xA9";
    EXPECT_EQ(json_escape(input), input);
}

TEST(JsonEscape, ThreeByteUtf8PassedThrough) {
    // U+4E16 (CJK character) = 0xE4 0xB8 0x96
    std::string input = "\xE4\xB8\x96";
    EXPECT_EQ(json_escape(input), input);
}

TEST(JsonEscape, FourByteUtf8PassedThrough) {
    // U+1F600 (grinning face) = 0xF0 0x9F 0x98 0x80
    std::string input = "\xF0\x9F\x98\x80";
    EXPECT_EQ(json_escape(input), input);
}

// ----------------------------------------------------------------------------
// Invalid / truncated UTF-8 escaped as \uXXXX
// ----------------------------------------------------------------------------
// NOTE: The fast path only triggers escaping for control chars, quotes, and
// backslash. Bytes >= 0x80 alone do NOT trigger the slow path. Invalid UTF-8
// is only escaped when the string also contains a character that forces the
// slow path. We test this by mixing invalid UTF-8 with a control char.

TEST(JsonEscape, OrphanContinuationByteEscaped) {
    // 0x80 alone passes through on the fast path (no trigger character)
    std::string_view alone("\x80", 1);
    EXPECT_EQ(json_escape(alone), std::string("\x80"));

    // With a newline forcing the slow path, orphan 0x80 gets escaped
    std::string_view mixed("\x80\n", 2);
    EXPECT_EQ(json_escape(mixed), "\\u0080\\n");
}

TEST(JsonEscape, TruncatedTwoByteSequenceEscaped) {
    // 0xC3 alone passes through (fast path, no trigger)
    std::string_view alone("\xC3", 1);
    EXPECT_EQ(json_escape(alone), std::string("\xC3"));

    // With a tab forcing the slow path, truncated sequence gets escaped
    std::string_view mixed("\xC3\t", 2);
    EXPECT_EQ(json_escape(mixed), "\\u00c3\\t");
}

TEST(JsonEscape, TruncatedThreeByteSequenceEscaped) {
    // 0xE4 0xB8 alone passes through (fast path)
    std::string_view alone("\xE4\xB8", 2);
    EXPECT_EQ(json_escape(alone), std::string("\xE4\xB8"));

    // With a quote forcing the slow path, both bytes get individually escaped
    std::string mixed = std::string("\xE4\xB8") + "\"";
    EXPECT_EQ(json_escape(mixed), "\\u00e4\\u00b8\\\"");
}

// ----------------------------------------------------------------------------
// Mixed content
// ----------------------------------------------------------------------------

TEST(JsonEscape, MixedContent) {
    // Combines: normal ASCII, quote, newline, valid UTF-8, null byte
    std::string input = "key=\"val\"\n";
    input += "\xC3\xA9";          // valid 2-byte UTF-8
    input += std::string("\0", 1); // null byte
    input += "end";

    std::string expected = "key=\\\"val\\\"\\n";
    expected += "\xC3\xA9";
    expected += "\\u0000";
    expected += "end";

    EXPECT_EQ(json_escape(input), expected);
}
