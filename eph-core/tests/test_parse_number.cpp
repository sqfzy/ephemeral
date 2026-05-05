/// @file test_parse_number.cpp
/// Unit tests for eph::core::parse_number() and parse_int().

#include <climits>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "eph/core/parse_number.hpp"

using namespace eph::core;

// ============================================================================
// parse_number — normal path
// ============================================================================

TEST(ParseNumber, SimpleInteger) {
    auto r = parse_number("42");
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(*r, 42.0);
}

TEST(ParseNumber, SimpleDecimal) {
    auto r = parse_number("3.14");
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(*r, 3.14);
}

TEST(ParseNumber, NegativeInteger) {
    auto r = parse_number("-7");
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(*r, -7.0);
}

TEST(ParseNumber, NegativeDecimal) {
    auto r = parse_number("-0.001");
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(*r, -0.001);
}

TEST(ParseNumber, Zero) {
    auto r = parse_number("0");
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(*r, 0.0);
}

TEST(ParseNumber, ZeroPointZero) {
    auto r = parse_number("0.0");
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(*r, 0.0);
}

TEST(ParseNumber, LargeInteger) {
    auto r = parse_number("123456789012345");
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(*r, 123456789012345.0);
}

TEST(ParseNumber, PrecisionLimitDecimals) {
    // Typical exchange price: 8 decimal places
    auto r = parse_number("0.00000001");
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(*r, 1e-8);
}

// ============================================================================
// parse_number — scientific notation
// ============================================================================

TEST(ParseNumber, ScientificNotationPositiveExponent) {
    auto r = parse_number("1.5e10");
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(*r, 1.5e10);
}

TEST(ParseNumber, ScientificNotationNegativeExponent) {
    auto r = parse_number("3e-8");
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(*r, 3e-8);
}

TEST(ParseNumber, ScientificNotationUppercaseE) {
    auto r = parse_number("2.5E3");
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(*r, 2500.0);
}

TEST(ParseNumber, ScientificNotationExplicitPlusExponent) {
    auto r = parse_number("1.0e+5");
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(*r, 100000.0);
}

TEST(ParseNumber, ScientificNotationZeroExponent) {
    auto r = parse_number("7.5e0");
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(*r, 7.5);
}

TEST(ParseNumber, ScientificNotationIntegerMantissa) {
    auto r = parse_number("5e2");
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(*r, 500.0);
}

TEST(ParseNumber, ScientificNotationDecimalMantissa) {
    auto r = parse_number("0.5e1");
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(*r, 5.0);
}

// ============================================================================
// parse_number — boundary conditions
// ============================================================================

TEST(ParseNumber, ExponentAtLimit308) {
    // 1e308 is within IEEE 754 range
    auto r = parse_number("1e308");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(std::isfinite(*r));
}

TEST(ParseNumber, ExponentExceeds308ReturnsNullopt) {
    // Exponent > 308 is rejected by the parser's guard
    EXPECT_FALSE(parse_number("1e309").has_value());
    EXPECT_FALSE(parse_number("1e1000").has_value());
}

// The exponent-magnitude guard at parser.hpp:89 fires symmetrically
// for negative exponents — `exp > 308` is checked BEFORE the sign
// is applied, so `1e-1000` (which IEEE 754 would underflow to +0.0)
// is rejected with the same nullopt as `1e1000`. The earlier-fix
// VerySmallNegativeExponent test only covers the in-range case
// (1e-308 succeeds); a future refactor that splits the magnitude
// check from the sign handling could regress this rejection
// silently. Lock it with a focused test.
TEST(ParseNumber, NegativeExponentExceeds308ReturnsNullopt) {
    EXPECT_FALSE(parse_number("1e-309").has_value());
    EXPECT_FALSE(parse_number("1e-1000").has_value());
    EXPECT_FALSE(parse_number("-1.5e-500").has_value());
}

TEST(ParseNumber, VerySmallNegativeExponent) {
    auto r = parse_number("1e-308");
    ASSERT_TRUE(r.has_value());
    // Subnormal or very small but finite
    EXPECT_TRUE(std::isfinite(*r));
    EXPECT_GT(*r, 0.0);
}

TEST(ParseNumber, LargeProductOverflowsToInfReturnsNullopt) {
    // 9.9e308 overflows double → +inf → rejected by isfinite check
    EXPECT_FALSE(parse_number("9.9e308").has_value());
}

TEST(ParseNumber, NegativeZero) {
    auto r = parse_number("-0");
    ASSERT_TRUE(r.has_value());
    // IEEE 754 -0.0 is still 0.0 for comparison
    EXPECT_DOUBLE_EQ(*r, 0.0);
}

TEST(ParseNumber, LeadingZeros) {
    auto r = parse_number("007");
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(*r, 7.0);
}

TEST(ParseNumber, LeadingZerosDecimal) {
    auto r = parse_number("00.5");
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(*r, 0.5);
}

TEST(ParseNumber, SingleDigitFraction) {
    auto r = parse_number("1.5");
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(*r, 1.5);
}

TEST(ParseNumber, ManyFractionDigits) {
    // 18 digits of fraction — double can't represent all, but should parse
    auto r = parse_number("0.123456789012345678");
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR(*r, 0.123456789012345678, 1e-15);
}

// ============================================================================
// parse_number — error paths (malformed input)
// ============================================================================

TEST(ParseNumber, EmptyStringReturnsNullopt) {
    EXPECT_FALSE(parse_number("").has_value());
}

TEST(ParseNumber, BareMinusReturnsNullopt) {
    EXPECT_FALSE(parse_number("-").has_value());
}

TEST(ParseNumber, BareDotReturnsNullopt) {
    EXPECT_FALSE(parse_number(".").has_value());
}

TEST(ParseNumber, IntegerDotWithoutFractionReturnsNullopt) {
    // "1." has no fractional digit → rejected
    EXPECT_FALSE(parse_number("1.").has_value());
}

TEST(ParseNumber, DotWithoutIntegerOrFractionReturnsNullopt) {
    EXPECT_FALSE(parse_number("-.").has_value());
}

TEST(ParseNumber, BareExponentReturnsNullopt) {
    EXPECT_FALSE(parse_number("e5").has_value());
    EXPECT_FALSE(parse_number("E5").has_value());
}

TEST(ParseNumber, ExponentWithoutDigitsReturnsNullopt) {
    EXPECT_FALSE(parse_number("1e").has_value());
    EXPECT_FALSE(parse_number("1E").has_value());
    EXPECT_FALSE(parse_number("1e-").has_value());
    EXPECT_FALSE(parse_number("1e+").has_value());
}

TEST(ParseNumber, TrailingCharactersReturnsNullopt) {
    EXPECT_FALSE(parse_number("42x").has_value());
    EXPECT_FALSE(parse_number("3.14abc").has_value());
    EXPECT_FALSE(parse_number("1e5z").has_value());
}

TEST(ParseNumber, LeadingPlusSignReturnsNullopt) {
    // parse_number only accepts '-', not '+'
    EXPECT_FALSE(parse_number("+42").has_value());
}

TEST(ParseNumber, SpacesReturnsNullopt) {
    EXPECT_FALSE(parse_number(" 42").has_value());
    EXPECT_FALSE(parse_number("42 ").has_value());
    EXPECT_FALSE(parse_number(" ").has_value());
}

TEST(ParseNumber, NonDigitCharactersReturnsNullopt) {
    EXPECT_FALSE(parse_number("abc").has_value());
    EXPECT_FALSE(parse_number("NaN").has_value());
    EXPECT_FALSE(parse_number("inf").has_value());
    EXPECT_FALSE(parse_number("Infinity").has_value());
}

TEST(ParseNumber, NullByteInMiddleReturnsNullopt) {
    // string_view with embedded null — should fail on the null byte
    EXPECT_FALSE(parse_number(std::string_view("1\0002", 3)).has_value());
}

TEST(ParseNumber, NegativeFractionOnlyParsesCorrectly) {
    // "-.5" has no integer part but has a fractional part — parser accepts this
    auto r = parse_number("-.5");
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(*r, -0.5);
}

TEST(ParseNumber, FractionOnlyWithoutIntegerPart) {
    // ".5" has no integer part and no sign — accepted if frac digits exist
    auto r = parse_number(".5");
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(*r, 0.5);
}

// ============================================================================
// parse_int — normal path
// ============================================================================

TEST(ParseInt, SimplePositive) {
    auto r = parse_int("42");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 42);
}

TEST(ParseInt, SimpleNegative) {
    auto r = parse_int("-123");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, -123);
}

TEST(ParseInt, Zero) {
    auto r = parse_int("0");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 0);
}

TEST(ParseInt, LeadingZeros) {
    auto r = parse_int("007");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 7);
}

TEST(ParseInt, NegativeZero) {
    auto r = parse_int("-0");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 0);
}

TEST(ParseInt, SingleDigit) {
    for (int i = 0; i <= 9; ++i) {
        std::string s(1, static_cast<char>('0' + i));
        auto r = parse_int(s);
        ASSERT_TRUE(r.has_value()) << "digit " << i;
        EXPECT_EQ(*r, i);
    }
}

// ============================================================================
// parse_int — boundary conditions (INT64_MIN / INT64_MAX)
// ============================================================================

TEST(ParseInt, Int64Max) {
    // INT64_MAX = 9223372036854775807
    auto r = parse_int("9223372036854775807");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, INT64_MAX);
}

TEST(ParseInt, Int64Min) {
    // INT64_MIN = -9223372036854775808
    auto r = parse_int("-9223372036854775808");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, INT64_MIN);
}

TEST(ParseInt, OverflowPositiveByOneReturnsNullopt) {
    // INT64_MAX + 1 = 9223372036854775808 (unsigned fits but signed overflows)
    EXPECT_FALSE(parse_int("9223372036854775808").has_value());
}

TEST(ParseInt, OverflowNegativeByOneReturnsNullopt) {
    // INT64_MIN - 1 = -9223372036854775809
    EXPECT_FALSE(parse_int("-9223372036854775809").has_value());
}

TEST(ParseInt, LargePositiveOverflowReturnsNullopt) {
    // Way beyond int64 range
    EXPECT_FALSE(parse_int("99999999999999999999").has_value());
}

TEST(ParseInt, LargeNegativeOverflowReturnsNullopt) {
    EXPECT_FALSE(parse_int("-99999999999999999999").has_value());
}

TEST(ParseInt, Int64MaxMinusOne) {
    auto r = parse_int("9223372036854775806");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, INT64_MAX - 1);
}

TEST(ParseInt, Int64MinPlusOne) {
    auto r = parse_int("-9223372036854775807");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, INT64_MIN + 1);
}

// ============================================================================
// parse_int — error paths
// ============================================================================

TEST(ParseInt, EmptyStringReturnsNullopt) {
    EXPECT_FALSE(parse_int("").has_value());
}

TEST(ParseInt, BareMinusReturnsNullopt) {
    EXPECT_FALSE(parse_int("-").has_value());
}

TEST(ParseInt, DecimalPointReturnsNullopt) {
    // parse_int does not handle fractions
    EXPECT_FALSE(parse_int("3.14").has_value());
}

TEST(ParseInt, ScientificNotationReturnsNullopt) {
    EXPECT_FALSE(parse_int("1e5").has_value());
}

TEST(ParseInt, TrailingCharsReturnsNullopt) {
    EXPECT_FALSE(parse_int("42x").has_value());
    EXPECT_FALSE(parse_int("123 ").has_value());
}

TEST(ParseInt, LeadingSpaceReturnsNullopt) {
    EXPECT_FALSE(parse_int(" 42").has_value());
}

TEST(ParseInt, PlusSignReturnsNullopt) {
    EXPECT_FALSE(parse_int("+42").has_value());
}

TEST(ParseInt, NonDigitReturnsNullopt) {
    EXPECT_FALSE(parse_int("abc").has_value());
}

TEST(ParseInt, NullByteReturnsNullopt) {
    EXPECT_FALSE(parse_int(std::string_view("1\0002", 3)).has_value());
}
