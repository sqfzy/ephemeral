/// @file test_poller_modify_interest.cpp
/// Unit tests for `KernelPoller::modify_interest_` — the dynamic epoll
/// interest re-arm used by the non-blocking connect state machine to add
/// EPOLLOUT during a handshake and drop it once Established.
///
/// Covers:
///   - default registration (EPOLLIN) does NOT fire poll_once_ on a socket
///     that is only writable (not readable)
///   - re-arming to EPOLLIN|EPOLLOUT makes poll() dispatch the writable
///     pollable
///   - re-arming back to EPOLLIN-only stops the writable wakeups
///   - modify_interest_ on an unregistered obj returns NotFound
///   - modify_interest_ on nullptr returns InvalidConfig

#include <gtest/gtest.h>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>

#include "eph/net/kernel/poller.hpp"

namespace ek = eph::net::kernel;

// ---------------------------------------------------------------------------
// A KernelPollable backed by one end of a socketpair. The peer end is held
// open and never read, so our end is always *writable* but never *readable*
// — exactly what we need to tell EPOLLIN-only from EPOLLOUT interest apart.
// ---------------------------------------------------------------------------

struct WritablePollable {
    using PacketView = int;  // concept needs the type; value unused

    int               fd_{-1};
    int               peer_{-1};
    ek::KernelPoller* attached_{nullptr};
    std::atomic<std::size_t> polls_{0};

    WritablePollable() {
        int sv[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
            fd_   = sv[0];
            peer_ = sv[1];
        }
    }
    ~WritablePollable() {
        if (fd_ >= 0)   ::close(fd_);
        if (peer_ >= 0) ::close(peer_);
    }
    WritablePollable(const WritablePollable&)            = delete;
    WritablePollable& operator=(const WritablePollable&) = delete;

    [[nodiscard]] int fd() const noexcept { return fd_; }

    std::size_t poll_once_() noexcept {
        // Count dispatches; a non-zero counter delta proves the interest
        // mask fired and the Poller dispatched to us.
        polls_.fetch_add(1);
        return 1;
    }

    void notify_attached_(ek::KernelPoller* p) noexcept { attached_ = p; }
    void notify_detached_() noexcept { attached_ = nullptr; }
    [[nodiscard]] bool is_attached_() const noexcept { return attached_ != nullptr; }
    [[nodiscard]] void* native_handle() noexcept {
        return reinterpret_cast<void*>(static_cast<std::intptr_t>(fd_));
    }
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(PollerModifyInterest, EpollinOnlyDoesNotFireOnWritableSocket) {
    auto poller = ek::KernelPoller::create();
    ASSERT_TRUE(poller.has_value());
    WritablePollable p;
    ASSERT_GE(p.fd(), 0);
    ASSERT_TRUE((*poller)->add(&p).has_value());

    // Socket is writable but has no inbound data → EPOLLIN must not dispatch.
    const std::size_t before = p.polls_.load();
    (void)(*poller)->poll();
    EXPECT_EQ(p.polls_.load(), before);

    ASSERT_TRUE((*poller)->remove(&p).has_value());
}

TEST(PollerModifyInterest, ReArmingEpolloutDispatchesWritablePollable) {
    auto poller = ek::KernelPoller::create();
    ASSERT_TRUE(poller.has_value());
    WritablePollable p;
    ASSERT_GE(p.fd(), 0);
    ASSERT_TRUE((*poller)->add(&p).has_value());

    // Add EPOLLOUT: now the writable socket should make poll() dispatch.
    auto m = (*poller)->modify_interest_(static_cast<void*>(&p),
                                         EPOLLIN | EPOLLOUT);
    ASSERT_TRUE(m.has_value()) << "modify_interest_: " << m.error().detail;

    const std::size_t before = p.polls_.load();
    (void)(*poller)->poll();
    EXPECT_GT(p.polls_.load(), before) << "EPOLLOUT did not dispatch";

    // Revert to EPOLLIN-only: writable wakeups stop again.
    ASSERT_TRUE((*poller)->modify_interest_(static_cast<void*>(&p), EPOLLIN)
                    .has_value());
    const std::size_t after_revert = p.polls_.load();
    (void)(*poller)->poll();
    EXPECT_EQ(p.polls_.load(), after_revert);

    ASSERT_TRUE((*poller)->remove(&p).has_value());
}

TEST(PollerModifyInterest, UnregisteredObjReturnsNotFound) {
    auto poller = ek::KernelPoller::create();
    ASSERT_TRUE(poller.has_value());
    WritablePollable p;  // never added
    auto m = (*poller)->modify_interest_(static_cast<void*>(&p), EPOLLIN);
    ASSERT_FALSE(m.has_value());
    EXPECT_EQ(m.error().code, eph::core::Error::NotFound);
}

TEST(PollerModifyInterest, NullptrReturnsInvalidConfig) {
    auto poller = ek::KernelPoller::create();
    ASSERT_TRUE(poller.has_value());
    auto m = (*poller)->modify_interest_(nullptr, EPOLLIN);
    ASSERT_FALSE(m.has_value());
    EXPECT_EQ(m.error().code, eph::core::Error::InvalidConfig);
}
