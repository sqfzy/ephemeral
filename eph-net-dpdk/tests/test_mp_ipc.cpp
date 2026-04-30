/// @file test_mp_ipc.cpp
/// Unit tests for `detail::mp_ipc` — the typed RAII wrapper around
/// DPDK's `rte_mp_*` cross-process IPC.
///
/// These tests boot a single EAL instance via `dpdk_test_env` (--no-pci
/// --no-huge), then exercise the wrapper *within one process* by
/// registering handlers and sending messages to ourselves through the
/// loopback path that DPDK provides when no peer is configured. The
/// real cross-process behavior is exercised in stage 3's e2e binary.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

#include <gtest/gtest.h>

#include "dpdk_test_env.hpp"

#include "eph/core/error.hpp"
#include "eph/dpdk/detail/mp_ipc.hpp"

using eph::core::Error;
using eph::dpdk::detail::kMpIpcMaxNameLen;
using eph::dpdk::detail::kMpIpcMaxParamLen;
using eph::dpdk::detail::MpIpcAction;
using eph::dpdk::detail::pack_msg;
using eph::dpdk::detail::parse_payload;

namespace {

struct TinyMsg {
    uint32_t magic;
    uint32_t value;
};
static_assert(sizeof(TinyMsg) == 8);

struct OversizeMsg {
    uint8_t buf[kMpIpcMaxParamLen + 1];   // exceeds RTE_MP_MAX_PARAM_LEN
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// pack_msg / parse_payload — pure logic, no EAL touch
// ─────────────────────────────────────────────────────────────────────────────

TEST(MpIpcPack, RoundtripPodPayload) {
    TinyMsg in{.magic = 0xDEADBEEF, .value = 42};
    rte_mp_msg msg{};
    ASSERT_TRUE(pack_msg(msg, "eph_test_action", in));
    EXPECT_STREQ(msg.name, "eph_test_action");
    EXPECT_EQ(msg.len_param, static_cast<int>(sizeof(TinyMsg)));
    EXPECT_EQ(msg.num_fds, 0);

    auto parsed = parse_payload<TinyMsg>(&msg);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->magic, in.magic);
    EXPECT_EQ(parsed->value, in.value);
}

TEST(MpIpcPack, EmptyName_Rejected) {
    TinyMsg in{};
    rte_mp_msg msg{};
    EXPECT_FALSE(pack_msg(msg, "", in));
}

TEST(MpIpcPack, OversizeName_Rejected) {
    TinyMsg in{};
    rte_mp_msg msg{};
    std::string huge(kMpIpcMaxNameLen + 5, 'x');
    EXPECT_FALSE(pack_msg(msg, huge, in));
}

TEST(MpIpcParse, NullMsg_ReturnsInvalidConfig) {
    auto r = parse_payload<TinyMsg>(nullptr);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::InvalidConfig);
}

TEST(MpIpcParse, LenMismatch_ReturnsInvalidConfig) {
    rte_mp_msg msg{};
    msg.len_param = 99;   // wrong size
    auto r = parse_payload<TinyMsg>(&msg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::InvalidConfig);
}

// ─────────────────────────────────────────────────────────────────────────────
// MpIpcAction — RAII registration (needs EAL)
// ─────────────────────────────────────────────────────────────────────────────

namespace {

std::atomic<int> g_handler_invocations{0};

int test_handler_thunk(const rte_mp_msg* /*msg*/, const void* /*peer*/) {
    g_handler_invocations.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

} // namespace

TEST(MpIpcAction, RegisterUnique_OwnsRegistration) {
    g_handler_invocations.store(0);
    {
        MpIpcAction action("eph_test_unique_a", &test_handler_thunk);
        EXPECT_TRUE(static_cast<bool>(action));
        EXPECT_EQ(action.name(), "eph_test_unique_a");
    }
    // Re-registering same name after RAII unregister should succeed.
    MpIpcAction reregister("eph_test_unique_a", &test_handler_thunk);
    EXPECT_TRUE(static_cast<bool>(reregister));
}

TEST(MpIpcAction, EmptyName_Degrades) {
    MpIpcAction action("", &test_handler_thunk);
    EXPECT_FALSE(static_cast<bool>(action));
}

TEST(MpIpcAction, NullHandler_Degrades) {
    MpIpcAction action("eph_test_null_handler", nullptr);
    EXPECT_FALSE(static_cast<bool>(action));
}

TEST(MpIpcAction, OversizeName_Degrades) {
    std::string huge(kMpIpcMaxNameLen + 1, 'a');
    MpIpcAction action(huge, &test_handler_thunk);
    EXPECT_FALSE(static_cast<bool>(action));
}

TEST(MpIpcAction, DoubleRegisterSameName_SecondDegrades) {
    MpIpcAction first("eph_test_dup_b", &test_handler_thunk);
    ASSERT_TRUE(static_cast<bool>(first));
    // DPDK rejects duplicate; second handle reports degraded.
    MpIpcAction second("eph_test_dup_b", &test_handler_thunk);
    EXPECT_FALSE(static_cast<bool>(second));
}

TEST(MpIpcAction, MoveConstructor_TransfersOwnership) {
    MpIpcAction src("eph_test_mv_a", &test_handler_thunk);
    ASSERT_TRUE(static_cast<bool>(src));
    MpIpcAction dst(std::move(src));
    EXPECT_FALSE(static_cast<bool>(src));
    EXPECT_TRUE(static_cast<bool>(dst));
    EXPECT_EQ(dst.name(), "eph_test_mv_a");
}

TEST(MpIpcAction, MoveAssignment_UnregistersPrev) {
    MpIpcAction first("eph_test_mv_b1", &test_handler_thunk);
    MpIpcAction second("eph_test_mv_b2", &test_handler_thunk);
    ASSERT_TRUE(static_cast<bool>(first));
    ASSERT_TRUE(static_cast<bool>(second));
    first = std::move(second);
    EXPECT_TRUE(static_cast<bool>(first));
    EXPECT_EQ(first.name(), "eph_test_mv_b2");
    EXPECT_FALSE(static_cast<bool>(second));
    // After this point the original 'eph_test_mv_b1' has been
    // unregistered (first's old state freed); a fresh registration of
    // that name should succeed.
    MpIpcAction reuse("eph_test_mv_b1", &test_handler_thunk);
    EXPECT_TRUE(static_cast<bool>(reuse));
}

// ─────────────────────────────────────────────────────────────────────────────
// mp_ipc_request_sync — input-validation guards
// ─────────────────────────────────────────────────────────────────────────────

// Negative timeout would propagate to a negative tv_sec / tv_nsec on the
// timespec passed to DPDK's internal sem_timedwait. Some DPDK versions hung
// indefinitely on this; the wrapper now rejects up-front so the caller gets
// a deterministic InvalidConfig instead of a flaky stuck-RPC.
TEST(MpIpcRequestSync, NegativeTimeoutRejectedUpfront) {
    TinyMsg req{.magic = 0xCAFEBABE, .value = 7};
    auto r = eph::dpdk::detail::mp_ipc_request_sync<TinyMsg, TinyMsg>(
        "eph_test_neg_to", req, std::chrono::milliseconds{-100});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::InvalidConfig);
}

// Zero timeout is unspecified across DPDK versions ("try once" / "no-op");
// reject too so the wrapper has consistent semantics regardless of the DPDK
// version the binary links against.
TEST(MpIpcRequestSync, ZeroTimeoutRejectedUpfront) {
    TinyMsg req{};
    auto r = eph::dpdk::detail::mp_ipc_request_sync<TinyMsg, TinyMsg>(
        "eph_test_zero_to", req, std::chrono::milliseconds{0});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::InvalidConfig);
}

