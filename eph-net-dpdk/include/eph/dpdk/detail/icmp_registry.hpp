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

#include <spdlog/spdlog.h>

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

    /// Linear-scan cap. 256 covers high-fanout HFT topologies
    /// (e.g. multi-lcore market-data fan-outs and the bench's 100-conn
    /// rss_scaling_ws sweep). Bumping requires no ABI change; the scan
    /// is only on ICMP (a cold path) and at 256 entries the linear
    /// probe is still ~1 µs even on Graviton. Memory cost: 256 × ~48 B
    /// Entry ≈ 14 KiB per IcmpRegistry instance — negligible relative
    /// to a Platform's lifetime. Original cap was 64; bumped when the
    /// rss_scaling_ws sweep at conn_count=100 exhausted the registry.
    static constexpr std::size_t kMaxTargets = 256;

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
            SPDLOG_ERROR(
                "IcmpRegistry::register_target: stream={} cb={} must not be null "
                "(proto={} src=0x{:08x}:{} dst=0x{:08x}:{})",
                stream, reinterpret_cast<void*>(cb), proto,
                tuple.src_ip, tuple.src_port, tuple.dst_ip, tuple.dst_port);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig,
                "IcmpRegistry::register_target: stream/cb must not be null"});
        }
        std::lock_guard<std::mutex> g(mu_);
        if (n_targets_ >= kMaxTargets) {
            SPDLOG_ERROR(
                "IcmpRegistry::register_target: registry full ({} entries) "
                "(proto={} src=0x{:08x}:{} dst=0x{:08x}:{})",
                kMaxTargets, proto, tuple.src_ip, tuple.src_port,
                tuple.dst_ip, tuple.dst_port);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::OutOfMemory,
                "IcmpRegistry::register_target: registry full"});
        }
        for (std::size_t i = 0; i < n_targets_; ++i) {
            if (entry_matches_(targets_[i], tuple, proto)) {
                SPDLOG_WARN(
                    "IcmpRegistry::register_target: tuple already registered "
                    "at index {} (proto={} src=0x{:08x}:{} dst=0x{:08x}:{})",
                    i, proto, tuple.src_ip, tuple.src_port,
                    tuple.dst_ip, tuple.dst_port);
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
    /// @note The matching scan AND the callback itself run under
    ///       `mu_` to close a UAF window. Concrete scenario the lock
    ///       hold-through prevents:
    ///         1. lcore enters `dispatch`, snapshots `(stream, cb)`.
    ///         2. lcore releases lock (the prior shape).
    ///         3. app thread's `~Stream` runs `~Handle` → `unregister`,
    ///            acquires lock, removes slot, returns. App proceeds to
    ///            destroy `sess_` / `codec_` / the entire stream object.
    ///         4. lcore invokes `cb(stream, mtu)` on freed memory.
    ///       Holding the lock through the callback makes step 3 block
    ///       on step 4 — `~Stream` cannot proceed until the callback
    ///       finishes touching the stream.
    /// @warning Callback contract: the callback **must not** call back
    ///       into the registry (`register_target` / `unregister`) on
    ///       this thread — `mu_` is non-recursive and recursion
    ///       deadlocks. The stock production callback
    ///       (`DpdkTcpStream::on_icmp_mtu_thunk_`) only forwards into
    ///       `TcpSession::on_icmp_frag_needed`, which is registry-free
    ///       and trivially conforms.
    /// @note Thin wrapper over `dispatch_returns_hit` — kept as a
    ///       distinct entry point so callers that don't need the hit
    ///       boolean (the in-process `Platform::on_icmp_callback_`
    ///       dispatch closure, the legacy single-process tests) read
    ///       a clearer call site. Forwarder shape avoids the previous
    ///       100% body-duplication that drifted the two paths' fix
    ///       history (see icmp_registry pre-2026-05-03 history).
    void dispatch(const ::eph::dpdk::net::ParsedIcmp& parsed) noexcept {
        (void)dispatch_returns_hit(parsed);
    }

    /// @brief Same dispatch logic as `dispatch()`, but returns whether
    ///        the message hit a registered target. Added for the
    ///        cross-process ICMP path: if a Frag Needed lands on a
    ///        secondary's RX queue but no local target matches, the
    ///        Poller closure needs to know "miss" so it can forward
    ///        the message to the owner process via IPC. Returning a
    ///        bool from the existing `dispatch()` would change its
    ///        signature and break the reshape's "IcmpRegistry public
    ///        API unchanged" invariant — this is a strict superset,
    ///        and `dispatch()` now thin-forwards to this overload.
    /// @return true iff some entry matched (and its callback was
    ///         invoked); false otherwise (no-op including the
    ///         `embedded_valid==false` short-circuit).
    /// @note Thread-safe under `mu_`. Same UAF-safe callback-under-
    ///       lock semantics as documented on `dispatch()`.
    [[nodiscard]] bool
    dispatch_returns_hit(const ::eph::dpdk::net::ParsedIcmp& parsed) noexcept {
        if (!parsed.embedded_valid) return false;

        std::lock_guard<std::mutex> g(mu_);
        for (std::size_t i = 0; i < n_targets_; ++i) {
            const auto& e = targets_[i];
            if (e.proto            == parsed.embedded_proto   &&
                e.tuple.src_ip     == parsed.embedded_src_ip  &&
                e.tuple.dst_ip     == parsed.embedded_dst_ip  &&
                e.tuple.src_port   == parsed.embedded_src_port &&
                e.tuple.dst_port   == parsed.embedded_dst_port) {
                ++dispatched_;
                // Invoke under the lock so a concurrent unregister
                // cannot free the stream out from under us. See the
                // @note on `dispatch()`.
                if (e.cb) e.cb(e.stream, parsed.next_hop_mtu);
                return true;
            }
        }
        return false;
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

    // ── Compile-time invariants ──────────────────────────────────────────
    //
    // kMaxTargets == 0 would silently turn `register_target` into "always
    // returns OutOfMemory" and `dispatch` into a no-op; both are
    // surprising failure modes that can lurk through unit tests. A
    // future tuner shrinking kMaxTargets to free a few KiB has to
    // re-affirm the lower bound here.
    static_assert(kMaxTargets >= 1,
                  "IcmpRegistry::kMaxTargets must allow at least one slot");
    // Linear scans on register/dispatch are O(n_targets_); 4096 is a
    // soft ceiling that keeps the worst-case dispatch latency under a
    // few µs even on Graviton (~1 ns/entry). Hard fail at compile time
    // if someone bumps the cap past this without a perf re-evaluation.
    static_assert(kMaxTargets <= 4096,
                  "IcmpRegistry::kMaxTargets > 4096 risks µs-class "
                  "dispatch latency; re-benchmark before raising");
};

} // namespace eph::dpdk::detail
