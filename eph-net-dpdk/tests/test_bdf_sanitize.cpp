/// @file test_bdf_sanitize.cpp
/// Unit tests for `eph::dpdk::detail::sanitize_bdf_for_file_prefix`.
///
/// Pure logic — no EAL or DPDK runtime dependency. The helper is the
/// auto-derivation step inside `Platform::create` / `Platform::serve_nic`
/// (daemon-led model): `file_prefix = "eph_" + sanitize(bdf)` lets the
/// daemon primary and every secondary application agree on the EAL
/// `--file-prefix` from the PCI BDF alone, with no explicit string
/// coordination between operators and developers.

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "eph/core/error.hpp"
#include "eph/dpdk/detail/bdf_sanitize.hpp"

using eph::core::Error;
using eph::dpdk::detail::sanitize_bdf_for_file_prefix;

TEST(BdfSanitize, StandardBdf_FullDomainForm) {
    auto r = sanitize_bdf_for_file_prefix("0000:28:00.0");
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    EXPECT_EQ(*r, "0000_28_00_0");
}

TEST(BdfSanitize, StandardBdf_ShortForm) {
    auto r = sanitize_bdf_for_file_prefix("28:00.0");
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    EXPECT_EQ(*r, "28_00_0");
}

TEST(BdfSanitize, EmptyBdf_Rejected) {
    auto r = sanitize_bdf_for_file_prefix("");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::InvalidConfig);
}

TEST(BdfSanitize, OverlongBdf_Rejected) {
    // 13+ chars: too long for canonical PCI BDF.
    auto r = sanitize_bdf_for_file_prefix("00000:28:00.0");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::InvalidConfig);
}

TEST(BdfSanitize, TooShortBdf_Rejected) {
    auto r = sanitize_bdf_for_file_prefix("8:0.0");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::InvalidConfig);
}

TEST(BdfSanitize, InvalidChar_NonHex_Rejected) {
    auto r = sanitize_bdf_for_file_prefix("0000:zz:00.0");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::InvalidConfig);
}

TEST(BdfSanitize, InvalidChar_Whitespace_Rejected) {
    auto r = sanitize_bdf_for_file_prefix("000 :28:00.0");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::InvalidConfig);
}

TEST(BdfSanitize, MissingDot_Rejected) {
    // Has ':' but no '.' — not a canonical PCI BDF.
    auto r = sanitize_bdf_for_file_prefix("0000:28:000");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::InvalidConfig);
}

TEST(BdfSanitize, MissingColon_Rejected) {
    // Has '.' but no ':'.
    auto r = sanitize_bdf_for_file_prefix("0000.28.00.0");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::InvalidConfig);
}

TEST(BdfSanitize, HexCaseInsensitive_Accepted) {
    auto lower = sanitize_bdf_for_file_prefix("0000:af:bc.d");
    ASSERT_TRUE(lower.has_value()) << lower.error().detail;
    EXPECT_EQ(*lower, "0000_af_bc_d");

    auto upper = sanitize_bdf_for_file_prefix("0000:AF:BC.D");
    ASSERT_TRUE(upper.has_value()) << upper.error().detail;
    // Case is preserved — mixing wouldn't agree across peers anyway,
    // so callers should normalize their input. We only validate.
    EXPECT_EQ(*upper, "0000_AF_BC_D");
}

TEST(BdfSanitize, MinLengthBoundary_7Chars_Accepted) {
    // The lower bound `kBdfMinLen = 7` is the canonical short PCI
    // BDF form `BB:DD.F` (one hex byte + ':' + two hex + '.' + one
    // hex). Sits exactly at the boundary — a regression that
    // tightened the check to `<= 7` would silently reject every
    // short-form BDF.
    auto r = sanitize_bdf_for_file_prefix("0:00.0");  // 6 chars, must fail
    ASSERT_FALSE(r.has_value());

    auto ok = sanitize_bdf_for_file_prefix("a:bc.d");  // still 6 chars
    ASSERT_FALSE(ok.has_value());

    auto seven = sanitize_bdf_for_file_prefix("aa:bc.d");  // 7 chars, valid
    ASSERT_TRUE(seven.has_value()) << seven.error().detail;
    EXPECT_EQ(*seven, "aa_bc_d");
}

TEST(BdfSanitize, EmbeddedNulRejected) {
    // The header doc explicitly forbids NUL bytes ("no whitespace,
    // no glob metas, no NUL byte"). The validator currently catches
    // it via the is_hex branch (NUL is neither hex, ':', nor '.'),
    // which is a fragile reliance — pin it with a regression test
    // built from a `string_view{ptr, size}` so the NUL survives into
    // the function (a `const char*` ctor would terminate early).
    constexpr char kBuf[] = "00\0:28:00.0";  // size 11, has embedded NUL
    std::string_view sv{kBuf, sizeof(kBuf) - 1};
    auto r = sanitize_bdf_for_file_prefix(sv);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::InvalidConfig);
}

TEST(BdfSanitize, OutputFitsInFilePrefixCap) {
    // The whole point of the helper: the output must fit inside
    // kMpRegistryFilePrefixMax (24) once "eph_" (4) is prepended.
    // Worst case full-form BDF is 12 chars → "eph_" + 12 = 16 < 24.
    auto r = sanitize_bdf_for_file_prefix("ffff:ff:ff.f");
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    EXPECT_EQ(r->size(), 12u);
    const std::string with_prefix = std::string("eph_") + *r;
    EXPECT_LT(with_prefix.size(), 24u);
}
