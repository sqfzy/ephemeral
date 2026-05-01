/// @file test_dpdk_platform_mempool.cpp
/// Unit tests for Platform's per-lcore / NUMA-aware mempool layout (T2.9).
///
/// Coverage:
///   * DefaultStillSinglePool — `per_lcore_pools == 0` produces exactly
///     one pool, and `pool_for_lcore(any)` returns it (backwards compat).
///   * PerLcorePoolsCreated   — `per_lcore_pools = N` produces N distinct
///     pools, each addressable via `pool_for_lcore(i)`; out-of-range
///     returns `nullptr`.
///   * NumaSocketIdRecorded   — each per-lcore pool's `socket_id` matches
///     `rte_lcore_to_socket_id(lcore_id)` (or 0 on single-socket / NUMA-
///     disabled hosts).
///   * OverflowReleaseSafe    — drain a pool to exhaustion, alloc returns
///     null cleanly; release back, alloc succeeds again.
///   * MovedFromReturnsNull   — moved-from Platform returns nullptr from
///     every accessor (regression: `pool_for_lcore` must inherit the
///     same null-guard contract `mempool()` already obeys).
///   * ConfigRejectsHugePerLcore — validate_config policies the upper
///     bound on `per_lcore_pools`.
///
/// EAL bootstrap is private to this binary: we register two `net_null`
/// vdevs (port 0 + port 1) so we can hold one default-mode Platform AND
/// one per-lcore-mode Platform simultaneously without fighting for the
/// single one-shot port that the shared `dpdk_test_env.hpp` provides.
/// Runs under `--no-pci --no-huge`, so any host (no vfio-pci / no
/// hugepages) executes the full suite.
///
/// Why one-shot net_null requires care: `rte_eth_dev_close()` permanently
/// drops a `net_null` vdev — the port id can never be reattached in the
/// same process. We therefore use gtest fixture suites that build ONE
/// Platform per mode and reuse it across all tests in that suite. Each
/// suite gets its own port (`net_null0` for default-mode,
/// `net_null1` for per-lcore-mode).

#include <cstdint>
#include <optional>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include "eph/dpdk/platform.hpp"

#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

namespace eph::net::dpdk::test_mempool {

/// Private gtest Environment for this binary only — does NOT use the
/// shared `dpdk_test_env.hpp` because we need TWO net_null ports so
/// default-mode and per-lcore-mode Platforms can coexist (net_null is
/// one-shot per port: rte_eth_dev_close drops it permanently).
class DpdkMempoolTestEnv : public ::testing::Environment {
public:
    void SetUp() override {
        // Two net_null vdevs so default-mode and per-lcore-mode test
        // suites can each own one port for the duration of the binary.
        // Note: pass `--vdev` and the value as separate argv tokens —
        // DPDK accepts both `--vdev=name` and `--vdev name`, but the
        // separate-token form is what the official unit tests use and
        // it works reliably on every DPDK release we've validated.
        std::vector<const char*> raw = {
            "test_dpdk_platform_mempool",
            "--no-huge",
            "--no-pci",
            "--vdev", "net_null0",
            "--vdev", "net_null1",
            "--log-level=1",
        };
        int argc = static_cast<int>(raw.size());
        std::vector<char*> argv;
        argv.reserve(raw.size());
        for (auto a : raw) argv.push_back(const_cast<char*>(a));

        int ret = rte_eal_init(argc, argv.data());
        ASSERT_GE(ret, 0)
            << "EAL init failed (rte_errno=" << rte_errno
            << "): " << rte_strerror(rte_errno);
    }

    void TearDown() override {
        rte_eal_cleanup();
    }
};

namespace detail {
[[maybe_unused]]
static const auto* env_reg =
    ::testing::AddGlobalTestEnvironment(new DpdkMempoolTestEnv());
}

}  // namespace eph::net::dpdk::test_mempool

namespace {

using ::eph::dpdk::Platform;
using ::eph::dpdk::PlatformConfigV3;

// Conservative test config: small pool, single queue, link timeout 0
// (don't wait — net_null link is virtual). Keeping `mbuf_pool_size`
// modest so each per-lcore pool stays small.
constexpr PlatformConfigV3 kBaseCfg{
    .port_id         = 0,
    .nb_rx_queues    = 1,
    .nb_tx_queues    = 1,
    .nb_rx_desc      = 256,
    .nb_tx_desc      = 512,
    .mbuf_pool_size  = 1023,
    .mbuf_cache_size = 32,
    .link_timeout_ms = 0,
};

// Local helper: route a v3 PlatformConfig through the canonical
// v3→v2 translation, then run the v2 structural validator against
// the result. Tests in this file pin down validator semantics
// (per_lcore_pools bound, RSS rx/tx queue mismatch, etc.) that the
// v2 validator currently owns; the validator moves into the v3
// path in stage 2 of the v2/v3 merge, at which point this helper
// becomes a direct call.
inline auto validate_v3(const PlatformConfigV3& cfg) {
    return ::eph::dpdk::validate_config(
        ::eph::dpdk::detail::v3_to_legacy_(cfg));
}

// ---------------------------------------------------------------------------
// Config-validation tests — no real Platform needed.
// ---------------------------------------------------------------------------

TEST(PlatformMempoolConfig, ValidatorAcceptsZeroPerLcorePools) {
    PlatformConfigV3 cfg = kBaseCfg;
    cfg.per_lcore_pools = 0;
    EXPECT_TRUE(validate_v3(cfg).empty());
}

TEST(PlatformMempoolConfig, ValidatorAcceptsModeratePerLcorePools) {
    PlatformConfigV3 cfg = kBaseCfg;
    cfg.per_lcore_pools = 4;
    EXPECT_TRUE(validate_v3(cfg).empty());
}

TEST(PlatformMempoolConfig, ValidatorRejectsExcessPerLcorePools) {
    PlatformConfigV3 cfg = kBaseCfg;
    cfg.per_lcore_pools = 1024;  // > RTE_MAX_LCORE (256)
    auto err = validate_v3(cfg);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("per_lcore_pools"), std::string_view::npos);
}

TEST(PlatformMempoolConfig, ValidatorRejectsRssWithSingleTxQueue) {
    // RSS-aware connect pins `tx_queue_id = rx_queue_id`, so any stream
    // landing on a non-zero RSS bucket would silently drive
    // `tx_queue_id >= nb_tx_queues` when `nb_tx_queues == 1` and
    // `nb_rx_queues > 1`. The Phase-1 retro
    // (.artifacts/reshape-rss-aware-connect-final-20260430.md) calls
    // out the symptom — a clean connect() followed by zero TX bytes —
    // as the worst kind of misconfiguration: silent. This validator
    // line surfaces it at create() time.
    PlatformConfigV3 cfg = kBaseCfg;
    cfg.nb_rx_queues = 4;
    cfg.nb_tx_queues = 1;  // intentionally mismatched
    auto err = validate_v3(cfg);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("nb_tx_queues"), std::string_view::npos);
    EXPECT_NE(err.find("nb_rx_queues"), std::string_view::npos);
}

TEST(PlatformMempoolConfig, ValidatorAcceptsMatchedMultiQueue) {
    // The matched case (rx == tx == N > 1) must continue to validate —
    // this is the canonical RSS-aware multi-queue layout.
    PlatformConfigV3 cfg = kBaseCfg;
    cfg.nb_rx_queues = 4;
    cfg.nb_tx_queues = 4;
    EXPECT_TRUE(validate_v3(cfg).empty()) << validate_v3(cfg);
}

TEST(PlatformMempoolConfig, ValidatorAcceptsSingleQueueBoth) {
    // Single-queue bring-up (the test fixture default) must remain valid:
    // the new check is gated on `nb_rx_queues > 1`.
    PlatformConfigV3 cfg = kBaseCfg;
    cfg.nb_rx_queues = 1;
    cfg.nb_tx_queues = 1;
    EXPECT_TRUE(validate_v3(cfg).empty());
}

TEST(PlatformMempoolConfig, DumpContainsPerLcoreField) {
    PlatformConfigV3 cfg = kBaseCfg;
    cfg.per_lcore_pools = 3;
    auto d = cfg.dump();
    EXPECT_NE(d.find("per_lcore_pools"), std::string::npos);
    EXPECT_NE(d.find("per_lcore_pools=3"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Live Platform tests.
//
// Two fixture suites — one per port. Each holds ONE Platform alive for
// the duration of the suite (SetUpTestSuite / TearDownTestSuite), so the
// one-shot net_null vdev is consumed exactly once per suite. All tests
// in a suite share the same Platform via the static pointer.
//
// On hosts where Platform::create() fails (e.g. EAL init succeeded but
// the vdev isn't there for some reason), tests SKIP cleanly with a
// diagnostic — they never abort.
// ---------------------------------------------------------------------------

// Suite-naming notes:
//   * `Platform_A_PerLcore` runs first — gtest runs suites in
//     registration / alphabetical order, and we need the per-lcore
//     suite to claim port 1 while BOTH net_null vdevs are still live
//     (count=2). The `_A_` / `_B_` infixes encode that ordering.
//   * Once the per-lcore suite tears down (rte_eth_dev_close on port 1),
//     count drops to 1 and port 0 is still valid — perfect for the
//     default-mode suite.
//   * Platform's port_id check is `port_id >= count` (live port count,
//     not high-water mark), which is why ordering matters here. We can
//     work around it by ordering test suites — no platform-side change
//     needed.

// ---- Per-lcore-mode suite (per_lcore_pools = 4, port 1) -------------
// Runs FIRST because port_id=1 is only valid while count >= 2 (i.e.
// before the default-mode suite closes port 0).

class Platform_A_PerLcore : public ::testing::Test {
protected:
    static constexpr uint16_t kPools = 4;
    static std::optional<Platform> plat_;

    static void SetUpTestSuite() {
        PlatformConfigV3 cfg = kBaseCfg;
        cfg.port_id = 1;                    // net_null1
        cfg.per_lcore_pools = kPools;
        auto r = Platform::create(cfg);
        if (r.has_value()) {
            plat_.emplace(std::move(*r));
        }
    }

    static void TearDownTestSuite() {
        plat_.reset();
    }
};
std::optional<Platform> Platform_A_PerLcore::plat_{};

// ---- Default-mode suite (per_lcore_pools = 0, port 0) ---------------
// Runs SECOND. By the time the per-lcore suite has torn down, port 1
// is gone and port 0 is the surviving port — count=1 makes port_id=0
// the only valid choice anyway.

class Platform_B_DefaultMode : public ::testing::Test {
protected:
    static std::optional<Platform> plat_;

    static void SetUpTestSuite() {
        PlatformConfigV3 cfg = kBaseCfg;
        cfg.port_id = 0;            // net_null0
        cfg.per_lcore_pools = 0;    // default — single shared pool
        auto r = Platform::create(cfg);
        if (r.has_value()) {
            plat_.emplace(std::move(*r));
        }
    }

    static void TearDownTestSuite() {
        plat_.reset();
    }
};
std::optional<Platform> Platform_B_DefaultMode::plat_{};

TEST_F(Platform_A_PerLcore, AvailableOrSkip) {
    if (!plat_) {
        GTEST_SKIP() << "per-lcore-mode Platform unavailable on net_null1";
    }
    EXPECT_TRUE(plat_->is_running());
}

TEST_F(Platform_A_PerLcore, PerLcorePoolsCreated) {
    if (!plat_) {
        GTEST_SKIP() << "per-lcore-mode Platform unavailable on net_null1";
    }
    // All requested pools must be addressable, distinct, and non-null.
    std::set<rte_mempool*> seen;
    for (uint16_t i = 0; i < kPools; ++i) {
        rte_mempool* p = plat_->pool_for_lcore(i);
        ASSERT_NE(p, nullptr) << "pool_for_lcore(" << i << ") is null";
        EXPECT_TRUE(seen.insert(p).second)
            << "pool_for_lcore(" << i << ") is a duplicate of an earlier slot";
    }
    EXPECT_EQ(seen.size(), kPools);

    // Out-of-range lcore id must return nullptr — caller policy is
    // documented as "return null or wrap"; we chose nullptr so misuse
    // is loud (the alternative — wrap to slot 0 — would silently mask
    // an off-by-one in user code).
    EXPECT_EQ(plat_->pool_for_lcore(kPools), nullptr);
    EXPECT_EQ(plat_->pool_for_lcore(kPools + 1), nullptr);
    EXPECT_EQ(plat_->pool_for_lcore(255), nullptr);

    // Legacy `mempool()` accessor must still return a non-null pool —
    // we pin it at slot 0 so RX queue setup and unupdated call sites
    // keep working.
    auto* legacy = plat_->mempool();
    ASSERT_NE(legacy, nullptr);
    EXPECT_EQ(legacy, plat_->pool_for_lcore(0));
}

TEST_F(Platform_A_PerLcore, NumaSocketIdRecorded) {
    if (!plat_) {
        GTEST_SKIP() << "per-lcore-mode Platform unavailable on net_null1";
    }
    // For each per-lcore pool, verify its DPDK-recorded socket_id matches
    // what `rte_lcore_to_socket_id` reports for that lcore. On
    // single-socket / NUMA-disabled hosts the expected value is 0; on
    // genuine multi-socket NUMA systems it should be the lcore's local
    // socket. Either way the invariant is "pool's socket == lcore's
    // socket" — never a mismatch.
    for (uint16_t i = 0; i < kPools; ++i) {
        rte_mempool* p = plat_->pool_for_lcore(i);
        ASSERT_NE(p, nullptr);

        // `rte_mempool::socket_id` is the public field; valid on every
        // pool created through `rte_pktmbuf_pool_create`.
        const int pool_sock = p->socket_id;
        // If the lcore isn't enabled the implementation falls back to
        // SOCKET_ID_ANY. Match that fallback here so the assertion is
        // meaningful in both cases.
        const int expected =
            rte_lcore_is_enabled(static_cast<unsigned>(i))
                ? static_cast<int>(rte_lcore_to_socket_id(
                      static_cast<unsigned>(i)))
                : SOCKET_ID_ANY;

        // SOCKET_ID_ANY (-1) → DPDK records the actual socket the alloc
        // landed on (typically 0 on net_null / single-socket hosts).
        // Treat that as "matches expected" by accepting the recorded
        // socket_id as a valid socket >= 0.
        if (expected == SOCKET_ID_ANY) {
            EXPECT_GE(pool_sock, 0)
                << "pool's recorded socket_id should be a real socket "
                   "id (>= 0) when SOCKET_ID_ANY was passed in";
        } else {
            EXPECT_EQ(pool_sock, expected)
                << "lcore=" << i << " expected socket=" << expected
                << " but pool reports socket=" << pool_sock;
        }
    }
}

TEST_F(Platform_A_PerLcore, OverflowReleaseSafe) {
    if (!plat_) {
        GTEST_SKIP() << "per-lcore-mode Platform unavailable on net_null1";
    }
    // Drain pool 1 (not the canonical pool 0 backing RX) to exhaustion.
    rte_mempool* p = plat_->pool_for_lcore(1);
    ASSERT_NE(p, nullptr);

    // We don't assume the exact capacity: `mbuf_pool_size = 1023` minus
    // the per-lcore cache reservation. We just keep allocating until
    // alloc returns nullptr.
    std::vector<rte_mbuf*> bufs;
    bufs.reserve(2048);
    while (true) {
        rte_mbuf* m = rte_pktmbuf_alloc(p);
        if (m == nullptr) break;
        bufs.push_back(m);
    }
    // Sanity: we drained at least *some* mbufs — pool wasn't already
    // empty at start.
    EXPECT_GT(bufs.size(), 0u);
    // Now allocation must cleanly return nullptr (no crash, no UB).
    EXPECT_EQ(rte_pktmbuf_alloc(p), nullptr);

    // Release every mbuf back to the pool. Pool must be reusable
    // immediately after the last release — a basic mempool sanity
    // check that catches grossly broken mempool init / wrong cache
    // semantics.
    for (auto* m : bufs) rte_pktmbuf_free(m);
    bufs.clear();
    rte_mbuf* fresh = rte_pktmbuf_alloc(p);
    EXPECT_NE(fresh, nullptr) << "pool didn't recover after drain+release";
    if (fresh) rte_pktmbuf_free(fresh);
}

// ---- Default-mode tests (run AFTER per-lcore suite) ----------------

TEST_F(Platform_B_DefaultMode, AvailableOrSkip) {
    if (!plat_) {
        GTEST_SKIP() << "default-mode Platform unavailable on net_null0";
    }
    EXPECT_TRUE(plat_->is_running());
}

TEST_F(Platform_B_DefaultMode, DefaultStillSinglePool) {
    if (!plat_) {
        GTEST_SKIP() << "default-mode Platform unavailable on net_null0";
    }
    auto* shared = plat_->mempool();
    ASSERT_NE(shared, nullptr);

    // pool_for_lcore must return the same shared pool regardless of
    // lcore_id when per_lcore_pools is 0. This is the backwards-compat
    // contract — any existing call site that switches to
    // `pool_for_lcore(rte_lcore_id())` must keep working unchanged.
    EXPECT_EQ(plat_->pool_for_lcore(0), shared);
    EXPECT_EQ(plat_->pool_for_lcore(1), shared);
    EXPECT_EQ(plat_->pool_for_lcore(7), shared);
    EXPECT_EQ(plat_->pool_for_lcore(255), shared);
}

TEST_F(Platform_B_DefaultMode, MovedFromReturnsNull) {
    // pool_for_lcore must obey the same null-guard contract as
    // `mempool()`: a moved-from Platform returns nullptr for every
    // accessor. Move the suite's Platform into a local; assert the
    // moved-from slot is null; restore so suite teardown frees once.
    if (!plat_) {
        GTEST_SKIP() << "default-mode Platform unavailable on net_null0";
    }
    Platform local = std::move(*plat_);
    EXPECT_EQ(plat_->mempool(),         nullptr);  // legacy accessor
    EXPECT_EQ(plat_->pool_for_lcore(0), nullptr);  // new accessor
    EXPECT_EQ(plat_->pool_for_lcore(7), nullptr);
    // The moved-to Platform must still be live.
    EXPECT_NE(local.mempool(), nullptr);
    EXPECT_NE(local.pool_for_lcore(0), nullptr);
    // Restore: move back so the suite teardown frees exactly once.
    *plat_ = std::move(local);
}

}  // namespace
