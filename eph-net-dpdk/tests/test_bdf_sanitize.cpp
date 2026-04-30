/// @file test_bdf_sanitize.cpp
/// Unit tests for `eph::dpdk::detail::sanitize_bdf_for_file_prefix`.
///
/// Pure logic — no EAL or DPDK runtime dependency. The helper is the
/// auto-derivation step inside `Platform::join_dynamic` (Mode 2):
/// `file_prefix = "eph_" + sanitize(bdf)` lets two processes sharing
/// the same NIC agree on the prefix without explicit string
/// coordination.

#include <string>

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
