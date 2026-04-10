#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

#include "eph/utils/kill_switch.hpp"

using eph::utils::KillSwitch;

// ---------------------------------------------------------------------------
// Basic state tests
// ---------------------------------------------------------------------------

TEST(KillSwitchTest, initial_state_is_not_tripped) {
    KillSwitch ks;
    EXPECT_FALSE(ks.tripped());
}

TEST(KillSwitchTest, trip_flips_state_to_true) {
    KillSwitch ks;
    ks.trip();
    EXPECT_TRUE(ks.tripped());
}

TEST(KillSwitchTest, trip_without_callback_does_not_crash) {
    KillSwitch ks;  // default nullptr callback
    ASSERT_NO_THROW(ks.trip());
    EXPECT_TRUE(ks.tripped());
}

TEST(KillSwitchTest, nullptr_callback_explicit_does_not_crash) {
    KillSwitch ks{nullptr};
    ks.trip();
    EXPECT_TRUE(ks.tripped());
}

// ---------------------------------------------------------------------------
// Callback semantics
// ---------------------------------------------------------------------------

TEST(KillSwitchTest, callback_fires_exactly_once_on_first_trip) {
    std::atomic<int> count{0};
    KillSwitch ks{[&] { count.fetch_add(1, std::memory_order_relaxed); }};

    ks.trip();
    EXPECT_EQ(count.load(), 1);
}

TEST(KillSwitchTest, callback_not_re_invoked_on_second_trip) {
    std::atomic<int> count{0};
    KillSwitch ks{[&] { count.fetch_add(1, std::memory_order_relaxed); }};

    ks.trip();
    ks.trip();
    ks.trip();
    EXPECT_EQ(count.load(), 1);
    EXPECT_TRUE(ks.tripped());
}

TEST(KillSwitchTest, callback_not_fired_until_trip_called) {
    std::atomic<bool> fired{false};
    KillSwitch ks{[&] { fired.store(true); }};

    EXPECT_FALSE(fired.load());
    EXPECT_FALSE(ks.tripped());
}

TEST(KillSwitchTest, callback_can_observe_external_state) {
    int sentinel = 0;
    KillSwitch ks{[&] { sentinel = 42; }};
    ks.trip();
    EXPECT_EQ(sentinel, 42);
}

// ---------------------------------------------------------------------------
// Idempotency
// ---------------------------------------------------------------------------

TEST(KillSwitchTest, trip_is_idempotent_state_wise) {
    KillSwitch ks;
    for (int i = 0; i < 1000; ++i) {
        ks.trip();
        EXPECT_TRUE(ks.tripped());
    }
}

// ---------------------------------------------------------------------------
// Thread safety / memory ordering
// ---------------------------------------------------------------------------

TEST(KillSwitchTest, multi_thread_trip_fires_callback_exactly_once) {
    constexpr int kThreads      = 16;
    constexpr int kTripsPerThr  = 1000;

    std::atomic<int> fire_count{0};
    KillSwitch ks{[&] { fire_count.fetch_add(1, std::memory_order_relaxed); }};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    std::atomic<bool> go{false};

    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) { /* spin */ }
            for (int i = 0; i < kTripsPerThr; ++i) {
                ks.trip();
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& th : workers) th.join();

    EXPECT_EQ(fire_count.load(), 1);
    EXPECT_TRUE(ks.tripped());
}

TEST(KillSwitchTest, tripped_observes_write_from_other_thread) {
    KillSwitch ks;
    std::atomic<bool> observer_saw{false};

    std::thread observer([&] {
        // Spin until we see tripped.  If memory ordering is correct,
        // this loop terminates promptly after the trip on the main thread.
        while (!ks.tripped()) {
            std::this_thread::yield();
        }
        observer_saw.store(true, std::memory_order_release);
    });

    // Small delay to ensure observer is spinning.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ks.trip();
    observer.join();

    EXPECT_TRUE(observer_saw.load());
}

// ---------------------------------------------------------------------------
// Poll pattern (main-loop style)
// ---------------------------------------------------------------------------

TEST(KillSwitchTest, poll_loop_terminates_after_trip) {
    KillSwitch ks;
    std::atomic<int> iterations{0};

    std::thread tripper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ks.trip();
    });

    while (!ks.tripped()) {
        iterations.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    tripper.join();

    EXPECT_GT(iterations.load(), 0);
    EXPECT_TRUE(ks.tripped());
}

// ---------------------------------------------------------------------------
// Destructor / lifetime
// ---------------------------------------------------------------------------

TEST(KillSwitchTest, destructor_is_clean_when_never_tripped) {
    // Scope entry/exit with no trip() must not invoke any callback.
    std::atomic<int> count{0};
    {
        KillSwitch ks{[&] { count.fetch_add(1); }};
        (void)ks.tripped();
    }
    EXPECT_EQ(count.load(), 0);
}

TEST(KillSwitchTest, heap_allocation_and_destruction_clean) {
    // Exercise the common use case where a KillSwitch lives inside a
    // shared manager — guards against UAF in the bound callback.
    std::atomic<int> count{0};
    auto ks = std::make_unique<KillSwitch>([&] { count.fetch_add(1); });
    ks->trip();
    ks.reset();
    EXPECT_EQ(count.load(), 1);
}

// ---------------------------------------------------------------------------
// Compile-time: D-3 one-way design
// ---------------------------------------------------------------------------

// These checks are redundant with the static_asserts inside kill_switch.hpp
// but live here so a test failure surfaces the contract explicitly.  The
// detection idiom uses concepts so that the intentional absence of these
// members is a SFINAE failure rather than a hard error.
namespace {
template <class T> concept ks_has_reset  = requires(T& t) { t.reset(); };
template <class T> concept ks_has_untrip = requires(T& t) { t.untrip(); };
template <class T> concept ks_has_clear  = requires(T& t) { t.clear(); };
} // namespace

TEST(KillSwitchTest, does_not_expose_reset_or_untrip_or_clear) {
    static_assert(!ks_has_reset<KillSwitch>,
                  "KillSwitch must not expose reset() — D-3");
    static_assert(!ks_has_untrip<KillSwitch>,
                  "KillSwitch must not expose untrip() — D-3");
    static_assert(!ks_has_clear<KillSwitch>,
                  "KillSwitch must not expose clear() — D-3");
    SUCCEED();
}

TEST(KillSwitchTest, is_not_copyable_or_movable) {
    static_assert(!std::is_copy_constructible_v<KillSwitch>);
    static_assert(!std::is_move_constructible_v<KillSwitch>);
    static_assert(!std::is_copy_assignable_v<KillSwitch>);
    static_assert(!std::is_move_assignable_v<KillSwitch>);
    SUCCEED();
}
