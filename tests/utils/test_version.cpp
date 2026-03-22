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
