#include <cstdint>

#include <gtest/gtest.h>

#include "eph/base/cache.hpp"
#include "eph/utils/alignment.hpp"

using namespace eph::utils;

TEST(AlignTest, SmallTypesGetCacheLineAlignment) {
    // Types with natural alignment < 64 should be promoted to cache line size
    static_assert(Align<char> == eph::base::CACHE_LINE_SIZE);
    static_assert(Align<int> == eph::base::CACHE_LINE_SIZE);
    static_assert(Align<uint64_t> == eph::base::CACHE_LINE_SIZE);
    static_assert(Align<double> == eph::base::CACHE_LINE_SIZE);
    static_assert(Align<void*> == eph::base::CACHE_LINE_SIZE);

    EXPECT_EQ(Align<char>, 64);
    EXPECT_EQ(Align<int>, 64);
    EXPECT_EQ(Align<uint64_t>, 64);
}

TEST(AlignTest, OverAlignedTypesPreserveAlignment) {
    // Types with alignment > cache line size should keep their natural alignment
    struct alignas(128) OverAligned { char data; };
    struct alignas(256) VeryOverAligned { char data; };

    static_assert(Align<OverAligned> == 128);
    static_assert(Align<VeryOverAligned> == 256);

    EXPECT_EQ(Align<OverAligned>, 128);
    EXPECT_EQ(Align<VeryOverAligned>, 256);
}

TEST(AlignTest, CacheLineAlignedTypeStaysAtCacheLine) {
    // A type already at cache line alignment should stay there
    struct alignas(64) ExactCacheLine { char data; };

    static_assert(Align<ExactCacheLine> == eph::base::CACHE_LINE_SIZE);
    EXPECT_EQ(Align<ExactCacheLine>, 64);
}

TEST(AlignTest, StructWithMixedMemberAlignment) {
    struct Mixed {
        char   a;
        double b;
        int    c;
    };
    // Mixed struct has natural alignment of double (8), promoted to 64
    static_assert(Align<Mixed> == eph::base::CACHE_LINE_SIZE);
    EXPECT_EQ(Align<Mixed>, 64);
}

TEST(AlignTest, CacheLineSizeIs64) {
    static_assert(eph::base::CACHE_LINE_SIZE == 64);
    EXPECT_EQ(eph::base::CACHE_LINE_SIZE, 64);
}
