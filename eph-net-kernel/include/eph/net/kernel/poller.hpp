#pragma once

/// @file poller.hpp
/// Linux epoll-based multiplexer satisfying `eph::net::Poller`.
///
/// Design summary:
///
///   - **Heterogeneous pollables via function-pointer type erasure**
///     (spec "P2"). Each registered object becomes a `PollableEntry` holding
///     `{void* obj, size_t(*poll_fn)(void*), int fd}`. `add<P>(P*)` captures
///     a thunk `+[](void* p) noexcept { return static_cast<P*>(p)->poll_once_(); }`
///     into `poll_fn`. No std::function, no vtable — pure function pointer,
///     branch-predictable and inlinable.
///
///   - **epoll (level-triggered)** by default, for forward-compatibility
///     with the Stream's blocking recv fallback. The hot path uses
///     `epoll_wait(timeout=0)` so `poll()` is non-blocking; the
///     `poll(timeout)` overload forwards the caller-specified deadline.
///
///   - **Pollable notification hooks**. Every `Pollable` type in the kernel
///     backend (`KernelTcpStream`, `KernelUdpSocket`) declares a
///     `notify_attached_(KernelPoller*)` / `notify_detached_()` friend
///     entry point. The Poller invokes these through a second function
///     pointer captured at register time — no cross-type friendship needed.
///
///   - **Not thread-safe**. One Poller belongs to exactly one user thread;
///     DPDK reactor semantics apply even for the kernel variant.
///
/// The Poller's destructor:
///   1. iterates `entries_` calling each pollable's detach-notify thunk,
///   2. closes `epoll_fd_`,
///   3. clears `entries_`.
/// so that any `KernelTcpStream` still attached at shutdown stops pointing at
/// a dangling poller.

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <vector>

#include <sys/epoll.h>
#include <unistd.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "eph/core/error.hpp"
#include "eph/net/concepts.hpp"
#include "eph/net/kernel/config.hpp"

namespace eph::net::kernel {

namespace detail {

/// @brief Lazily-initialized logger for the kernel poller subsystem.
inline spdlog::logger* poller_logger() {
    static auto* l = [] {
        auto lg = spdlog::get("net.kernel.poller");
        if (!lg) {
            try {
                lg = spdlog::stdout_color_mt("net.kernel.poller");
            } catch (const spdlog::spdlog_ex&) {
                lg = spdlog::get("net.kernel.poller");
            }
        }
        return lg.get();
    }();
    return l;
}

} // namespace detail

// Forward declaration so the Pollable notify hooks can reference us.
class KernelPoller;

// ---------------------------------------------------------------------------
// KernelPoller
// ---------------------------------------------------------------------------

/// @brief Non-owning, non-thread-safe epoll multiplexer.
class KernelPoller {
public:
    // ── Factory ──────────────────────────────────────────────────────────

    /// @brief Create a new Poller; wraps `epoll_create1(EPOLL_CLOEXEC)`.
    [[nodiscard]] static std::expected<std::unique_ptr<KernelPoller>, core::ErrorInfo>
    create(PollerConfig cfg = {}) noexcept {
        auto* log = detail::poller_logger();

        const int fd = ::epoll_create1(EPOLL_CLOEXEC);
        if (fd < 0) {
            SPDLOG_LOGGER_ERROR(log,
                "KernelPoller::create: epoll_create1 failed: {}",
                std::strerror(errno));
            return std::unexpected(core::ErrorInfo{
                core::Error::OutOfMemory,
                "KernelPoller::create: epoll_create1 failed"});
        }

        // Construct via new + unique_ptr because the constructor is private
        // (enforced via the friend declaration inside the .reset call below).
        // We use the public constructor for simplicity — the class invariants
        // are enforced by the factory alone.
        auto p = std::unique_ptr<KernelPoller>(new KernelPoller(fd, cfg));
        SPDLOG_LOGGER_DEBUG(log,
            "KernelPoller::create: epoll_fd={} max_events={}",
            fd, cfg.max_events_per_wait);
        return p;
    }

    ~KernelPoller() {
        // Notify every still-attached Pollable so its `attached_to_` pointer
        // is cleared before we destroy state. Otherwise a later ~Stream would
        // call remove() on a dangling Poller*.
        for (const auto& e : entries_) {
            if (e.detach_fn != nullptr) {
                e.detach_fn(e.obj);
            }
        }
        entries_.clear();
        if (epoll_fd_ >= 0) {
            ::close(epoll_fd_);
            epoll_fd_ = -1;
        }
    }

    KernelPoller(const KernelPoller&)            = delete;
    KernelPoller& operator=(const KernelPoller&) = delete;
    KernelPoller(KernelPoller&&)                 = delete;
    KernelPoller& operator=(KernelPoller&&)      = delete;

    // ── Registration ─────────────────────────────────────────────────────

    /// @brief Register `obj` with the Poller. The Pollable's fd must not
    ///        already be registered.
    ///
    /// `add` accepts any concrete type `P` satisfying `eph::net::Pollable`
    /// PLUS the kernel-backend extension that exposes an int-typed
    /// `native_handle()` (kernel fd) and a `notify_attached_` / `notify_detached_`
    /// friend entry point. Those extensions live in the `KernelTcpStream` /
    /// `KernelUdpSocket` headers; when those headers are not included at the
    /// add() call site, a compile error surfaces immediately.
    template <class P>
    [[nodiscard]] std::expected<void, core::ErrorInfo> add(P* obj) noexcept {
        [[maybe_unused]] auto* log = detail::poller_logger();
        if (obj == nullptr) {
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "KernelPoller::add: nullptr"});
        }
        const int fd = obj->fd();
        if (fd < 0) {
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "KernelPoller::add: pollable has closed fd"});
        }

        // Reject duplicates defensively — epoll_ctl(ADD) would return EEXIST
        // anyway but the message here is clearer.
        for (const auto& e : entries_) {
            if (e.obj == static_cast<void*>(obj)) {
                return std::unexpected(core::ErrorInfo{
                    core::Error::InvalidConfig,
                    "KernelPoller::add: already registered"});
            }
        }

        struct epoll_event ev{};
        ev.events   = EPOLLIN;  // RX-ready interest. Stream handles POLLOUT
                                // via its own send() poll fallback.
        ev.data.ptr = static_cast<void*>(obj);
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) != 0) {
            SPDLOG_LOGGER_ERROR(log,
                "KernelPoller::add: epoll_ctl(ADD) failed fd={} errno={} ({})",
                fd, errno, std::strerror(errno));
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "KernelPoller::add: epoll_ctl(ADD) failed"});
        }

        // Capture type-specific thunks. These are noexcept and compile-time
        // resolved — no std::function and no heap allocation.
        PollableEntry entry{};
        entry.obj       = static_cast<void*>(obj);
        entry.fd        = fd;
        entry.poll_fn   = +[](void* p) noexcept -> std::size_t {
            return static_cast<P*>(p)->poll_once_();
        };
        entry.detach_fn = +[](void* p) noexcept {
            static_cast<P*>(p)->notify_detached_();
        };
        entries_.push_back(entry);

        obj->notify_attached_(this);
        SPDLOG_LOGGER_DEBUG(log,
            "KernelPoller::add: fd={} entries={}", fd, entries_.size());
        return {};
    }

    /// @brief Unregister `obj`. Returns `InvalidConfig` if not registered.
    template <class P>
    [[nodiscard]] std::expected<void, core::ErrorInfo> remove(P* obj) noexcept {
        [[maybe_unused]] auto* log = detail::poller_logger();
        if (obj == nullptr) {
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "KernelPoller::remove: nullptr"});
        }
        auto it = std::find_if(entries_.begin(), entries_.end(),
            [p = static_cast<void*>(obj)](const PollableEntry& e) {
                return e.obj == p;
            });
        if (it == entries_.end()) {
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "KernelPoller::remove: not registered"});
        }
        const int fd = it->fd;
        // Best-effort EPOLL_CTL_DEL. ENOENT is acceptable (fd may have been
        // closed by a peer RST and reaped by the kernel already).
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) != 0
            && errno != ENOENT && errno != EBADF) {
            SPDLOG_LOGGER_WARN(log,
                "KernelPoller::remove: epoll_ctl(DEL) errno={} ({})",
                errno, std::strerror(errno));
        }
        entries_.erase(it);
        obj->notify_detached_();
        SPDLOG_LOGGER_DEBUG(log,
            "KernelPoller::remove: fd={} entries={}", fd, entries_.size());
        return {};
    }

    // ── Poll ─────────────────────────────────────────────────────────────

    /// @brief Non-blocking poll. Drains all ready events and returns the
    ///        aggregate count of frames delivered by the per-pollable
    ///        `poll_once_()` methods.
    std::size_t poll() noexcept {
        return poll_impl_(0);
    }

    /// @brief Poll with a blocking timeout. A timeout of 0 is equivalent to
    ///        the non-blocking overload.
    std::size_t poll(std::chrono::milliseconds timeout) noexcept {
        const int ms = (timeout.count() < 0)
            ? -1
            : static_cast<int>(std::min<long long>(
                static_cast<long long>(timeout.count()),
                static_cast<long long>(std::numeric_limits<int>::max())));
        return poll_impl_(ms);
    }

    // ── Introspection (test hooks) ───────────────────────────────────────

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] int epoll_fd() const noexcept { return epoll_fd_; }

private:
    /// @brief Type-erased entry. Function pointers only — no std::function.
    struct PollableEntry {
        void*      obj       = nullptr;
        std::size_t (*poll_fn)(void*) noexcept = nullptr;
        void     (*detach_fn)(void*) noexcept = nullptr;
        int        fd        = -1;
    };

    KernelPoller(int epfd, PollerConfig cfg) noexcept
        : epoll_fd_(epfd), cfg_(cfg) {
        entries_.reserve(cfg.initial_capacity);
    }

    /// @brief Shared poll body for both overloads; `timeout_ms` as epoll sees it.
    std::size_t poll_impl_(int timeout_ms) noexcept {
        if (epoll_fd_ < 0 || entries_.empty()) return 0;

        const int max_ev = std::max(1, cfg_.max_events_per_wait);
        // Stack-allocated events buffer sized at most 256 to avoid blowing
        // the stack; larger max_events_per_wait values fall back to the
        // 256-entry cap per wait.
        constexpr int kMaxBurst = 256;
        struct epoll_event events[kMaxBurst];
        const int n_ev_cap = std::min(max_ev, kMaxBurst);

        int n = 0;
        for (;;) {
            n = ::epoll_wait(epoll_fd_, events, n_ev_cap, timeout_ms);
            if (n >= 0) break;
            if (errno == EINTR) {
                // EINTR: if we were non-blocking, return immediately rather
                // than spinning the caller. Otherwise retry.
                if (timeout_ms == 0) return 0;
                continue;
            }
            SPDLOG_LOGGER_WARN(detail::poller_logger(),
                "KernelPoller::poll: epoll_wait errno={} ({})",
                errno, std::strerror(errno));
            return 0;
        }

        std::size_t total = 0;
        // Iterate ready events. We look up each event's obj pointer in
        // `entries_` by identity — the vector is small (typical < 16) so a
        // linear scan is fast and branch-predictable.
        for (int i = 0; i < n; ++i) {
            void* obj = events[i].data.ptr;
            // Resolve to an entry snapshot — a callback may remove itself
            // mid-poll, which would erase the entry; we must not dereference
            // the erased entry afterward.
            std::size_t (*pf)(void*) noexcept = nullptr;
            for (const auto& e : entries_) {
                if (e.obj == obj) {
                    pf = e.poll_fn;
                    break;
                }
            }
            if (pf != nullptr) {
                total += pf(obj);
            }
        }
        return total;
    }

    int          epoll_fd_{-1};
    PollerConfig cfg_{};
    std::vector<PollableEntry> entries_;
};

// ---------------------------------------------------------------------------
// Concept conformance
// ---------------------------------------------------------------------------

static_assert(eph::net::Poller<KernelPoller>,
              "KernelPoller must satisfy eph::net::Poller");

} // namespace eph::net::kernel
