/// @file test_audit_sweeper.cpp
/// M series — exit-on-shutdown semantics for the 1 Hz audit-sweep
/// thread. The thread itself is spawned by `Platform::serve_nic` (a
/// daemon entry that needs EAL + hugepages), but the cv-wait-on-flag
/// pattern is purely standard-library and can be unit-tested in
/// isolation. This test pins that pattern so a future refactor that
/// breaks the shutdown semantics fails loudly here instead of leaving
/// a zombie thread in production.
///
/// What's tested:
///   - Worker thread loops on `running.load()` and sleeps via
///     `cv.wait_for(1s)`.
///   - Setting `running=false` + `cv.notify_all()` causes the thread
///     to exit promptly (well under the 1 s tick).
///   - Repeated stop+start cycles do not deadlock or leak threads.
///
/// What's NOT tested here:
///   - The actual sweep work (`icmp_directory_->audit_sweep_one_round`)
///     — covered by test_icmp_directory_hmac.
///   - End-to-end Platform::serve_nic spawn + ~Impl join — needs EAL
///     and is exercised by the hardware-gated integration test.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include <gtest/gtest.h>

namespace {

struct SweeperState {
    std::atomic<bool>       running{false};
    std::mutex              mu;
    std::condition_variable cv;
    std::atomic<uint64_t>   ticks{0};
};

// Mirrors the loop body in Platform::serve_nic's audit_sweeper_thread
// closure (commit 0ac91e49+M series). If this body diverges from the
// real one, both should be updated together.
void sweeper_loop(SweeperState* st) noexcept {
    while (st->running.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lk{st->mu};
        st->cv.wait_for(
            lk, std::chrono::seconds(1),
            [st]() {
                return !st->running.load(std::memory_order_acquire);
            });
        if (!st->running.load(std::memory_order_acquire)) break;
        lk.unlock();
        st->ticks.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace

TEST(AuditSweeper, ShutdownIsPrompt) {
    SweeperState st{};
    st.running.store(true);
    auto t = std::thread(sweeper_loop, &st);

    // Brief uptime: the thread enters cv_wait_for(1s) — far longer
    // than the 50 ms we sleep here. We then signal shutdown; the
    // join must complete in well under 100 ms (vs the 1 s tick).
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const auto before = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> g{st.mu};
        st.running.store(false, std::memory_order_release);
    }
    st.cv.notify_all();
    t.join();
    const auto after = std::chrono::steady_clock::now();

    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            after - before).count();
    EXPECT_LT(elapsed_ms, 100)
        << "Shutdown took " << elapsed_ms
        << " ms — should be <100 ms (cv notify breaks the 1 s tick "
           "early). If this fails, the sweeper is sleeping by some "
           "non-cv mechanism and ~Impl will block for up to 1 s.";
    // Likely 0-1 ticks fired (we sleep 50 ms then notify; tick fires
    // every 1 s). We don't assert on exact count to avoid flakiness
    // on slow CI; just that the thread exited cleanly.
}

TEST(AuditSweeper, MultipleStartStopCyclesDoNotDeadlock) {
    SweeperState st{};
    for (int cycle = 0; cycle < 4; ++cycle) {
        st.running.store(true);
        auto t = std::thread(sweeper_loop, &st);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        {
            std::lock_guard<std::mutex> g{st.mu};
            st.running.store(false, std::memory_order_release);
        }
        st.cv.notify_all();
        t.join();
        EXPECT_FALSE(st.running.load());
    }
    SUCCEED();
}

TEST(AuditSweeper, ImmediateStopBeforeFirstTickIsClean) {
    // Edge case: ~Impl can fire before the worker has run a single
    // tick (e.g. a Platform created and immediately destroyed in a
    // smoke test). The sweeper should still exit cleanly without
    // ever incrementing ticks.
    SweeperState st{};
    st.running.store(true);
    auto t = std::thread(sweeper_loop, &st);
    {
        std::lock_guard<std::mutex> g{st.mu};
        st.running.store(false, std::memory_order_release);
    }
    st.cv.notify_all();
    t.join();
    EXPECT_EQ(st.ticks.load(), 0u);
}
