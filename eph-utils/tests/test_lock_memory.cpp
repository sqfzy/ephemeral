/// @file test_lock_memory.cpp
/// @brief Coverage for `eph::utils::lock_memory` — the mlockall wrapper
/// used by the latency bench client + mockex to immunize the hot path
/// against page-fault tail spikes.
///
/// Tests focus on contract behavior (no-op flags, error message shape)
/// because a real mlockall success path is environment-dependent
/// (RLIMIT_MEMLOCK, CAP_IPC_LOCK). The success path is asserted
/// indirectly via "no error returned when caller has the right caps"
/// — skipped automatically when running unprivileged with a low
/// memlock ulimit.

#include <gtest/gtest.h>

#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#include "eph/utils/cpu.hpp"

namespace {

bool can_mlockall_succeed() {
    // Probe: try mlockall with no flags is a no-op (returns 0 on Linux).
    // Use it as a cheap "is mlockall callable here" check. The real
    // success-path test below will only run when this returns true AND
    // the user has sudo/cap_ipc_lock — otherwise we skip.
    if (geteuid() == 0) return true;
    rlimit rl{};
    if (::getrlimit(RLIMIT_MEMLOCK, &rl) != 0) return false;
    // Need at least the working set + a margin. 64 MiB covers the test
    // binary + gtest fixture + libstdc++.
    return rl.rlim_cur >= 64ull * 1024ull * 1024ull;
}

} // namespace

TEST(LockMemory, NoFlagsIsNoOp) {
    eph::utils::LockMemoryOptions opts{
        .current = false, .future = false, .on_fault = false};
    auto r = eph::utils::lock_memory(opts, "test_no_flags");
    EXPECT_TRUE(r.has_value())
        << "lock_memory with all-false flags must succeed as a no-op";
}

TEST(LockMemory, SuccessWhenPrivileged) {
    if (!can_mlockall_succeed()) {
        GTEST_SKIP()
            << "skipping mlockall success-path: not root and "
               "RLIMIT_MEMLOCK is too small to lock the test binary's "
               "working set. Re-run as root or with `ulimit -l unlimited`.";
    }
    auto r = eph::utils::lock_memory(
        {.current = true, .future = true, .on_fault = false},
        "test_success");
    EXPECT_TRUE(r.has_value())
        << "lock_memory should succeed when caller has the privileges; "
           "got error: " << (r ? std::string{} : r.error());
    // Best-effort cleanup so this test doesn't leave the rest of the
    // suite holding pages locked. munlockall ignores failure.
    (void)::munlockall();
}

TEST(LockMemory, EpermPathProducesActionableError) {
    if (geteuid() == 0) {
        GTEST_SKIP() << "running as root; cannot exercise EPERM path";
    }
    // Probing with only MCL_FUTURE under a constrained memlock ulimit
    // typically returns EPERM (or ENOMEM); either is fine — we just
    // want to verify the error message carries an actionable hint.
    rlimit rl{};
    if (::getrlimit(RLIMIT_MEMLOCK, &rl) == 0 &&
        rl.rlim_cur >= 64ull * 1024ull * 1024ull) {
        GTEST_SKIP()
            << "RLIMIT_MEMLOCK too generous — would succeed; can't test "
               "the failure path here";
    }
    auto r = eph::utils::lock_memory(
        {.current = true, .future = true, .on_fault = false},
        "test_eperm");
    if (r.has_value()) {
        // Some unprivileged environments succeed (containers with elevated
        // memlock). Treat as non-failure but verify cleanup.
        (void)::munlockall();
        SUCCEED() << "mlockall succeeded under unprivileged user "
                     "(elevated memlock?); not a failure";
        return;
    }
    const std::string& err = r.error();
    EXPECT_NE(err.find("lock_memory"), std::string::npos)
        << "error must carry the helper tag: " << err;
    // Either the EPERM hint or the ENOMEM hint must appear.
    const bool has_actionable_hint =
        err.find("CAP_IPC_LOCK") != std::string::npos ||
        err.find("RLIMIT_MEMLOCK") != std::string::npos ||
        err.find("ulimit") != std::string::npos;
    EXPECT_TRUE(has_actionable_hint)
        << "error message lacks actionable diagnostic: " << err;
}
