#include <gtest/gtest.h>

#include <eph/utils/scope_guard.hpp>

namespace {

TEST(ScopeGuard, RunsCleanupOnDestructionWhenArmed) {
    int counter = 0;
    {
        auto guard = eph::utils::ScopeGuard{[&] { ++counter; }};
        EXPECT_TRUE(guard.armed());
        EXPECT_EQ(counter, 0);
    }
    EXPECT_EQ(counter, 1);
}

TEST(ScopeGuard, DoesNotRunCleanupAfterDisarm) {
    int counter = 0;
    {
        auto guard = eph::utils::ScopeGuard{[&] { ++counter; }};
        guard.disarm();
        EXPECT_FALSE(guard.armed());
    }
    EXPECT_EQ(counter, 0);
}

TEST(ScopeGuard, DisarmIsIdempotent) {
    int counter = 0;
    {
        auto guard = eph::utils::ScopeGuard{[&] { ++counter; }};
        guard.disarm();
        guard.disarm();
        guard.disarm();
        EXPECT_FALSE(guard.armed());
    }
    EXPECT_EQ(counter, 0);
}

TEST(ScopeGuard, RunsCleanupOnEarlyReturnPath) {
    // Simulates the "multiple early-return resource leak" pattern that
    // motivated this utility.
    int   release_count = 0;
    auto  body = [&](bool early_exit) {
        auto guard = eph::utils::ScopeGuard{[&] { ++release_count; }};
        if (early_exit) {
            return;            // guard fires
        }
        guard.disarm();         // success path: caller took ownership
    };

    body(true);
    EXPECT_EQ(release_count, 1);

    body(false);
    EXPECT_EQ(release_count, 1);   // unchanged: disarmed
}

TEST(ScopeGuard, CapturesByReferenceCorrectly) {
    int  resource_state = 1;
    {
        auto guard = eph::utils::ScopeGuard{[&] { resource_state = 0; }};
        EXPECT_EQ(resource_state, 1);
    }
    EXPECT_EQ(resource_state, 0);
}

TEST(ScopeGuard, CapturesByValueWorksForRawPointers) {
    int  buffer = 42;
    int* ptr    = &buffer;
    {
        auto guard = eph::utils::ScopeGuard{[ptr] { *ptr = 0; }};
    }
    EXPECT_EQ(buffer, 0);
}

TEST(ScopeGuard, MultipleGuardsRunInReverseOrder) {
    // LIFO destruction order is the standard C++ guarantee. Verifies the
    // common pattern of layered guards (claim X, claim Y, release Y first
    // on failure, then release X).
    std::string log;
    {
        auto g1 = eph::utils::ScopeGuard{[&] { log += "1"; }};
        auto g2 = eph::utils::ScopeGuard{[&] { log += "2"; }};
        auto g3 = eph::utils::ScopeGuard{[&] { log += "3"; }};
    }
    EXPECT_EQ(log, "321");
}

TEST(ScopeGuard, StatelessLambdaIsZeroOverhead) {
    // Sanity check: a stateless lambda has zero capture storage. The
    // ScopeGuard adds one bool. Total size on a 64-bit target should be
    // 1 (bool) padded to alignof(F)=1 → 1 byte; with empty-base
    // optimisation we expect ≤ a few bytes regardless.
    auto guard = eph::utils::ScopeGuard{[] {}};
    static_assert(sizeof(guard) <= 8,
                  "stateless ScopeGuard should compile to ≤ 8 bytes");
    guard.disarm();
}

}  // namespace
