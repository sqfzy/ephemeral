#pragma once

/// @file icmp_registry.hpp
/// ICMP Frag Needed target registry — maps {embedded 4-tuple + IP
/// protocol} to (stream, callback) pairs. Used by `eph::dpdk::Platform`
/// to route router-originated ICMP messages back to the stream that
/// caused them.
///
/// ## Ownership model — shared_ptr (Major 2 root fix)
///
/// `IcmpRegistry` must be heap-allocated and reference-counted via
/// `std::shared_ptr`. Two reasons:
///
///   1. **Shared lifetime** between `Platform` (strong ref in Impl)
///      and `DpdkPoller`'s ICMP callback closure (strong ref captured
///      by value in the std::function). Platform destruction can
///      predate Poller destruction without UAF — registry lives as
///      long as either holder is alive.
///
///   2. **weak_ptr Handle** — each `IcmpRegistry::Handle` (held by a
///      Stream's `icmp_reg_`) stores a `weak_ptr<IcmpRegistry>`. If
///      the registry has already died when the handle destructs,
///      `.lock()` returns empty and the unregister is safely skipped.
///
/// The class inherits `enable_shared_from_this` so `register_target`
/// can hand a `weak_ptr` to each `Handle`. Constructing an
/// `IcmpRegistry` outside a `std::shared_ptr` is supported but
/// degraded: `weak_from_this()` returns an empty `weak_ptr` (well-
/// defined since C++17 — non-throwing, not UB), so the Handle's
/// destructor finds an expired registry and skips unregister. The
/// slot in `targets_` is never reclaimed in that case, but no UAF
/// occurs because the standalone registry has no other strong refs
/// and dies with its scope. Production code should still use
/// `std::make_shared<IcmpRegistry>()` so unregister actually runs.
///
/// ## Thread safety
///
/// All state mutation (register / unregister / dispatch / counters)
/// happens under `mu_`. Production HFT topologies have:
///
///   - Control thread (stream create/destroy) → register / unregister
///   - LCore thread (poll loop) → dispatch
///
/// Without the lock, hot reconnect (stream destroy racing with ICMP
/// dispatch) would corrupt `targets_`. The lock is uncontended in
/// steady state — register/unregister are startup/teardown events,
/// dispatch fires << 1 Hz (only when routers emit ICMP Frag Needed).
/// Hot path (poll cycles without ICMP) never touches the registry.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <string>

#include "eph/core/error.hpp"
#include "eph/dpdk/packet_core.hpp"    // ConnectionTuple
#include "eph/dpdk/packet_parse.hpp"   // ParsedIcmp

namespace eph::dpdk::detail {

class IcmpRegistry : public std::enable_shared_from_this<IcmpRegistry> {
public:
    /// @brief Callback invoked on a dispatch hit.
    /// @param stream  User-opaque pointer the caller passed at
    ///                register time (typically `DpdkTcpStream*`).
    /// @param mtu     Next-hop MTU from the ICMP Type 3 Code 4 message.
    using MtuCallback = void(*)(void* stream, uint16_t mtu) noexcept;

    /// Linear-scan cap. 64 covers every realistic HFT topology
    /// (one or two lcores × a few streams each). Bumping requires
    /// no ABI change; the scan is only on ICMP (a cold path).
    static constexpr std::size_t kMaxTargets = 64;

    // ── RAII Handle ──────────────────────────────────────────────────────
    //
    // Holds a `weak_ptr<IcmpRegistry>` — destructor tries `.lock()` and
    // unregisters only if the registry is still alive. Move-only.
    // Three distinguishable states:
    //   * default-constructed: engaged_=false, reg_weak_ empty → dtor no-op
    //   * active:              engaged_=true,  reg_weak_ targets live registry
    //   * moved-from:          engaged_=false (source of move)

    class Handle {
    public:
        Handle() noexcept = default;

        ~Handle() noexcept {
            release_();
        }

        Handle(const Handle&)            = delete;
        Handle& operator=(const Handle&) = delete;

        Handle(Handle&& o) noexcept
            : reg_weak_(std::move(o.reg_weak_)),
              tuple_(o.tuple_),
              proto_(o.proto_),
              engaged_(o.engaged_) {
            o.engaged_ = false;
        }
        Handle& operator=(Handle&& o) noexcept {
            if (this != &o) {
                release_();
                reg_weak_ = std::move(o.reg_weak_);
                tuple_    = o.tuple_;
                proto_    = o.proto_;
                engaged_  = o.engaged_;
                o.engaged_ = false;
            }
            return *this;
        }

        [[nodiscard]] bool engaged() const noexcept { return engaged_; }

    private:
        friend class IcmpRegistry;

        Handle(std::weak_ptr<IcmpRegistry> reg,
               ::eph::dpdk::net::ConnectionTuple tuple,
               uint8_t proto) noexcept
            : reg_weak_(std::move(reg)),
              tuple_(tuple),
              proto_(proto),
              engaged_(true) {}

        /// Unregister from the registry if it's still alive. Safe to call
        /// on moved-from or default-constructed handles (no-op).
        void release_() noexcept {
            if (!engaged_) return;
            // weak_ptr::lock() is thread-safe and returns an empty
            // shared_ptr if the last strong ref has dropped. Holding
            // the temporary shared_ptr for the duration of the
            // unregister() call keeps the registry alive across the
            // call, even if another thread is concurrently dropping
            // the last external reference.
            if (auto reg = reg_weak_.lock()) {
                reg->unregister(tuple_, proto_);
            }
            engaged_ = false;
        }

        std::weak_ptr<IcmpRegistry>       reg_weak_;
        ::eph::dpdk::net::ConnectionTuple tuple_{};
        uint8_t                           proto_{0};
        bool                              engaged_{false};
    };

    // ── Public API ───────────────────────────────────────────────────────

    /// @brief Register a target. Duplicate (tuple, proto) are rejected
    ///        — caller has a bookkeeping bug, not a retryable error.
    /// @return An RAII Handle on success; an ErrorInfo on
    ///         InvalidConfig (null cb/stream or duplicate) or
    ///         OutOfMemory (registry full).
    /// @note Thread-safe under `mu_`.
    /// @note The returned Handle holds a weak_ptr derived from
    ///       `weak_from_this()`. On an `IcmpRegistry` not managed
    ///       by `shared_ptr` the weak_ptr is empty (C++17 contract,
    ///       not UB) and Handle::dtor's unregister call is silently
    ///       skipped — see the file-level comment for the slot-leak
    ///       trade-off.
    [[nodiscard]] std::expected<Handle, ::eph::core::ErrorInfo>
    register_target(::eph::dpdk::net::ConnectionTuple tuple,
                    uint8_t     proto,
                    void*       stream,
                    MtuCallback cb) noexcept {
        if (stream == nullptr || cb == nullptr) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig,
                "IcmpRegistry::register_target: stream/cb must not be null"});
        }
        std::lock_guard<std::mutex> g(mu_);
        if (n_targets_ >= kMaxTargets) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::OutOfMemory,
                "IcmpRegistry::register_target: registry full"});
        }
        for (std::size_t i = 0; i < n_targets_; ++i) {
            if (entry_matches_(targets_[i], tuple, proto)) {
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::InvalidConfig,
                    "IcmpRegistry::register_target: tuple already registered"});
            }
        }
        auto& slot = targets_[n_targets_++];
        slot.tuple  = tuple;
        slot.proto  = proto;
        slot.stream = stream;
        slot.cb     = cb;
        return Handle{weak_from_this(), tuple, proto};
    }

    /// @brief Unregister a (tuple, proto) pair. No-op if not found.
    ///        Uses swap-with-last removal to keep the array compact.
    /// @note Thread-safe under `mu_`.
    void unregister(const ::eph::dpdk::net::ConnectionTuple& tuple,
                    uint8_t proto) noexcept {
        std::lock_guard<std::mutex> g(mu_);
        for (std::size_t i = 0; i < n_targets_; ++i) {
            if (entry_matches_(targets_[i], tuple, proto)) {
                targets_[i] = targets_[n_targets_ - 1];
                targets_[--n_targets_] = Entry{};
                return;
            }
        }
    }

    /// @brief Walk the registry looking for an entry matching the
    ///        embedded 4-tuple + protocol in `parsed`. On match,
    ///        invoke its callback with `parsed.next_hop_mtu` and bump
    ///        the `dispatched()` counter. Silently no-op on no match
    ///        or on `embedded_valid == false`.
    /// @note The **match-and-copy** happens under `mu_`, but the
    ///       callback itself is invoked **after releasing the lock**.
    ///       This lets callbacks safely call back into the registry
    ///       (e.g. `unregister` from a different thread's Handle
    ///       destructor, or a future self-unregistering target)
    ///       without recursive-lock deadlock.
    void dispatch(const ::eph::dpdk::net::ParsedIcmp& parsed) noexcept {
        if (!parsed.embedded_valid) return;

        // Match-and-copy phase: scan under the lock and snapshot the
        // (stream, cb) pair into stack locals so we can release the
        // lock before invoking. dispatched_ is bumped here (before
        // the call) so the counter and the decision to invoke are
        // atomic with respect to other threads' scans — a concurrent
        // unregister will remove the entry we just snapshotted, but
        // the counter correctly records that the dispatch-to-that-
        // entry happened.
        void*       stream = nullptr;
        MtuCallback cb     = nullptr;
        {
            std::lock_guard<std::mutex> g(mu_);
            for (std::size_t i = 0; i < n_targets_; ++i) {
                const auto& e = targets_[i];
                if (e.proto            == parsed.embedded_proto   &&
                    e.tuple.src_ip     == parsed.embedded_src_ip  &&
                    e.tuple.dst_ip     == parsed.embedded_dst_ip  &&
                    e.tuple.src_port   == parsed.embedded_src_port &&
                    e.tuple.dst_port   == parsed.embedded_dst_port) {
                    stream = e.stream;
                    cb     = e.cb;
                    ++dispatched_;
                    break;
                }
            }
        }
        // Invoke out-of-lock — callback is free to mutate the
        // registry (or anything else) without deadlock risk.
        if (cb) cb(stream, parsed.next_hop_mtu);
    }

    /// @brief Cumulative count of successful dispatches.
    /// @note Thread-safe; takes `mu_` for a consistent snapshot.
    [[nodiscard]] uint64_t dispatched() const noexcept {
        std::lock_guard<std::mutex> g(mu_);
        return dispatched_;
    }

    /// @brief Current number of registered targets (0..kMaxTargets).
    /// @note Thread-safe; takes `mu_` for a consistent snapshot.
    [[nodiscard]] std::size_t size() const noexcept {
        std::lock_guard<std::mutex> g(mu_);
        return n_targets_;
    }

private:
    struct Entry {
        ::eph::dpdk::net::ConnectionTuple tuple{};
        uint8_t     proto{0};
        void*       stream{nullptr};
        MtuCallback cb{nullptr};
    };

    [[nodiscard]] static bool entry_matches_(
        const Entry& e,
        const ::eph::dpdk::net::ConnectionTuple& tuple,
        uint8_t proto) noexcept {
        return e.proto            == proto         &&
               e.tuple.src_ip     == tuple.src_ip   &&
               e.tuple.dst_ip     == tuple.dst_ip   &&
               e.tuple.src_port   == tuple.src_port &&
               e.tuple.dst_port   == tuple.dst_port;
    }

    mutable std::mutex             mu_;
    std::array<Entry, kMaxTargets> targets_{};
    std::size_t                    n_targets_{0};
    uint64_t                       dispatched_{0};
};

} // namespace eph::dpdk::detail
