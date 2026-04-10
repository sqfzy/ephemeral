#pragma once

/// @file test/test_poller.hpp
/// In-memory `Poller` for unit tests: no epoll, no DPDK, no syscalls.
///
/// Part of Phase 2 of the v3.3 refactor (see
/// `.artifacts/design-eph-v3.3-architecture-20260410.md`). `TestPoller<P>`
/// holds a flat vector of registered `P*`, and on `poll()` walks the list
/// calling `poll_once_()` on each. It also flips `attached_` on the
/// pollable (via `set_attached`) so `is_attached()` behaves correctly.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <vector>

#include "eph/core/error.hpp"
#include "eph/net/concepts.hpp"

namespace eph::net::test {

// ---------------------------------------------------------------------------
// TestPoller
// ---------------------------------------------------------------------------

/// @brief In-memory Poller mock parameterized on a concrete `Pollable` type.
///
/// Semantics:
///   - `add(p)`: pushes `p` into `registered_`; returns `InvalidConfig` if
///              the same pointer is already registered or if `p == nullptr`.
///              Calls `p->set_attached(true)` so concept-level state is
///              consistent.
///   - `remove(p)`: erases the first match; returns `InvalidConfig` if not
///              present. Flips `attached_` to false on success.
///   - `poll()`: iterates a stable snapshot of `registered_` and calls
///              `poll_once_()` on each. Returns the sum of per-pollable
///              counts. Using a snapshot is deliberate: a pollable callback
///              may `remove` itself mid-poll (e.g. on graceful close) and
///              we must not invalidate the iterator.
///   - `poll(timeout)`: identical to `poll()` — timeouts are meaningless for
///              an in-memory driver.
///
/// @tparam P  any type satisfying `eph::net::Pollable` (typically
///            `FakeStream`, `FakeDatagram`, or a user-defined mock).
template <class P>
    requires Pollable<P>
class TestPoller {
public:
    TestPoller() = default;

    [[nodiscard]] static std::unique_ptr<TestPoller> create() {
        return std::make_unique<TestPoller>();
    }

    /// @brief Register `p` with the poller. Idempotent failure on nullptr
    ///        or duplicate; success flips `p->set_attached(true)` so the
    ///        Stream / Datagram concept `is_attached()` returns true.
    std::expected<void, core::ErrorInfo> add(P* p) {
        if (p == nullptr) {
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "TestPoller::add nullptr"});
        }
        if (std::find(registered_.begin(), registered_.end(), p)
            != registered_.end()) {
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "TestPoller::add duplicate"});
        }
        registered_.push_back(p);
        // Fake pollables (FakeStream / FakeDatagram) expose a `set_attached`
        // test hook. We require it as a refinement of the generic Pollable
        // concept here so that attached state stays observable through the
        // public Stream/Datagram API.
        p->set_attached(true);
        return {};
    }

    /// @brief Unregister `p`. Returns `InvalidConfig` if `p` is not
    ///        currently registered. Flips `p->set_attached(false)` on
    ///        success.
    std::expected<void, core::ErrorInfo> remove(P* p) {
        auto it = std::find(registered_.begin(), registered_.end(), p);
        if (it == registered_.end()) {
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "TestPoller::remove not registered"});
        }
        (*it)->set_attached(false);
        registered_.erase(it);
        return {};
    }

    /// @brief Drive all registered pollables by one step. Returns the
    ///        aggregate frame/packet count.
    std::size_t poll() noexcept {
        // Snapshot: a callback may call `remove()` on itself, which would
        // otherwise invalidate iterators mid-loop.
        auto snapshot = registered_;
        std::size_t total = 0;
        for (auto* p : snapshot) {
            // A pollable may have been removed between snapshot-copy and
            // this iteration; skip if no longer present.
            if (std::find(registered_.begin(), registered_.end(), p)
                == registered_.end()) {
                continue;
            }
            total += p->poll_once_();
        }
        return total;
    }

    /// @brief Optional timeout overload — identical to the bare `poll()`
    ///        for the in-memory driver. Present so `TestPoller` satisfies
    ///        the optional `poll(timeout)` part of the concept contract.
    std::size_t poll(std::chrono::milliseconds /*timeout*/) noexcept {
        return poll();
    }

    /// @brief Current number of registered pollables (test introspection).
    [[nodiscard]] std::size_t size() const noexcept {
        return registered_.size();
    }

    /// @brief Whether `p` is currently registered (test introspection).
    [[nodiscard]] bool contains(P* p) const noexcept {
        return std::find(registered_.begin(), registered_.end(), p)
               != registered_.end();
    }

private:
    std::vector<P*> registered_;
};

} // namespace eph::net::test
