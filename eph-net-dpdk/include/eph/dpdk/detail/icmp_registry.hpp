#pragma once

/// @file icmp_registry.hpp
/// Standalone ICMP Frag Needed target registry — a small heap-free,
/// EAL-free store that maps {embedded 4-tuple + IP protocol} to
/// (stream, callback) pairs. Used by `eph::dpdk::Platform` to route
/// router-originated ICMP messages back to the stream that caused
/// them.
///
/// Factored out of `Platform::Impl` so that the registry's state-
/// machine (register/unregister/dispatch, linear-scan matching,
/// swap-with-last compaction, RAII Handle lifecycle) can be unit-
/// tested without spinning up EAL + NIC — i.e. on CI / laptop / any
/// dev machine.
///
/// Thread-safety: not thread-safe. Register/unregister are expected
/// on the stream-construction thread; `dispatch()` runs on the lcore
/// poll thread. In typical HFT topologies the two are separated by
/// startup ordering (streams attach before poll loop starts), so no
/// overlap occurs. A future multi-threaded use case would need
/// external synchronisation or a mutex here.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

#include "eph/core/error.hpp"
#include "eph/dpdk/packet_core.hpp"    // ConnectionTuple
#include "eph/dpdk/packet_parse.hpp"   // ParsedIcmp

namespace eph::dpdk::detail {

/// @brief ICMP-to-stream registry. Owns an inline fixed-capacity
///        array of targets (no heap). Returns RAII `Handle`s on
///        register; the handle's destructor (or move-overwrite)
///        unregisters.
class IcmpRegistry {
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
    // Move-only. Empty-constructed, default-moved-from, and alive
    // variants all distinguishable via `engaged_`. Destructor calls
    // `reg_->unregister(tuple_, proto_)` only when engaged_ && reg_
    // — both guards matter: moved-from has engaged_=false, default-
    // constructed has reg_=nullptr.

    class Handle {
    public:
        Handle() noexcept = default;
        Handle(IcmpRegistry* reg,
               ::eph::dpdk::net::ConnectionTuple tuple,
               uint8_t proto) noexcept
            : reg_(reg), tuple_(tuple), proto_(proto), engaged_(true) {}

        ~Handle() noexcept {
            if (engaged_ && reg_ != nullptr) {
                reg_->unregister(tuple_, proto_);
            }
        }

        Handle(const Handle&)            = delete;
        Handle& operator=(const Handle&) = delete;

        Handle(Handle&& o) noexcept
            : reg_(o.reg_), tuple_(o.tuple_),
              proto_(o.proto_), engaged_(o.engaged_) {
            o.engaged_ = false;
        }
        Handle& operator=(Handle&& o) noexcept {
            if (this != &o) {
                if (engaged_ && reg_ != nullptr) {
                    reg_->unregister(tuple_, proto_);
                }
                reg_     = o.reg_;
                tuple_   = o.tuple_;
                proto_   = o.proto_;
                engaged_ = o.engaged_;
                o.engaged_ = false;
            }
            return *this;
        }

        [[nodiscard]] bool engaged() const noexcept { return engaged_; }

    private:
        IcmpRegistry*                     reg_{nullptr};
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
        if (n_targets_ >= kMaxTargets) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::OutOfMemory,
                "IcmpRegistry::register_target: registry full"});
        }
        for (std::size_t i = 0; i < n_targets_; ++i) {
            const auto& e = targets_[i];
            if (entry_matches_(e, tuple, proto)) {
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
        return Handle{this, tuple, proto};
    }

    /// @brief Unregister a (tuple, proto) pair. No-op if not found.
    ///        Uses swap-with-last removal to keep the array compact.
    void unregister(const ::eph::dpdk::net::ConnectionTuple& tuple,
                    uint8_t proto) noexcept {
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
    ///        the `dispatched()` counter. Silently no-op on no match.
    ///
    /// Safe to call with any `ParsedIcmp`; requires
    /// `parsed.embedded_valid == true` to attempt dispatch.
    void dispatch(const ::eph::dpdk::net::ParsedIcmp& parsed) noexcept {
        if (!parsed.embedded_valid) return;
        for (std::size_t i = 0; i < n_targets_; ++i) {
            const auto& e = targets_[i];
            if (e.proto            == parsed.embedded_proto   &&
                e.tuple.src_ip     == parsed.embedded_src_ip  &&
                e.tuple.dst_ip     == parsed.embedded_dst_ip  &&
                e.tuple.src_port   == parsed.embedded_src_port &&
                e.tuple.dst_port   == parsed.embedded_dst_port) {
                e.cb(e.stream, parsed.next_hop_mtu);
                ++dispatched_;
                return;
            }
        }
    }

    /// @brief Cumulative count of successful dispatches.
    [[nodiscard]] uint64_t dispatched() const noexcept { return dispatched_; }

    /// @brief Current number of registered targets (0..kMaxTargets).
    [[nodiscard]] std::size_t size() const noexcept { return n_targets_; }

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

    std::array<Entry, kMaxTargets> targets_{};
    std::size_t                    n_targets_{0};
    uint64_t                       dispatched_{0};
};

} // namespace eph::dpdk::detail
