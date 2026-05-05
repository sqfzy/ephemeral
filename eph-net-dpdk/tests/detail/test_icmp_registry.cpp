/// @file test_icmp_registry.cpp
/// Pure-unit coverage for `eph::dpdk::detail::IcmpRegistry` — the
/// heap-allocated, shared_ptr-managed ICMP dispatch target store.
///
/// Post-Major-2 requirements:
///   * IcmpRegistry inherits enable_shared_from_this → must be
///     constructed via `std::make_shared<IcmpRegistry>()`; stack
///     construction is UB (weak_from_this returns empty).
///   * Handle holds `weak_ptr<IcmpRegistry>` — safe deregistration
///     even if the registry has already been freed.
///   * Registry uses std::mutex internally — thread-safe register /
///     unregister / dispatch.
///
/// Coverage:
///   * Register / duplicate / null / full (OutOfMemory)
///   * Unregister via Handle dtor (weak_ptr lock + unregister)
///   * Dispatch match / miss / invalid embedded / multi-target
///   * Handle move semantics (default, move-ctor, move-assign, self-move)
///   * Re-register after unregister + swap-with-last compaction
///   * NEW (Major 2 lifecycle):
///     - RegistryDiesBeforeHandle — Handle dtor safe after registry freed
///     - MultipleSharedRefsKeepRegistryAlive — ref-count semantics
///     - ExtraRefOutlivesOriginalOwner — closure-captured-ref scenario
///     - MoveConstructHandleAfterRegistryDies — weak_ptr transfer edge
///     - ConcurrentRegisterUnregisterDispatch — mutex stress pin
///
/// No DPDK EAL, no mempool, no NIC — runs on any dev box. Designed
/// to pass under ASan + TSan.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "eph/core/error.hpp"
#include "eph/dpdk/detail/icmp_registry.hpp"
#include "eph/dpdk/packet_core.hpp"
#include "eph/dpdk/packet_parse.hpp"

using eph::dpdk::detail::IcmpRegistry;
using eph::dpdk::net::ConnectionTuple;
using eph::dpdk::net::ParsedIcmp;
using eph::dpdk::net::kIpProtoTcp;
using eph::dpdk::net::kIpProtoUdp;

namespace {

// Mock "stream" — the registered callback pokes it on dispatch hit.
// Atomic counters so the concurrent pin test can read them from
// multiple threads without UB (hits increments in the dispatch
// thread, reads happen in the main thread after joins).
struct StreamMock {
    std::atomic<uint16_t> last_mtu{0};
    std::atomic<int>      hits{0};
};
StreamMock g_mock_a;
StreamMock g_mock_b;
StreamMock g_mock_c;

void on_mtu_a(void* /*user*/, uint16_t mtu) noexcept {
    g_mock_a.last_mtu.store(mtu, std::memory_order_relaxed);
    g_mock_a.hits.fetch_add(1, std::memory_order_relaxed);
}
void on_mtu_b(void* /*user*/, uint16_t mtu) noexcept {
    g_mock_b.last_mtu.store(mtu, std::memory_order_relaxed);
    g_mock_b.hits.fetch_add(1, std::memory_order_relaxed);
}
void on_mtu_c(void* /*user*/, uint16_t mtu) noexcept {
    g_mock_c.last_mtu.store(mtu, std::memory_order_relaxed);
    g_mock_c.hits.fetch_add(1, std::memory_order_relaxed);
}

void reset_mocks() noexcept {
    g_mock_a.last_mtu.store(0); g_mock_a.hits.store(0);
    g_mock_b.last_mtu.store(0); g_mock_b.hits.store(0);
    g_mock_c.last_mtu.store(0); g_mock_c.hits.store(0);
}

constexpr ConnectionTuple make_tuple(uint32_t src_ip, uint16_t src_port,
                                       uint32_t dst_ip, uint16_t dst_port) noexcept {
    return ConnectionTuple{src_ip, dst_ip, src_port, dst_port};
}

ParsedIcmp make_icmp(const ConnectionTuple& t, uint8_t proto,
                     uint16_t next_hop_mtu) noexcept {
    ParsedIcmp p{};
    p.type              = 3;
    p.code              = 4;
    p.next_hop_mtu      = next_hop_mtu;
    p.embedded_src_ip   = t.src_ip;
    p.embedded_dst_ip   = t.dst_ip;
    p.embedded_src_port = t.src_port;
    p.embedded_dst_port = t.dst_port;
    p.embedded_proto    = proto;
    p.embedded_valid    = true;
    return p;
}

constexpr ConnectionTuple kTupleA = {0x0A000001, 0x0A000002, 50000, 443};
constexpr ConnectionTuple kTupleB = {0x0A000003, 0x0A000004, 50001, 8443};
constexpr ConnectionTuple kTupleC = {0x0A000005, 0x0A000006, 50002, 9443};

// Convenience factory — every registry in every test must be
// heap-allocated via make_shared (enable_shared_from_this contract).
std::shared_ptr<IcmpRegistry> make_registry() {
    return std::make_shared<IcmpRegistry>();
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// Register / unregister basics
// ═══════════════════════════════════════════════════════════════════════

TEST(IcmpRegistry, EmptyRegistryHasSizeZero) {
    auto r = make_registry();
    EXPECT_EQ(r->size(), 0u);
    EXPECT_EQ(r->dispatched(), 0u);
}

TEST(IcmpRegistry, RegisterReturnsEngagedHandle) {
    auto r = make_registry();
    reset_mocks();
    auto h = r->register_target(kTupleA, kIpProtoTcp, &g_mock_a, &on_mtu_a);
    ASSERT_TRUE(h.has_value()) << h.error().detail;
    EXPECT_TRUE(h->engaged());
    EXPECT_EQ(r->size(), 1u);
}

TEST(IcmpRegistry, NullStreamOrCallbackRejected) {
    auto r = make_registry();
    auto h_null_stream = r->register_target(kTupleA, kIpProtoTcp, nullptr, &on_mtu_a);
    ASSERT_FALSE(h_null_stream.has_value());
    EXPECT_EQ(h_null_stream.error().code, eph::core::Error::InvalidConfig);

    auto h_null_cb = r->register_target(kTupleA, kIpProtoTcp, &g_mock_a, nullptr);
    ASSERT_FALSE(h_null_cb.has_value());
    EXPECT_EQ(h_null_cb.error().code, eph::core::Error::InvalidConfig);

    EXPECT_EQ(r->size(), 0u);
}

TEST(IcmpRegistry, DuplicateTupleProtoRejected) {
    auto r = make_registry();
    reset_mocks();

    auto h1 = r->register_target(kTupleA, kIpProtoTcp, &g_mock_a, &on_mtu_a);
    ASSERT_TRUE(h1.has_value());

    auto h2 = r->register_target(kTupleA, kIpProtoTcp, &g_mock_b, &on_mtu_b);
    ASSERT_FALSE(h2.has_value());
    EXPECT_EQ(h2.error().code, eph::core::Error::InvalidConfig);

    // Same tuple but different proto — allowed.
    auto h3 = r->register_target(kTupleA, kIpProtoUdp, &g_mock_c, &on_mtu_c);
    ASSERT_TRUE(h3.has_value()) << h3.error().detail;

    EXPECT_EQ(r->size(), 2u);
}

TEST(IcmpRegistry, RegistryFullReturnsOutOfMemory) {
    auto r = make_registry();
    reset_mocks();

    std::vector<IcmpRegistry::Handle> holders;
    holders.reserve(IcmpRegistry::kMaxTargets);
    for (std::size_t i = 0; i < IcmpRegistry::kMaxTargets; ++i) {
        ConnectionTuple t{static_cast<uint32_t>(0x0A000000 + i),
                          0x0A00FFFF,
                          static_cast<uint16_t>(40000 + i),
                          443};
        auto h = r->register_target(t, kIpProtoTcp, &g_mock_a, &on_mtu_a);
        ASSERT_TRUE(h.has_value()) << "i=" << i << ": " << h.error().detail;
        holders.push_back(std::move(*h));
    }
    EXPECT_EQ(r->size(), IcmpRegistry::kMaxTargets);

    ConnectionTuple overflow{0x0BADCAFE, 0x0A00FFFF, 65000, 443};
    auto h = r->register_target(overflow, kIpProtoTcp, &g_mock_a, &on_mtu_a);
    ASSERT_FALSE(h.has_value());
    EXPECT_EQ(h.error().code, eph::core::Error::OutOfMemory);
}

TEST(IcmpRegistry, UnregisterByHandleDestructionShrinksRegistry) {
    auto r = make_registry();
    reset_mocks();
    {
        auto h = r->register_target(kTupleA, kIpProtoTcp, &g_mock_a, &on_mtu_a);
        ASSERT_TRUE(h.has_value());
        EXPECT_EQ(r->size(), 1u);
    }  // handle dtor → weak_ptr.lock() → unregister
    EXPECT_EQ(r->size(), 0u);
}

TEST(IcmpRegistry, UnregisterUnknownTupleIsNoop) {
    auto r = make_registry();
    r->unregister(kTupleA, kIpProtoTcp);
    EXPECT_EQ(r->size(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════
// Dispatch matching
// ═══════════════════════════════════════════════════════════════════════

TEST(IcmpRegistry, DispatchHitsRegisteredTarget) {
    auto r = make_registry();
    reset_mocks();
    auto h = r->register_target(kTupleA, kIpProtoTcp, &g_mock_a, &on_mtu_a);
    ASSERT_TRUE(h.has_value());
    r->dispatch(make_icmp(kTupleA, kIpProtoTcp, 1400));
    EXPECT_EQ(g_mock_a.hits.load(), 1);
    EXPECT_EQ(g_mock_a.last_mtu.load(), 1400u);
    EXPECT_EQ(r->dispatched(), 1u);
}

TEST(IcmpRegistry, DispatchWithUnmatchedTupleIsNoop) {
    auto r = make_registry();
    reset_mocks();
    auto h = r->register_target(kTupleA, kIpProtoTcp, &g_mock_a, &on_mtu_a);
    ASSERT_TRUE(h.has_value());
    ConnectionTuple wrong_port = kTupleA;
    wrong_port.src_port = 65535;
    r->dispatch(make_icmp(wrong_port, kIpProtoTcp, 1200));
    EXPECT_EQ(g_mock_a.hits.load(), 0);
    EXPECT_EQ(r->dispatched(), 0u);
}

TEST(IcmpRegistry, DispatchWithDifferentProtoIsNoop) {
    auto r = make_registry();
    reset_mocks();
    auto h = r->register_target(kTupleA, kIpProtoTcp, &g_mock_a, &on_mtu_a);
    ASSERT_TRUE(h.has_value());
    r->dispatch(make_icmp(kTupleA, kIpProtoUdp, 1400));
    EXPECT_EQ(g_mock_a.hits.load(), 0);
    EXPECT_EQ(r->dispatched(), 0u);
}

TEST(IcmpRegistry, DispatchWithEmbeddedInvalidIsNoop) {
    auto r = make_registry();
    reset_mocks();
    auto h = r->register_target(kTupleA, kIpProtoTcp, &g_mock_a, &on_mtu_a);
    ASSERT_TRUE(h.has_value());
    ParsedIcmp p = make_icmp(kTupleA, kIpProtoTcp, 1400);
    p.embedded_valid = false;
    r->dispatch(p);
    EXPECT_EQ(g_mock_a.hits.load(), 0);
    EXPECT_EQ(r->dispatched(), 0u);
}

TEST(IcmpRegistry, DispatchRoutesToCorrectTargetAmongMany) {
    auto r = make_registry();
    reset_mocks();
    auto ha = r->register_target(kTupleA, kIpProtoTcp, &g_mock_a, &on_mtu_a);
    auto hb = r->register_target(kTupleB, kIpProtoTcp, &g_mock_b, &on_mtu_b);
    auto hc = r->register_target(kTupleC, kIpProtoUdp, &g_mock_c, &on_mtu_c);
    ASSERT_TRUE(ha.has_value());
    ASSERT_TRUE(hb.has_value());
    ASSERT_TRUE(hc.has_value());
    r->dispatch(make_icmp(kTupleB, kIpProtoTcp, 1450));
    EXPECT_EQ(g_mock_a.hits.load(), 0);
    EXPECT_EQ(g_mock_b.hits.load(), 1);
    EXPECT_EQ(g_mock_c.hits.load(), 0);
    EXPECT_EQ(g_mock_b.last_mtu.load(), 1450u);
    EXPECT_EQ(r->dispatched(), 1u);
}

// ═══════════════════════════════════════════════════════════════════════
// RAII Handle semantics
// ═══════════════════════════════════════════════════════════════════════

TEST(IcmpRegistryHandle, DefaultConstructedIsDisengaged) {
    IcmpRegistry::Handle h;
    EXPECT_FALSE(h.engaged());
}

TEST(IcmpRegistryHandle, MoveTransfersEngagementAndOldIsDisengaged) {
    auto r = make_registry();
    reset_mocks();
    auto h1_or = r->register_target(kTupleA, kIpProtoTcp, &g_mock_a, &on_mtu_a);
    ASSERT_TRUE(h1_or.has_value());
    IcmpRegistry::Handle h1 = std::move(*h1_or);
    EXPECT_TRUE(h1.engaged());
    EXPECT_EQ(r->size(), 1u);

    IcmpRegistry::Handle h2 = std::move(h1);
    EXPECT_TRUE(h2.engaged());
    EXPECT_FALSE(h1.engaged());
    EXPECT_EQ(r->size(), 1u);
}

TEST(IcmpRegistryHandle, MoveAssignmentUnregistersOldTarget) {
    auto r = make_registry();
    reset_mocks();
    auto h1_or = r->register_target(kTupleA, kIpProtoTcp, &g_mock_a, &on_mtu_a);
    auto h2_or = r->register_target(kTupleB, kIpProtoTcp, &g_mock_b, &on_mtu_b);
    ASSERT_TRUE(h1_or.has_value());
    ASSERT_TRUE(h2_or.has_value());
    EXPECT_EQ(r->size(), 2u);

    IcmpRegistry::Handle h1 = std::move(*h1_or);
    IcmpRegistry::Handle h2 = std::move(*h2_or);

    h1 = std::move(h2);
    EXPECT_TRUE(h1.engaged());
    EXPECT_FALSE(h2.engaged());
    EXPECT_EQ(r->size(), 1u);  // A gone, B remains

    r->dispatch(make_icmp(kTupleA, kIpProtoTcp, 1400));
    r->dispatch(make_icmp(kTupleB, kIpProtoTcp, 1500));
    EXPECT_EQ(g_mock_a.hits.load(), 0);
    EXPECT_EQ(g_mock_b.hits.load(), 1);
}

TEST(IcmpRegistryHandle, SelfMoveAssignIsSafe) {
    auto r = make_registry();
    reset_mocks();
    auto h_or = r->register_target(kTupleA, kIpProtoTcp, &g_mock_a, &on_mtu_a);
    ASSERT_TRUE(h_or.has_value());
    IcmpRegistry::Handle h = std::move(*h_or);

    // Self-move-assign through a reference. (`h = std::move(h)`
    // warns; use the pointer round-trip to silence it and still
    // exercise the self-move branch.)
    IcmpRegistry::Handle& h_ref = h;
    h = std::move(h_ref);
    EXPECT_TRUE(h.engaged());
    EXPECT_EQ(r->size(), 1u);
}

// ═══════════════════════════════════════════════════════════════════════
// Re-register / swap-with-last
// ═══════════════════════════════════════════════════════════════════════

TEST(IcmpRegistry, ReRegisterAfterUnregisterSucceeds) {
    auto r = make_registry();
    reset_mocks();
    {
        auto h = r->register_target(kTupleA, kIpProtoTcp, &g_mock_a, &on_mtu_a);
        ASSERT_TRUE(h.has_value());
    }
    EXPECT_EQ(r->size(), 0u);

    auto h2 = r->register_target(kTupleA, kIpProtoTcp, &g_mock_b, &on_mtu_b);
    ASSERT_TRUE(h2.has_value()) << h2.error().detail;

    r->dispatch(make_icmp(kTupleA, kIpProtoTcp, 1200));
    EXPECT_EQ(g_mock_a.hits.load(), 0);
    EXPECT_EQ(g_mock_b.hits.load(), 1);
}

TEST(IcmpRegistry, SwapWithLastCompactionPreservesOtherEntries) {
    auto r = make_registry();
    reset_mocks();
    auto ha_or = r->register_target(kTupleA, kIpProtoTcp, &g_mock_a, &on_mtu_a);
    auto hb_or = r->register_target(kTupleB, kIpProtoTcp, &g_mock_b, &on_mtu_b);
    auto hc_or = r->register_target(kTupleC, kIpProtoTcp, &g_mock_c, &on_mtu_c);
    ASSERT_TRUE(ha_or.has_value());
    ASSERT_TRUE(hb_or.has_value());
    ASSERT_TRUE(hc_or.has_value());

    {
        IcmpRegistry::Handle hb = std::move(*hb_or);
        (void)hb;
    }
    EXPECT_EQ(r->size(), 2u);

    r->dispatch(make_icmp(kTupleA, kIpProtoTcp, 1400));
    r->dispatch(make_icmp(kTupleC, kIpProtoTcp, 1500));
    r->dispatch(make_icmp(kTupleB, kIpProtoTcp, 9999));
    EXPECT_EQ(g_mock_a.hits.load(), 1);
    EXPECT_EQ(g_mock_b.hits.load(), 0);
    EXPECT_EQ(g_mock_c.hits.load(), 1);
}

// ═══════════════════════════════════════════════════════════════════════
// Major 2 root-fix lifecycle pins
// ═══════════════════════════════════════════════════════════════════════

TEST(IcmpRegistryLifecycle, RegistryDiesBeforeHandle) {
    // The canonical UAF scenario: user accidentally held the Stream's
    // Handle in scope past the "owning" Platform / Registry. With the
    // old raw-pointer design the Handle dtor would deref freed memory.
    // With weak_ptr the Handle safely observes that registry is gone
    // and skips unregister.
    //
    // Simulate by letting `reg` drop while an active Handle is still
    // alive below it.
    IcmpRegistry::Handle handle;
    reset_mocks();
    {
        auto reg = make_registry();
        auto h_or = reg->register_target(kTupleA, kIpProtoTcp,
                                          &g_mock_a, &on_mtu_a);
        ASSERT_TRUE(h_or.has_value());
        handle = std::move(*h_or);
        EXPECT_TRUE(handle.engaged());
        // `reg` goes out of scope here → shared_ptr ref drops to 0
        // → registry freed → handle's weak_ptr.lock() now returns
        // empty forever after.
    }
    // handle is still alive and still engaged_=true. Dtor at the end
    // of this test must be safe (the weak_ptr lock will return empty,
    // so unregister is skipped).
    // Test body ends here → handle.~Handle() runs under ASan — if
    // we deref freed memory ASan will scream.
    SUCCEED();
}

TEST(IcmpRegistryLifecycle, MultipleSharedRefsKeepRegistryAlive) {
    // Two shared owners; dropping one doesn't destroy the registry.
    // Pin the ref-count semantics that let a Poller's closure keep
    // the registry alive after Platform::Impl drops its reference.
    reset_mocks();
    auto r1 = make_registry();
    auto h_or = r1->register_target(kTupleA, kIpProtoTcp,
                                     &g_mock_a, &on_mtu_a);
    ASSERT_TRUE(h_or.has_value());
    auto handle = std::move(*h_or);

    // Second strong ref — simulates the Poller closure capturing
    // the same shared_ptr by value.
    std::shared_ptr<IcmpRegistry> r2 = r1;
    EXPECT_EQ(r1.use_count(), 2);

    // Drop r1 (simulates Platform::Impl destruction).
    r1.reset();
    EXPECT_EQ(r2.use_count(), 1);

    // Registry alive via r2 → dispatch still works.
    r2->dispatch(make_icmp(kTupleA, kIpProtoTcp, 1400));
    EXPECT_EQ(g_mock_a.hits.load(), 1);

    // handle dtor at scope end: weak_ptr still valid → unregister runs
}

TEST(IcmpRegistryLifecycle, ExtraRefOutlivesOriginalOwner) {
    // Extension of the above: the "extra" shared ref (simulating a
    // Poller closure) outlives the original owner AND the registered
    // Handle. Verifies that the registry stays alive the whole time,
    // and that dispatch after Handle-gone is a safe no-op (no hit).
    reset_mocks();
    auto owner = make_registry();
    std::shared_ptr<IcmpRegistry> closure_ref = owner;

    {
        auto h = owner->register_target(kTupleA, kIpProtoTcp,
                                         &g_mock_a, &on_mtu_a);
        ASSERT_TRUE(h.has_value());
        closure_ref->dispatch(make_icmp(kTupleA, kIpProtoTcp, 1500));
        EXPECT_EQ(g_mock_a.hits.load(), 1);
    }  // Handle dtor → unregister

    // Owner drops; only closure_ref remains.
    owner.reset();

    // Dispatch through closure_ref — registry alive, but no targets.
    closure_ref->dispatch(make_icmp(kTupleA, kIpProtoTcp, 9999));
    EXPECT_EQ(g_mock_a.hits.load(), 1)  // unchanged
        << "registry without registered target must not fire callback";
}

TEST(IcmpRegistryLifecycle, MoveConstructHandleAfterRegistryDies) {
    // weak_ptr move-construction is cheap and doesn't observe
    // expiration. Verify Handle move-ctor works even when source's
    // weak_ptr has already expired, and the moved handle's dtor
    // still safely no-ops.
    IcmpRegistry::Handle src;
    reset_mocks();
    {
        auto reg = make_registry();
        auto h_or = reg->register_target(kTupleA, kIpProtoTcp,
                                          &g_mock_a, &on_mtu_a);
        ASSERT_TRUE(h_or.has_value());
        src = std::move(*h_or);
    }  // registry destroyed, src's weak_ptr now expired

    // Move-ctor from expired source.
    IcmpRegistry::Handle dst(std::move(src));
    EXPECT_FALSE(src.engaged());
    EXPECT_TRUE(dst.engaged());  // still marked engaged — will no-op on dtor

    // dst's dtor at end of scope: lock() returns empty → safe no-op.
    SUCCEED();
}

TEST(IcmpRegistryLifecycle, DispatchCallbackInvokedUnderLockNonReentrant) {
    // Pin the post-e085dd41 contract: `dispatch()` invokes the callback
    // WHILE `mu_` is held. This was a deliberate semantic flip from the
    // earlier "snapshot (stream, cb) under lock then invoke unlocked"
    // shape — the unlocked invocation opened a UAF window where
    // `~Stream` on another thread could remove the slot AND destroy the
    // stream object between snapshot and invoke. Holding the lock
    // through the callback closes that window (~Stream blocks at
    // `Handle::release_()` on `mu_` until the callback returns).
    //
    // Trade-off: the callback MUST NOT call back into the registry
    // (`register_target`, `unregister`, `size`, `dispatched`) on the
    // same thread — `mu_` is non-recursive and recursion is UB
    // (typically deadlock). The single production callback
    // (`DpdkTcpStream::on_icmp_mtu_thunk_`) only forwards into
    // `TcpSession::on_icmp_frag_needed`, which is registry-free.
    //
    // This test PINs both halves of the contract:
    //   (a) the callback is observably invoked — sanity that we didn't
    //       silently drop the dispatch path,
    //   (b) a *different* thread attempting `register_target` during
    //       the callback is observably blocked and proceeds only after
    //       dispatch returns — proving the lock is held through the
    //       callback.
    //
    // The earlier name `DispatchCallbackCanReenterRegistry` and its
    // recursive-unregister-from-callback body asserted the OPPOSITE
    // (out-of-lock invocation); they predated e085dd41's fix and
    // deadlocked when run, hanging the whole test binary. Replaced
    // here with a contract PIN that actively defends the current shape.
    reset_mocks();
    auto r = make_registry();

    // Static signalling state — `MtuCallback` is `void(*)(void*, uint16_t)
    // noexcept` (raw fn pointer; capturing lambda cannot decay to it),
    // so the callback reads/writes static atomics. Re-initialised at
    // the start of this test; no other test exercises these symbols.
    static std::atomic<bool> s_cb_started{false};
    static std::atomic<bool> s_cb_done{false};
    static std::atomic<bool> s_peer_returned{false};
    s_cb_started.store(false);
    s_cb_done.store(false);
    s_peer_returned.store(false);

    auto on_mtu_signal = [](void* /*user*/, uint16_t mtu) noexcept {
        s_cb_started.store(true, std::memory_order_release);
        // Hold the callback open just long enough for the peer thread
        // to attempt register_target and observably block on `mu_`.
        // 50ms is well over the 1-2µs cost of a blocked
        // std::mutex::lock(); 100% deterministic on busy CI runners
        // and dev boxes alike.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // While we sleep, the peer's register_target MUST still be
        // blocked — assert that fact before exiting the callback.
        EXPECT_FALSE(s_peer_returned.load(std::memory_order_acquire))
            << "peer register_target completed while callback was still "
               "running — `mu_` is not held through the callback";
        g_mock_a.last_mtu.store(mtu, std::memory_order_relaxed);
        g_mock_a.hits.fetch_add(1, std::memory_order_relaxed);
        s_cb_done.store(true, std::memory_order_release);
    };

    auto ha = r->register_target(kTupleA, kIpProtoTcp,
                                  &g_mock_a, +on_mtu_signal);
    ASSERT_TRUE(ha.has_value()) << ha.error().detail;

    std::thread peer([&]{
        // Spin until the dispatch thread reports it's inside the
        // callback, then attempt to register a new (kTupleB) target —
        // that call must take `mu_`, blocking until the callback
        // returns and dispatch unwinds.
        while (!s_cb_started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        auto hb = r->register_target(kTupleB, kIpProtoTcp,
                                      &g_mock_b, &on_mtu_b);
        s_peer_returned.store(true, std::memory_order_release);
        EXPECT_TRUE(hb.has_value()) << hb.error().detail;
        // hb dtor runs at scope exit → unregister kTupleB; safe by
        // contract (different thread from dispatch, not callback re-entry).
    });

    r->dispatch(make_icmp(kTupleA, kIpProtoTcp, 1300));

    // Callback ran, hit count incremented, peer's register_target
    // observed mu_ contention but eventually succeeded after dispatch
    // released the lock.
    peer.join();
    EXPECT_TRUE(s_cb_done.load(std::memory_order_acquire));
    EXPECT_EQ(g_mock_a.hits.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(g_mock_a.last_mtu.load(std::memory_order_relaxed), 1300);
    EXPECT_TRUE(s_peer_returned.load(std::memory_order_acquire));

    // After peer's hb dtor + ha dtor below, registry should be empty.
    // (ha dtor at scope exit.)
    EXPECT_EQ(r->size(), 1u);  // only kTupleA still registered (ha not yet dtor'd)
}

TEST(IcmpRegistryLifecycle, ConcurrentRegisterUnregisterDispatch) {
    // Stress the mutex: two threads.
    //   T1 (writer): cycle register → drop Handle in a tight loop.
    //   T2 (reader): hammer dispatch against the same tuple.
    //
    // On the old (lock-free) code this would data-race: TSan would
    // report concurrent read/write on `targets_` / `n_targets_`, and
    // under ASan the reader could observe a half-updated slot.
    //
    // With the mutex, all operations are serialized → no race, no
    // torn reads, no segfaults.
    reset_mocks();
    auto r = make_registry();

    constexpr auto kDuration = std::chrono::milliseconds(50);
    std::atomic<bool> stop{false};
    std::atomic<int>  register_successes{0};
    std::atomic<int>  register_failures{0};  // transiently expected
    std::atomic<int>  dispatch_hits{0};

    std::thread writer([&]{
        while (!stop.load(std::memory_order_relaxed)) {
            auto h = r->register_target(kTupleA, kIpProtoTcp,
                                         &g_mock_a, &on_mtu_a);
            if (h) {
                register_successes.fetch_add(1, std::memory_order_relaxed);
                // Handle dtor at scope bottom → unregister
            } else {
                register_failures.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    std::thread reader([&]{
        while (!stop.load(std::memory_order_relaxed)) {
            auto before = r->dispatched();
            r->dispatch(make_icmp(kTupleA, kIpProtoTcp, 1400));
            if (r->dispatched() > before) {
                dispatch_hits.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    std::this_thread::sleep_for(kDuration);
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    reader.join();

    // Sanity: both threads made real progress (the mutex isn't
    // pathologically starving either side).
    EXPECT_GT(register_successes.load(), 100)
        << "writer made too little progress — possible starvation / deadlock";
    // reader's dispatch_hits can be 0 legitimately if the writer's
    // register/unregister cycles never land between two dispatches,
    // but in practice we'll see thousands.

    // After joins, registry should have 0 targets (last writer
    // iteration's Handle went out of scope).
    EXPECT_EQ(r->size(), 0u);

    // The fact we got here without ASan/TSan report is the primary
    // success signal. gtest has no hook to assert sanitizer clean,
    // so this test's value is realized under `xmake f -m tsan`.
    SUCCEED();
}
