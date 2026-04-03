#include <gtest/gtest.h>
#include "eph/version.hpp"

TEST(Version, ConstantsAreConsistent) {
    EXPECT_EQ(eph::kVersion,
              eph::kVersionMajor * 10000 +
              eph::kVersionMinor * 100 +
              eph::kVersionPatch);
}

TEST(Version, StringMatchesComponents) {
    auto expected = std::to_string(eph::kVersionMajor) + "." +
                    std::to_string(eph::kVersionMinor) + "." +
                    std::to_string(eph::kVersionPatch);
    EXPECT_EQ(eph::kVersionString, expected);
}

TEST(Version, PackedVersionComparable) {
    // Verify the packing scheme allows meaningful comparisons
    static_assert(eph::kVersion >= 10000, "Version should be >= 1.0.0");
    // 1.0.0 == 10000
    static_assert(eph::kVersion == 10000);
}

TEST(Version, FullVersionContainsLibraryName) {
    EXPECT_TRUE(eph::kVersionFull.starts_with("ephemeral/"));
    EXPECT_NE(eph::kVersionFull.find(eph::kVersionString), std::string_view::npos);
}

TEST(Version, VersionAtLeastCurrentVersion) {
    static_assert(eph::version_at_least(1, 0, 0));
    static_assert(eph::version_at_least(0, 99, 99));
    static_assert(!eph::version_at_least(1, 0, 1));
    static_assert(!eph::version_at_least(1, 1, 0));
    static_assert(!eph::version_at_least(2, 0, 0));
}
