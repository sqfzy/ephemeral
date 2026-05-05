/// @file test_kernel_poller.cpp
/// Unit tests for `eph::net::kernel::KernelPoller`.
///
/// Covers:
///   - factory returns a healthy poller, concept conformance
///   - add/remove lifecycle with a minimal Pollable mock
///   - poll() returns 0 with no registrations
///   - poll() drives a registered pollable when its fd is ready
///   - heterogeneous registration (two different Pollable types) works
///     end-to-end — this is the P2 function-pointer type-erase verification
///   - destructor detaches still-attached pollables

#include <gtest/gtest.h>

#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>

#include "eph/net/concepts.hpp"
#include "eph/net/kernel/poller.hpp"

namespace ek = eph::net::kernel;

// ---------------------------------------------------------------------------
// Minimal Pollable mocks
// ---------------------------------------------------------------------------

/// @brief A pollable fed by an eventfd. Each `poll_once_` call drains the
///        eventfd's counter and reports the value as "frames processed".
///        Used to verify add/remove/poll without pulling in KernelTcpStream.
struct EventfdPollable {
    using PacketView = int;  // concept needs the type to exist; value unused

    int           fd_{-1};
    ek::KernelPoller* attached_{nullptr};
    std::atomic<std::size_t> seen_{0};

    EventfdPollable() {
        fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    }
    ~EventfdPollable() {
        if (fd_ >= 0) ::close(fd_);
    }
    EventfdPollable(const EventfdPollable&)            = delete;
    EventfdPollable& operator=(const EventfdPollable&) = delete;

    [[nodiscard]] int fd() const noexcept { return fd_; }

    /// @brief Drain the eventfd counter. Returns the counter value.
    std::size_t poll_once_() noexcept {
        uint64_t v = 0;
        ssize_t n = ::read(fd_, &v, sizeof(v));
        if (n != sizeof(v)) return 0;
        seen_.fetch_add(static_cast<std::size_t>(v));
        return static_cast<std::size_t>(v);
    }

    void notify_attached_(ek::KernelPoller* p) noexcept { attached_ = p; }
    void notify_detached_() noexcept { attached_ = nullptr; }

    [[nodiscard]] bool is_attached_() const noexcept {
        return attached_ != nullptr;
    }
    [[nodiscard]] void* native_handle() noexcept {
        return reinterpret_cast<void*>(static_cast<std::intptr_t>(fd_));
    }
};

/// @brief A second, distinct pollable type to prove the Poller handles
///        heterogeneous registrations via function-pointer type erase.
struct PipePollable {
    using PacketView = int;

    int pipe_rd_{-1};
    int pipe_wr_{-1};
    ek::KernelPoller* attached_{nullptr};
    std::atomic<std::size_t> reads_{0};

    PipePollable() {
        int fds[2];
        if (::pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0) {
            pipe_rd_ = fds[0];
            pipe_wr_ = fds[1];
        }
    }
    ~PipePollable() {
        if (pipe_rd_ >= 0) ::close(pipe_rd_);
        if (pipe_wr_ >= 0) ::close(pipe_wr_);
    }
    PipePollable(const PipePollable&)            = delete;
    PipePollable& operator=(const PipePollable&) = delete;

    [[nodiscard]] int fd() const noexcept { return pipe_rd_; }

    /// @brief Drain the pipe one buffer at a time.
    std::size_t poll_once_() noexcept {
        char buf[256];
        ssize_t n = ::read(pipe_rd_, buf, sizeof(buf));
        if (n > 0) {
            reads_.fetch_add(1);
            return 1;
        }
        return 0;
    }

    void notify_attached_(ek::KernelPoller* p) noexcept { attached_ = p; }
    void notify_detached_() noexcept { attached_ = nullptr; }

    [[nodiscard]] bool is_attached_() const noexcept {
        return attached_ != nullptr;
    }
    [[nodiscard]] void* native_handle() noexcept {
        return reinterpret_cast<void*>(
            static_cast<std::intptr_t>(pipe_rd_));
    }
};

// ---------------------------------------------------------------------------
// Concept conformance
// ---------------------------------------------------------------------------

static_assert(eph::net::Poller<ek::KernelPoller>,
              "KernelPoller must satisfy eph::net::Poller");

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(KernelPoller, CreateReturnsHealthyInstance) {
    auto pr = ek::KernelPoller::create();
    ASSERT_TRUE(pr.has_value()) << pr.error().detail;
    auto p = std::move(*pr);
    ASSERT_NE(p.get(), nullptr);
    EXPECT_GE(p->epoll_fd(), 0);
    EXPECT_EQ(p->size(), 0u);
}

TEST(KernelPoller, PollWithNoRegistrationsReturnsZero) {
    auto p = ek::KernelPoller::create().value();
    EXPECT_EQ(p->poll(), 0u);
    EXPECT_EQ(p->poll(std::chrono::milliseconds{1}), 0u);
}

TEST(KernelPoller, AddRemoveLifecycle) {
    auto p = ek::KernelPoller::create().value();
    EventfdPollable ev;
    ASSERT_GE(ev.fd(), 0);

    auto ar = p->add(&ev);
    ASSERT_TRUE(ar.has_value()) << ar.error().detail;
    EXPECT_EQ(p->size(), 1u);
    EXPECT_TRUE(ev.is_attached_());

    // Duplicate add is rejected.
    auto ar2 = p->add(&ev);
    EXPECT_FALSE(ar2.has_value());
    EXPECT_EQ(ar2.error().code, eph::core::Error::InvalidConfig);

    auto rr = p->remove(&ev);
    ASSERT_TRUE(rr.has_value()) << rr.error().detail;
    EXPECT_EQ(p->size(), 0u);
    EXPECT_FALSE(ev.is_attached_());

    // Remove of non-registered pollable fails — symmetric with the DPDK
    // sibling (DpdkPoller::remove): nullptr is `InvalidConfig` (caller
    // programming error), not-registered is `NotFound` (recoverable
    // state mismatch). Pinning the distinction here so a future regression
    // that collapses both back into `InvalidConfig` trips this assert.
    auto rr2 = p->remove(&ev);
    EXPECT_FALSE(rr2.has_value());
    EXPECT_EQ(rr2.error().code, eph::core::Error::NotFound);
}

TEST(KernelPoller, PollDrivesReadyEventfd) {
    auto p = ek::KernelPoller::create().value();
    EventfdPollable ev;
    ASSERT_TRUE(p->add(&ev).has_value());

    // Signal 3 on the eventfd — epoll sees EPOLLIN, poll() dispatches to
    // the pollable, which reads the counter value and reports 3.
    uint64_t v = 3;
    ASSERT_EQ(::write(ev.fd(), &v, sizeof(v)), (ssize_t)sizeof(v));

    const std::size_t n = p->poll(std::chrono::milliseconds{100});
    EXPECT_EQ(n, 3u);
    EXPECT_EQ(ev.seen_.load(), 3u);
}

TEST(KernelPoller, PollHeterogeneousPollables) {
    // Register both an EventfdPollable and a PipePollable on the same
    // Poller. This verifies that the function-pointer type-erase works
    // across distinct Pollable types (the P2 design point).
    auto p = ek::KernelPoller::create().value();
    EventfdPollable ev;
    PipePollable    pp;
    ASSERT_GE(ev.fd(), 0);
    ASSERT_GE(pp.fd(), 0);

    ASSERT_TRUE(p->add(&ev).has_value());
    ASSERT_TRUE(p->add(&pp).has_value());
    EXPECT_EQ(p->size(), 2u);

    uint64_t v = 5;
    ASSERT_EQ(::write(ev.fd(), &v, sizeof(v)), (ssize_t)sizeof(v));
    const char msg[] = "pipe_data";
    ASSERT_EQ(::write(pp.pipe_wr_, msg, sizeof(msg)),
              (ssize_t)sizeof(msg));

    // Drain. We may need a couple of poll() calls since epoll_wait may
    // return events in batches.
    std::size_t total = 0;
    for (int i = 0; i < 5 && total < 6; ++i) {
        total += p->poll(std::chrono::milliseconds{50});
    }
    EXPECT_GE(total, 6u);           // 5 from eventfd + 1 from pipe
    EXPECT_EQ(ev.seen_.load(), 5u);
    EXPECT_EQ(pp.reads_.load(), 1u);
}

TEST(KernelPoller, DestructorDetachesAttachedPollables) {
    EventfdPollable ev;
    ASSERT_GE(ev.fd(), 0);
    {
        auto p = ek::KernelPoller::create().value();
        ASSERT_TRUE(p->add(&ev).has_value());
        EXPECT_TRUE(ev.is_attached_());
        // p drops out of scope here.
    }
    // The Poller's destructor must have cleared ev.attached_.
    EXPECT_FALSE(ev.is_attached_());
}

TEST(KernelPoller, PollTimeoutRespectsDeadline) {
    auto p = ek::KernelPoller::create().value();
    EventfdPollable ev;
    ASSERT_TRUE(p->add(&ev).has_value());
    // No events — poll(50ms) should block up to ~50ms then return 0.
    auto t0 = std::chrono::steady_clock::now();
    const auto n = p->poll(std::chrono::milliseconds{50});
    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT_EQ(n, 0u);
    // Generous upper bound to avoid CI flake; lower bound is the point.
    EXPECT_GE(dt, 30);
    EXPECT_LE(dt, 500);
}

// ─── add/remove pre-condition rejection paths ────────────────────────────
//
// Both `add` and `remove` have nullptr-rejection guards before any
// epoll_ctl interaction. `add` additionally rejects a Pollable whose
// `fd() < 0` (already closed) so a zombie object can't accidentally
// hijack the kernel's negative-fd error semantics. Lock all three.

TEST(KernelPoller, AddNullptrRejected) {
    auto p = ek::KernelPoller::create().value();
    auto r = p->add(static_cast<EventfdPollable*>(nullptr));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
    // Detail string must mention nullptr so log greps surface the misuse.
    EXPECT_NE(std::string_view{r.error().detail}.find("nullptr"),
              std::string_view::npos)
        << "detail: " << r.error().detail;
    // Empty entries list — guard fired before any state mutation.
    EXPECT_EQ(p->size(), 0u);
}

TEST(KernelPoller, RemoveNullptrRejected) {
    auto p = ek::KernelPoller::create().value();
    auto r = p->remove(static_cast<EventfdPollable*>(nullptr));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
    EXPECT_NE(std::string_view{r.error().detail}.find("nullptr"),
              std::string_view::npos)
        << "detail: " << r.error().detail;
}

/// Minimal pollable that always reports a closed fd. Used to drive the
/// `KernelPoller::add` "closed fd" rejection path without needing to
/// race a real socket close.
struct ClosedFdPollable {
    using PacketView = int;
    ek::KernelPoller* attached_{nullptr};
    [[nodiscard]] int fd() const noexcept { return -1; }
    std::size_t poll_once_() noexcept { return 0; }
    void notify_attached_(ek::KernelPoller* p) noexcept { attached_ = p; }
    void notify_detached_() noexcept { attached_ = nullptr; }
    [[nodiscard]] bool is_attached_() const noexcept {
        return attached_ != nullptr;
    }
};

TEST(KernelPoller, AddPollableWithClosedFdRejected) {
    auto p = ek::KernelPoller::create().value();
    ClosedFdPollable bad;
    auto r = p->add(&bad);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
    // Detail must mention "closed fd" so an operator can correlate.
    EXPECT_NE(std::string_view{r.error().detail}.find("closed fd"),
              std::string_view::npos)
        << "detail: " << r.error().detail;
    // notify_attached_ must NOT have fired — guard ran before the
    // hook would be reached.
    EXPECT_EQ(bad.attached_, nullptr);
    EXPECT_EQ(p->size(), 0u);
}

/// `add()` must reject pollables already attached to a different poller.
/// Without this guard, `notify_attached_(p2)` would silently overwrite
/// `attached_to_` to point at the new poller while the original poller
/// retained a stale entries_ row. Subsequent ~obj would dispatch
/// remove() to p2 and leave p1 with a dangling entry that surfaces as
/// the "orphan event" path in poll_impl_.
TEST(KernelPoller, AddRejectsAlreadyAttachedToOtherPoller) {
    auto p1 = ek::KernelPoller::create().value();
    auto p2 = ek::KernelPoller::create().value();

    EventfdPollable ev;
    ASSERT_GE(ev.fd(), 0);

    ASSERT_TRUE(p1->add(&ev).has_value());
    ASSERT_TRUE(ev.is_attached_());
    EXPECT_EQ(ev.attached_, p1.get());

    auto r = p2->add(&ev);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
    EXPECT_NE(std::string_view{r.error().detail}.find("already attached"),
              std::string_view::npos)
        << "detail: " << r.error().detail;

    // p1 still owns the registration; p2 is empty; ev still points at p1.
    EXPECT_EQ(p1->size(), 1u);
    EXPECT_EQ(p2->size(), 0u);
    EXPECT_EQ(ev.attached_, p1.get());

    // Clean up so ~p1 doesn't dangle past ~ev.
    ASSERT_TRUE(p1->remove(&ev).has_value());
}

// AddRejectsAlreadyAttachedToOtherPoller covers the cross-poller case
// (poller2 rejects an obj already attached to poller1) — but the
// SAME-poller duplicate-add case is a distinct code path: the
// is_attached_() guard at poller.hpp:192 catches it on the SAME poller
// too (obj reports as attached because we attached it ourselves), but
// the symmetric coverage was missing. Without this test, a future
// refactor that loosened the is_attached_() check (e.g. to allow
// "re-add to same poller is idempotent") would silently succeed,
// double-registering the fd in epoll and producing duplicate
// poll_once_() invocations per event.
TEST(KernelPoller, AddRejectsSamePollerDuplicate) {
    auto p = ek::KernelPoller::create().value();

    EventfdPollable ev;
    ASSERT_GE(ev.fd(), 0);

    // First add succeeds.
    ASSERT_TRUE(p->add(&ev).has_value());
    EXPECT_TRUE(ev.is_attached_());
    EXPECT_EQ(p->size(), 1u);

    // Second add to the SAME poller must reject. The is_attached_()
    // guard (poller.hpp:192) fires before we get to the duplicate-fd
    // detection at line 204; either path is fine, but the rejection
    // contract is what we lock here.
    auto r = p->add(&ev);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
    // entries_ size must be unchanged — the duplicate add must NOT
    // have inserted a second entry behind the rejection log.
    EXPECT_EQ(p->size(), 1u)
        << "duplicate add increased entries_ size — the fd is now "
        << "registered twice in epoll, poll_once_ will fire twice "
        << "per event";

    ASSERT_TRUE(p->remove(&ev).has_value());
}
