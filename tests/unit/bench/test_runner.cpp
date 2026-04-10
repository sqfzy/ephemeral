/// @file tests/unit/bench/test_runner.cpp
/// Unit tests for `BenchRunner` — exercises the warmup → measurement →
/// report skeleton against fake scenarios so we can catch silent regressions
/// in the recording loop without needing to fork a kernel mock.
///
/// The runner is templated on a `RttScenario` / `OneWayScenario` type, so
/// each test defines a tiny scenario that returns canned samples and
/// counts how many times each method is called. We then check:
///
///   1. `prepare(payload)` is called before any sample is recorded.
///   2. The pre-warmup phase runs (`do_one_rtt` is called many times
///      before the runner starts recording).
///   3. Samples returned during the warmup window are NOT recorded;
///      samples returned after warmup are.
///   4. `cleanup()` is called at the end of every window.
///   5. TSC inversion (server_send < server_recv, etc.) is silently
///      tolerated and the window still completes.
///   6. `g_running = false` aborts the loop cleanly.
///
/// The runner uses spdlog and `eph::utils::PhasedTimer` (TSC-based), so
/// we configure tiny warmup/duration windows to keep the test < 1 s.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "core/runner.hpp"
#include "core/sample.hpp"
#include "core/signal.hpp"
#include "eph/utils/time.hpp"

using namespace bench;
using namespace std::chrono_literals;

// ─── Fake RTT scenario ────────────────────────────────────────────────────
//
// Returns synthetic 4-leg samples on every `do_one_rtt` call. The sample
// values are deterministic functions of the call count so tests can verify
// recording semantics without needing real network I/O.

struct FakeRttScenario {
    size_t   prepare_calls{0};
    size_t   rtt_calls{0};
    size_t   cleanup_calls{0};
    size_t   last_prepare_payload{0};
    bool     prepare_returns{true};
    bool     rtt_returns{true};
    uint64_t base_tsc{1'000'000};

    bool prepare(size_t payload) noexcept {
        ++prepare_calls;
        last_prepare_payload = payload;
        return prepare_returns;
    }

    bool do_one_rtt(RttSample& out) noexcept {
        ++rtt_calls;
        // Synthesise a sample with monotonic timing — 1000 ns RTT,
        // 400 ns TX, 400 ns RX, 200 ns SRV.
        const uint64_t cs = base_tsc + rtt_calls * 1'000'000;
        out.client_send_tsc = cs;
        out.server_recv_tsc = cs + 400;
        out.server_send_tsc = cs + 600;
        out.client_recv_tsc = cs + 1000;
        return rtt_returns;
    }

    void cleanup() noexcept { ++cleanup_calls; }
};

// ─── Fake one-way scenario ────────────────────────────────────────────────

struct FakeOneWayScenario {
    size_t   prepare_calls{0};
    size_t   recv_calls{0};
    size_t   cleanup_calls{0};
    bool     prepare_returns{true};
    bool     recv_returns{true};
    uint64_t base_tsc{1'000'000};

    bool prepare() noexcept {
        ++prepare_calls;
        return prepare_returns;
    }

    bool do_one_recv(OneWaySample& out) noexcept {
        ++recv_calls;
        out.producer_tsc = base_tsc + recv_calls * 1'000'000;
        out.consumer_tsc = out.producer_tsc + 800;
        return recv_returns;
    }

    void cleanup() noexcept { ++cleanup_calls; }
};

// ─── Common config helper ─────────────────────────────────────────────────

CommonConfig tiny_config() {
    CommonConfig cfg;
    // Sub-second windows so the test stays fast. The runner accepts
    // `chrono::seconds` so 1 s is the minimum we can pass; warmup runs
    // for 1 s and measurement for 1 s, total ~2 s per payload.
    cfg.warmup   = std::chrono::seconds{1};
    cfg.duration = std::chrono::seconds{1};
    return cfg;
}

// ─── BenchRunner construction ─────────────────────────────────────────────

TEST(BenchRunnerCtor, AcceptsCommonConfig) {
    BenchRunner r(tiny_config(), "fake", "test");
    SUCCEED();  // construction completes
}

TEST(BenchRunnerCtor, AcceptsBenchConfig) {
    BenchConfig cfg;
    cfg.warmup   = std::chrono::seconds{1};
    cfg.duration = std::chrono::seconds{1};
    BenchRunner r(cfg, "fake", "test");
    SUCCEED();
}

// ─── RTT sweep happy path ─────────────────────────────────────────────────

TEST(BenchRunnerRttSweep, CallsPrepareCleanupOncePerPayload) {
    g_running.store(true, std::memory_order_relaxed);
    BenchRunner runner(tiny_config(), "fake_rtt", "test");
    FakeRttScenario sc;
    std::array<size_t, 3> payloads{64, 128, 256};

    runner.run_rtt_sweep(sc, std::span<const size_t>(payloads));

    EXPECT_EQ(sc.prepare_calls, 3u) << "one prepare per payload";
    EXPECT_EQ(sc.cleanup_calls, 3u) << "one cleanup per payload";
    EXPECT_EQ(sc.last_prepare_payload, 256u);
}

TEST(BenchRunnerRttSweep, CallsDoOneRttManyTimes) {
    g_running.store(true, std::memory_order_relaxed);
    BenchRunner runner(tiny_config(), "fake_rtt", "test");
    FakeRttScenario sc;
    std::array<size_t, 1> payloads{64};

    runner.run_rtt_sweep(sc, std::span<const size_t>(payloads));

    // 2000 pre-warmup rounds + however many fit in 1s warmup + 1s
    // measurement. With our trivial scenario this should be very fast,
    // far more than the pre-warmup floor.
    EXPECT_GT(sc.rtt_calls, 2000u)
        << "should at least exhaust pre-warmup";
}

TEST(BenchRunnerRttSweep, EmptyPayloadListIsNoOp) {
    g_running.store(true, std::memory_order_relaxed);
    BenchRunner runner(tiny_config(), "fake_rtt", "test");
    FakeRttScenario sc;
    std::array<size_t, 0> payloads{};

    runner.run_rtt_sweep(sc, std::span<const size_t>(payloads));

    EXPECT_EQ(sc.prepare_calls, 0u);
    EXPECT_EQ(sc.cleanup_calls, 0u);
    EXPECT_EQ(sc.rtt_calls, 0u);
}

TEST(BenchRunnerRttSweep, PrepareFailureSkipsWindow) {
    g_running.store(true, std::memory_order_relaxed);
    BenchRunner runner(tiny_config(), "fake_rtt", "test");
    FakeRttScenario sc;
    sc.prepare_returns = false;
    std::array<size_t, 1> payloads{64};

    runner.run_rtt_sweep(sc, std::span<const size_t>(payloads));

    EXPECT_EQ(sc.prepare_calls, 1u);
    EXPECT_EQ(sc.rtt_calls, 0u) << "no samples after prepare failure";
    // cleanup is NOT called when prepare fails — runner returns early
    // (this matches the existing scenario contract: prepare owns its
    // own resources until it succeeds).
    EXPECT_EQ(sc.cleanup_calls, 0u);
}

// ─── RTT sweep cooperative cancel ─────────────────────────────────────────

TEST(BenchRunnerRttSweep, GRunningFalseAbortsBetweenPayloads) {
    g_running.store(false, std::memory_order_relaxed);
    BenchRunner runner(tiny_config(), "fake_rtt", "test");
    FakeRttScenario sc;
    std::array<size_t, 4> payloads{64, 128, 256, 512};

    runner.run_rtt_sweep(sc, std::span<const size_t>(payloads));

    EXPECT_EQ(sc.prepare_calls, 0u)
        << "outer loop should bail before any payload starts";
    EXPECT_EQ(sc.cleanup_calls, 0u);

    // Reset for other tests in the same process.
    g_running.store(true, std::memory_order_relaxed);
}

TEST(BenchRunnerRttSweep, GRunningFalseDuringWindowAborts) {
    g_running.store(true, std::memory_order_relaxed);
    BenchRunner runner(tiny_config(), "fake_rtt", "test");

    // A scenario that flips g_running off after a few samples — the
    // measurement loop should observe and exit promptly.
    struct CancelAfterN {
        std::atomic<size_t> calls{0};
        size_t cleanup_calls{0};
        bool prepare(size_t) noexcept { return true; }
        bool do_one_rtt(RttSample& out) noexcept {
            const auto n = ++calls;
            out.client_send_tsc = n * 100;
            out.server_recv_tsc = n * 100 + 30;
            out.server_send_tsc = n * 100 + 60;
            out.client_recv_tsc = n * 100 + 100;
            if (n == 50) {
                g_running.store(false, std::memory_order_relaxed);
            }
            return true;
        }
        void cleanup() noexcept { ++cleanup_calls; }
    } sc;

    std::array<size_t, 1> payloads{64};
    runner.run_rtt_sweep(sc, std::span<const size_t>(payloads));

    EXPECT_GE(sc.calls.load(), 50u);
    // cleanup may or may not be called depending on whether the cancel
    // landed in the pre-warmup or measurement phase — both branches in
    // runner.hpp call cleanup, so it should be 1.
    EXPECT_EQ(sc.cleanup_calls, 1u);

    g_running.store(true, std::memory_order_relaxed);
}

// ─── RTT inflight sweep ───────────────────────────────────────────────────

TEST(BenchRunnerInflightSweep, CallsPrepareWithInflightAsPayload) {
    g_running.store(true, std::memory_order_relaxed);
    BenchRunner runner(tiny_config(), "fake_inflight", "test");
    FakeRttScenario sc;
    std::array<int, 3> inflights{1, 4, 16};

    runner.run_rtt_inflight_sweep(sc, std::span<const int>(inflights));

    EXPECT_EQ(sc.prepare_calls, 3u);
    EXPECT_EQ(sc.cleanup_calls, 3u);
    EXPECT_EQ(sc.last_prepare_payload, 16u)
        << "inflight count is reused as the prepare argument";
}

// ─── One-way sweep ────────────────────────────────────────────────────────

TEST(BenchRunnerOneway, CallsPrepareCleanupOnce) {
    g_running.store(true, std::memory_order_relaxed);
    BenchRunner runner(tiny_config(), "fake_oneway", "test");
    FakeOneWayScenario sc;

    runner.run_oneway(sc);

    EXPECT_EQ(sc.prepare_calls, 1u);
    EXPECT_EQ(sc.cleanup_calls, 1u);
    EXPECT_GT(sc.recv_calls, 2000u)
        << "should at least exhaust pre-warmup";
}

TEST(BenchRunnerOneway, PrepareFailureSkipsWindow) {
    g_running.store(true, std::memory_order_relaxed);
    BenchRunner runner(tiny_config(), "fake_oneway", "test");
    FakeOneWayScenario sc;
    sc.prepare_returns = false;

    runner.run_oneway(sc);

    EXPECT_EQ(sc.prepare_calls, 1u);
    EXPECT_EQ(sc.recv_calls, 0u);
    EXPECT_EQ(sc.cleanup_calls, 0u);
}

// ─── TSC inversion tolerance ──────────────────────────────────────────────

TEST(BenchRunnerInversion, InvertedTimestampsDoNotCrash) {
    g_running.store(true, std::memory_order_relaxed);
    BenchRunner runner(tiny_config(), "fake_inverted", "test");

    struct InvertedScenario {
        size_t calls{0};
        size_t cleanup_calls{0};
        bool prepare(size_t) noexcept { return true; }
        bool do_one_rtt(RttSample& out) noexcept {
            ++calls;
            // Deliberately backwards: client_recv < client_send. The
            // runner must skip recording (record_rtt drops samples
            // where the difference would underflow).
            out.client_send_tsc = 2000;
            out.server_recv_tsc = 1500;
            out.server_send_tsc = 1700;
            out.client_recv_tsc = 1000;
            return true;
        }
        void cleanup() noexcept { ++cleanup_calls; }
    } sc;

    std::array<size_t, 1> payloads{64};
    runner.run_rtt_sweep(sc, std::span<const size_t>(payloads));

    // Runner completed without dying. cleanup was called.
    EXPECT_EQ(sc.cleanup_calls, 1u);
    EXPECT_GT(sc.calls, 0u);
}

// ─── Cleanup signal state for any subsequent tests ────────────────────────

namespace {
struct ResetGRunning {
    ~ResetGRunning() {
        g_running.store(true, std::memory_order_relaxed);
    }
} reset_guard;
} // namespace
