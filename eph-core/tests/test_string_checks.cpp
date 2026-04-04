/// @file test_string_checks.cpp
/// Unit tests for eph::core::detail::contains_control_chars().

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "eph/core/detail/string_checks.hpp"

using eph::core::detail::contains_control_chars;

// ----------------------------------------------------------------------------
// Normal printable strings: no control chars
// ----------------------------------------------------------------------------

TEST(ContainsControlChars, NormalPrintableStringReturnsFalse) {
    EXPECT_FALSE(contains_control_chars("Hello, World! 123 ~@#$%"));
}

// ----------------------------------------------------------------------------
// Empty string: no characters at all
// ----------------------------------------------------------------------------

TEST(ContainsControlChars, EmptyStringReturnsFalse) {
    EXPECT_FALSE(contains_control_chars(""));
}

// ----------------------------------------------------------------------------
// Common control characters return true
// ----------------------------------------------------------------------------

TEST(ContainsControlChars, NewlineReturnsTrue) {
    EXPECT_TRUE(contains_control_chars("line1\nline2"));
}

TEST(ContainsControlChars, CarriageReturnReturnsTrue) {
    EXPECT_TRUE(contains_control_chars("a\rb"));
}

TEST(ContainsControlChars, TabReturnsTrue) {
    EXPECT_TRUE(contains_control_chars("a\tb"));
}

// ----------------------------------------------------------------------------
// Null byte (U+0000) returns true
// ----------------------------------------------------------------------------

TEST(ContainsControlChars, NullByteReturnsTrue) {
    std::string_view input("a\0b", 3);
    EXPECT_TRUE(contains_control_chars(input));
}

// ----------------------------------------------------------------------------
// DEL (0x7F) returns true
// ----------------------------------------------------------------------------

TEST(ContainsControlChars, DelCharReturnsTrue) {
    std::string_view input("a\x7F" "b", 3);
    EXPECT_TRUE(contains_control_chars(input));
}

// ----------------------------------------------------------------------------
// Other low control chars
// ----------------------------------------------------------------------------

TEST(ContainsControlChars, SohReturnsTrue) {
    // SOH = 0x01
    EXPECT_TRUE(contains_control_chars(std::string_view("\x01", 1)));
}

TEST(ContainsControlChars, EscReturnsTrue) {
    // ESC = 0x1B
    EXPECT_TRUE(contains_control_chars("prefix\x1B""suffix"));
}

TEST(ContainsControlChars, UnitSeparatorReturnsTrue) {
    // US = 0x1F (last char before space)
    EXPECT_TRUE(contains_control_chars(std::string_view("\x1F", 1)));
}

// ----------------------------------------------------------------------------
// High bytes (>= 0x80) are NOT flagged as control chars
// ----------------------------------------------------------------------------

TEST(ContainsControlChars, HighBytesReturnFalse) {
    // UTF-8 encoded characters should not trigger control char detection
    EXPECT_FALSE(contains_control_chars("\xC3\xA9"));       // U+00E9
    EXPECT_FALSE(contains_control_chars("\xE4\xB8\x96"));   // U+4E16
    EXPECT_FALSE(contains_control_chars("\xF0\x9F\x98\x80")); // U+1F600
}

// ----------------------------------------------------------------------------
// Constexpr validation
// ----------------------------------------------------------------------------

TEST(ContainsControlChars, ConstexprUsable) {
    // Verify the function is actually constexpr-evaluable
    static_assert(!contains_control_chars("hello"));
    static_assert(contains_control_chars("he\nllo"));
    static_assert(!contains_control_chars(""));
}
