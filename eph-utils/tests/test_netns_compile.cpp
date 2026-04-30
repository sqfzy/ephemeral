/// @file test_netns_compile.cpp
/// Compile-time smoke for `eph::utils::linux_::enter_netns`.
///
/// The header has no in-tree caller yet (it's exposed for bench / tooling
/// fixtures); without this test the header would only be exercised when
/// someone external picks it up. The runtime call is wrapped in a test
/// that asserts only the API shape and the failure-mode error string —
/// we deliberately do NOT require a real netns to be present (CI / local
/// dev would have to root + `ip netns add`).

#include <gtest/gtest.h>

#include "eph/utils/linux/netns.hpp"

TEST(NetnsCompile, MissingNetnsReturnsErrorWithPath) {
    // Pick a name almost certainly absent from /var/run/netns/. We assert
    // only that the call returns unexpected with a string containing the
    // path — this exercises every code branch except the success tail
    // without elevating to root.
    auto r = eph::utils::linux_::enter_netns("eph_definitely_not_a_real_ns");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("/var/run/netns/eph_definitely_not_a_real_ns"),
              std::string::npos)
        << "error string should embed the resolved path; got: " << r.error();
}

TEST(NetnsCompile, EmptyNameRejected) {
    auto r = eph::utils::linux_::enter_netns("");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("empty"), std::string::npos)
        << "expected empty-name diagnostic; got: " << r.error();
}

TEST(NetnsCompile, PathTraversalRejected) {
    // `..` would let a buggy caller resolve to a non-netns file under
    // /var/run/netns/. The function must reject it before open(2).
    auto r = eph::utils::linux_::enter_netns("../etc/passwd");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("must not contain"), std::string::npos)
        << "expected path-traversal diagnostic; got: " << r.error();
}

TEST(NetnsCompile, DotDotRejected) {
    auto r = eph::utils::linux_::enter_netns("..");
    ASSERT_FALSE(r.has_value());
}

TEST(NetnsCompile, DotRejected) {
    auto r = eph::utils::linux_::enter_netns(".");
    ASSERT_FALSE(r.has_value());
}
