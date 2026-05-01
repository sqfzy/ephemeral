/// @file test_dpdk_lcore_csv_parser.cpp
/// Unit tests for `eph::dpdk::test::parse_lcores_csv_to_mask` — the
/// EAL-CSV-to-bitmask helper extracted from `DpdkBenchEnv::create_via_autojoin`.
///
/// Pure parser logic, no EAL touch. Validates the round-3 strict-bound
/// closure: lcore IDs >= 64 (the MpRegistry v2 schema cap) are now
/// rejected at parse time rather than silently truncated, and negative
/// inputs are caught before strtoul wraps them into huge unsigneds.
///
/// Discovered via /pax --review LENS "production / MP mental model /
/// silent misuse closure" round 3 on 2026-05-01.

// dpdk_env.hpp gates its content with #ifdef EPH_USE_DPDK so the
// header can compile-out cleanly in kernel-only builds. For this
// unit test we explicitly opt in — the helper is pure C++ logic and
// has no EAL dependency.
#ifndef EPH_USE_DPDK
#  define EPH_USE_DPDK 1
#endif

#include <cstdint>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "eph/dpdk/test/dpdk_env.hpp"

using eph::dpdk::test::parse_lcores_csv_to_mask;

// ─────────────────────────────────────────────────────────────────────────────
// Happy path
// ─────────────────────────────────────────────────────────────────────────────

TEST(LcoreCsvParser, Empty_OptOut) {
    auto r = parse_lcores_csv_to_mask("");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(*r, 0u);
}

TEST(LcoreCsvParser, OnlyWhitespace_OptOut) {
    auto r = parse_lcores_csv_to_mask("   ");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(*r, 0u);
}

TEST(LcoreCsvParser, OnlyCommas_OptOut) {
    auto r = parse_lcores_csv_to_mask(",,,");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(*r, 0u);
}

TEST(LcoreCsvParser, SingleId_BitSet) {
    auto r = parse_lcores_csv_to_mask("5");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(*r, uint64_t{1} << 5);
}

TEST(LcoreCsvParser, MultipleSingles) {
    auto r = parse_lcores_csv_to_mask("0,1,2");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(*r, 0x7u);
}

TEST(LcoreCsvParser, Range_LowToHigh) {
    auto r = parse_lcores_csv_to_mask("0-3");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(*r, 0xFu);
}

TEST(LcoreCsvParser, MixedRangesAndSingles) {
    auto r = parse_lcores_csv_to_mask("0-1,4-5");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(*r, 0x33u);  // bits 0,1,4,5
}

TEST(LcoreCsvParser, Whitespace_Tolerated) {
    auto r = parse_lcores_csv_to_mask(" 0 , 1 , 2 ");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(*r, 0x7u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Schema-cap boundary — round-3 closure
// ─────────────────────────────────────────────────────────────────────────────

TEST(LcoreCsvParser, Boundary_LcoreZero_Allowed) {
    auto r = parse_lcores_csv_to_mask("0");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(*r, 1u);
}

TEST(LcoreCsvParser, Boundary_Lcore63_Allowed) {
    auto r = parse_lcores_csv_to_mask("63");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(*r, uint64_t{1} << 63);
}

TEST(LcoreCsvParser, Boundary_FullRange_0_63_Allowed) {
    auto r = parse_lcores_csv_to_mask("0-63");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(*r, ~uint64_t{0});  // all 64 bits set
}

TEST(LcoreCsvParser, Boundary_Lcore64_Rejected) {
    auto r = parse_lcores_csv_to_mask("64");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("schema cap"), std::string::npos)
        << "Error message must mention the schema cap to point the user "
           "at the root cause; got: " << r.error();
}

TEST(LcoreCsvParser, Boundary_RangeOverCap_Rejected) {
    auto r = parse_lcores_csv_to_mask("60-65");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("schema cap"), std::string::npos);
}

TEST(LcoreCsvParser, Boundary_Lcore999_Rejected) {
    auto r = parse_lcores_csv_to_mask("999");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("schema cap"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// Malformed input
// ─────────────────────────────────────────────────────────────────────────────

TEST(LcoreCsvParser, Malformed_Letters_Rejected) {
    auto r = parse_lcores_csv_to_mask("abc");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("malformed"), std::string::npos);
}

TEST(LcoreCsvParser, Malformed_LetterMidstream_Rejected) {
    auto r = parse_lcores_csv_to_mask("0,1,abc,2");
    ASSERT_FALSE(r.has_value());
}

TEST(LcoreCsvParser, ReversedRange_Rejected) {
    auto r = parse_lcores_csv_to_mask("5-3");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("reversed range"), std::string::npos);
}

TEST(LcoreCsvParser, Negative_Rejected) {
    // Without the explicit `-` guard, strtoul("-3", ...) returns
    // ULONG_MAX-2, which then either passes the `>= 64` check
    // (rejected, fine) — but only because we have the schema-cap
    // guard. Belt + suspenders: detect signed inputs explicitly so
    // the diagnostic is precise.
    auto r = parse_lcores_csv_to_mask("-3");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("signed integer"), std::string::npos);
}

TEST(LcoreCsvParser, NegativeInRange_Rejected) {
    auto r = parse_lcores_csv_to_mask("0--3");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("signed integer"), std::string::npos);
}

TEST(LcoreCsvParser, PlusSign_Rejected) {
    auto r = parse_lcores_csv_to_mask("+3");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("signed integer"), std::string::npos);
}

TEST(LcoreCsvParser, OverlongInput_Rejected) {
    // Sanity: 256-byte cap on input length so a runaway env var
    // doesn't fill the heap. 257-byte input must reject.
    std::string overlong(257, '0');
    auto r = parse_lcores_csv_to_mask(overlong);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("too long"), std::string::npos);
}
