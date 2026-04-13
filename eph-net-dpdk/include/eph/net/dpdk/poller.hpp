#pragma once

/// @file poller.hpp
/// DPDK lcore burst-poll multiplexer satisfying `eph::net::Poller`.
///
/// Design summary:
///
///   - **Heterogeneous Pollables via function-pointer type erasure**
///     (spec "P2"). Each registered object becomes a `PollableEntry`
///     holding `{void* obj, void(*process_burst_fn)(void*, rte_mbuf**,
///     uint16_t, uint64_t), uint32_t conn_id}`. `add<P>(P*)` captures a
///     noexcept thunk into `process_burst_fn`. No std::function, no vtable
///     — pure inlinable function pointer.
///
///   - **No epoll equivalent**: DPDK lcore polling is always non-blocking
///     — `poll()` calls `rte_eth_rx_burst` once, dispatches each mbuf to
///     the matching Pollable based on the 4-tuple / native handle, and
///     returns. There is no `poll(timeout)` overload (the design doc
///     explicitly excludes it).
///
///   - **Routing table**: uses a flat linear scan over the registered
///     entries (like the legacy `Reactor<>` hot-path dispatch loop). For
///     typical HFT deployments with 2-4 connections a cache-friendly
///     linear scan beats a hash map; can be swapped for a real 5-tuple
///     hash if the connection count is large enough to matter.
///
///   - **Pollable notification hooks**: every `Pollable` in the DPDK
///     backend (`DpdkTcpStream`, `DpdkUdpSocket`) exposes
///     `notify_attached_(DpdkPoller*)` / `notify_detached_()` so the
///     `add`/`remove` path can clear / set the stream's `attached_to_`
///     pointer without a cross-class friend declaration.
///
///   - **Not thread-safe**: one Poller owns one lcore.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <span>
#include <vector>

#include <rte_ethdev.h>
#include <rte_mbuf.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "eph/core/error.hpp"
#include "eph/dpdk/packet_parse.hpp"
#include "eph/net/concepts.hpp"
#include "eph/net/dpdk/config.hpp"
#include "eph/utils/time.hpp"

namespace eph::net::dpdk {

namespace detail {

/// @brief Lazily-initialized logger for the DPDK poller subsystem.
inline spdlog::logger* poller_logger() {
    static auto* l = [] {
        auto lg = spdlog::get("net.dpdk.poller");
        if (!lg) {
            try {
                lg = spdlog::stdout_color_mt("net.dpdk.poller");
            } catch (const spdlog::spdlog_ex&) {
                lg = spdlog::get("net.dpdk.poller");
            }
        }
        return lg.get();
    }();
    return l;
}

/// @brief Direction-symmetric FNV-1a hash of a 4-tuple.
///
/// Lifted from the legacy `Reactor` code path. Symmetric so that an
/// incoming packet with swapped src/dst matches the registered tuple
/// without a second hash computation.
[[nodiscard]] inline uint32_t hash_tuple(uint32_t src_ip, uint32_t dst_ip,
                                          uint16_t src_port,
                                          uint16_t dst_port) noexcept {
    uint64_t h = 14695981039346656037ULL;  // FNV-1a 64-bit offset basis
    auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ULL; };
    mix(src_ip ^ dst_ip);
    mix(static_cast<uint64_t>(src_ip) + dst_ip);
    mix(src_port ^ dst_port);
    mix(static_cast<uint64_t>(src_port) + dst_port);
    // Fold to 32 bits — 32 is plenty for the routing-table key space
    // (max kMaxConn registered entries).
    return static_cast<uint32_t>(h ^ (h >> 32));
}

} // namespace detail

// Forward declaration — template instantiation lives in tcp_stream.hpp /
// udp_socket.hpp so their friend decls can name DpdkPoller.
template <class P = void>
class DpdkPoller;

// ---------------------------------------------------------------------------
// DpdkPoller — type-erased specialization (P = void, the default)
// ---------------------------------------------------------------------------

/// @brief Heterogeneous DPDK lcore burst poller.
///
/// The `P = void` specialization is the expected form — it supports
/// registering a mix of `DpdkTcpStream<C,Tls>` and `DpdkUdpSocket<C>` on
/// the same port/queue, which is the dominant HFT layout (one TCP order
/// channel + N UDP market-data feeds all driven by one lcore).
///
/// The generic `P != void` primary template is intentionally minimal —
/// we provide it only so users can be explicit with
/// `DpdkPoller<DpdkTcpStream<...>>` when they are single-Pollable-typed
/// and want stronger type guarantees. Its implementation is identical
/// and simply forwards into the same storage.
template <>
class DpdkPoller<void> {
public:
    /// @brief Maximum registered Pollables. Fixed bound avoids a heap
    ///        allocation on the hot path and keeps the routing table
    ///        friendly for linear scan (typical N ≤ 8 in HFT).
    static constexpr std::size_t kMaxConn = 16;

    /// @brief DPDK burst size — matches the legacy `Reactor::rx_loop`
    ///        choice so microbenchmark results carry over cleanly.
    static constexpr uint16_t kBurstSize = 32;

    // ── Factory ──────────────────────────────────────────────────────────

    [[nodiscard]] static std::expected<std::unique_ptr<DpdkPoller>, core::ErrorInfo>
    create(PollerConfig cfg = {}) noexcept {
        auto* log = detail::poller_logger();
        SPDLOG_LOGGER_DEBUG(log,
            "DpdkPoller::create: port={} queue={} rx_cpu={}",
            cfg.port_id, cfg.rx_queue_id, cfg.rx_cpu);
        auto p = std::unique_ptr<DpdkPoller>(new DpdkPoller(cfg));
        return p;
    }

    ~DpdkPoller() {
        // Tear down in reverse: notify every still-attached Pollable so its
        // `attached_to_` pointer is cleared before we drop state. Otherwise
        // a later ~Stream / ~Socket would call remove() on a dangling Poller.
        for (std::size_t i = 0; i < n_entries_; ++i) {
            if (entries_[i].detach_fn != nullptr) {
                entries_[i].detach_fn(entries_[i].obj);
            }
        }
        n_entries_ = 0;
    }

    DpdkPoller(const DpdkPoller&)            = delete;
    DpdkPoller& operator=(const DpdkPoller&) = delete;
    DpdkPoller(DpdkPoller&&)                 = delete;
    DpdkPoller& operator=(DpdkPoller&&)      = delete;

    // ── Registration ─────────────────────────────────────────────────────

    /// @brief Register `obj` with the Poller. The Pollable's 4-tuple must
    ///        be stable for the lifetime of the registration.
    ///
    /// `add<P>` accepts any concrete type `P` satisfying `eph::net::Pollable`
    /// PLUS the DPDK-backend extension `notify_attached_` /
    /// `notify_detached_` + `connection_tuple_for_poller_()` hooks. Those
    /// extensions live in the `DpdkTcpStream` / `DpdkUdpSocket` headers;
    /// omitting an include at the `add()` call site surfaces a compile
    /// error immediately, no missing-symbol runtime surprises.
    template <class P>
    [[nodiscard]] std::expected<void, core::ErrorInfo> add(P* obj) noexcept {
        auto* log = detail::poller_logger();
        if (obj == nullptr) {
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "DpdkPoller::add: nullptr"});
        }
        if (n_entries_ >= kMaxConn) {
            return std::unexpected(core::ErrorInfo{
                core::Error::OutOfMemory,
                "DpdkPoller::add: entries table full"});
        }
        // Reject duplicates defensively — the linear scan on the hot path
        // would still be correct but routing becomes ambiguous.
        for (std::size_t i = 0; i < n_entries_; ++i) {
            if (entries_[i].obj == static_cast<void*>(obj)) {
                return std::unexpected(core::ErrorInfo{
                    core::Error::InvalidConfig,
                    "DpdkPoller::add: already registered"});
            }
        }

        PollableEntry& entry = entries_[n_entries_];
        entry.obj             = static_cast<void*>(obj);
        entry.process_burst_fn = +[](void* p, rte_mbuf** mbufs, uint16_t n,
                                      uint64_t rx_tsc) noexcept {
            static_cast<P*>(p)->process_burst_(mbufs, n, rx_tsc);
        };
        entry.detach_fn       = +[](void* p) noexcept {
            static_cast<P*>(p)->notify_detached_();
        };
        entry.tuple_fn        = +[](void* p, uint32_t* src_ip, uint32_t* dst_ip,
                                    uint16_t* src_port, uint16_t* dst_port) noexcept {
            static_cast<P*>(p)->tuple_for_poller_(src_ip, dst_ip, src_port, dst_port);
        };
        uint32_t src_ip = 0, dst_ip = 0;
        uint16_t src_port = 0, dst_port = 0;
        entry.tuple_fn(entry.obj, &src_ip, &dst_ip, &src_port, &dst_port);
        entry.src_ip   = src_ip;
        entry.dst_ip   = dst_ip;
        entry.src_port = src_port;
        entry.dst_port = dst_port;
        entry.conn_hash = detail::hash_tuple(src_ip, dst_ip, src_port, dst_port);
        ++n_entries_;

        obj->notify_attached_(this);
        SPDLOG_LOGGER_DEBUG(log,
            "DpdkPoller::add: obj={} tuple_hash=0x{:08x} entries={}",
            static_cast<void*>(obj), entry.conn_hash, n_entries_);
        return {};
    }

    /// @brief Unregister `obj`. Returns `InvalidConfig` if not registered.
    template <class P>
    [[nodiscard]] std::expected<void, core::ErrorInfo> remove(P* obj) noexcept {
        auto* log = detail::poller_logger();
        if (obj == nullptr) {
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "DpdkPoller::remove: nullptr"});
        }
        for (std::size_t i = 0; i < n_entries_; ++i) {
            if (entries_[i].obj != static_cast<void*>(obj)) continue;
            // Shift-left the tail to preserve insertion order; entries_
            // is small (kMaxConn) so the memmove cost is negligible.
            for (std::size_t j = i + 1; j < n_entries_; ++j) {
                entries_[j - 1] = entries_[j];
            }
            --n_entries_;
            entries_[n_entries_] = PollableEntry{};
            obj->notify_detached_();
            SPDLOG_LOGGER_DEBUG(log,
                "DpdkPoller::remove: obj={} entries={}",
                static_cast<void*>(obj), n_entries_);
            return {};
        }
        return std::unexpected(core::ErrorInfo{
            core::Error::InvalidConfig,
            "DpdkPoller::remove: not registered"});
    }

    // ── Poll ─────────────────────────────────────────────────────────────

    /// @brief Non-blocking burst poll — drains up to `kBurstSize` mbufs
    ///        from the NIC, dispatches each to the matching Pollable,
    ///        and returns the number of mbufs processed.
    ///
    /// @note Hot path — must stay noexcept and allocation-free.
    std::size_t poll() noexcept {
        if (n_entries_ == 0) return 0;

        rte_mbuf* mbufs[kBurstSize];
        const uint16_t n = rte_eth_rx_burst(cfg_.port_id, cfg_.rx_queue_id,
                                             mbufs, kBurstSize);
        if (n == 0) return 0;

        const uint64_t rx_tsc = eph::utils::TSC::now();

        std::size_t dispatched = 0;
        for (uint16_t i = 0; i < n; ++i) {
            PollableEntry* entry = lookup_by_5tuple_(mbufs[i]);
            if (entry != nullptr) {
                entry->process_burst_fn(entry->obj, &mbufs[i], 1, rx_tsc);
                ++dispatched;
            } else {
                // No routing match — drop. This mirrors the legacy
                // Reactor behaviour for unmatched packets.
                rte_pktmbuf_free(mbufs[i]);
            }
        }
        return dispatched;
    }

    // ── Introspection (test hooks) ───────────────────────────────────────

    [[nodiscard]] std::size_t size() const noexcept { return n_entries_; }
    [[nodiscard]] uint16_t port_id() const noexcept { return cfg_.port_id; }
    [[nodiscard]] uint16_t rx_queue_id() const noexcept { return cfg_.rx_queue_id; }

private:
    /// @brief Per-registered-Pollable state. Pure POD — kept in a fixed-
    ///        size array so the hot path has no indirection through a
    ///        vector header.
    struct PollableEntry {
        void*    obj               = nullptr;
        void   (*process_burst_fn)(void*, rte_mbuf**, uint16_t, uint64_t) noexcept = nullptr;
        void   (*detach_fn)(void*) noexcept = nullptr;
        void   (*tuple_fn)(void*, uint32_t*, uint32_t*, uint16_t*, uint16_t*) noexcept = nullptr;
        uint32_t conn_hash         = 0;
        uint32_t src_ip            = 0;
        uint32_t dst_ip            = 0;
        uint16_t src_port          = 0;
        uint16_t dst_port          = 0;
    };

    explicit DpdkPoller(PollerConfig cfg) noexcept : cfg_(cfg) {}

    /// @brief Route an incoming mbuf to the matching PollableEntry by
    ///        4-tuple. Handles both TCP and UDP.
    PollableEntry* lookup_by_5tuple_(rte_mbuf* mbuf) noexcept {
        // L2+L3 parse first so we can dispatch by protocol without doing
        // the TCP or UDP parse twice in the collision / miss path.
        auto ip_hdr = eph::dpdk::net::parse_ip_header(mbuf);
        if (!ip_hdr) return nullptr;

        uint32_t pkt_src_ip = 0, pkt_dst_ip = 0;
        uint16_t pkt_src_port = 0, pkt_dst_port = 0;

        if (ip_hdr.proto == eph::dpdk::net::kIpProtoTcp) {
            auto parsed = eph::dpdk::net::parse_packet(mbuf);
            if (!parsed.tcp) return nullptr;
            pkt_src_ip   = parsed.src_ip();
            pkt_dst_ip   = parsed.dst_ip();
            pkt_src_port = parsed.src_port();
            pkt_dst_port = parsed.dst_port();
        } else if (ip_hdr.proto == eph::dpdk::net::kIpProtoUdp) {
            auto parsed = eph::dpdk::net::parse_udp_from_ip(mbuf, ip_hdr);
            if (!parsed.udp) return nullptr;
            pkt_src_ip   = parsed.src_ip();
            pkt_dst_ip   = parsed.dst_ip();
            pkt_src_port = parsed.src_port();
            pkt_dst_port = parsed.dst_port();
        } else {
            return nullptr;
        }

        // Incoming packets carry swapped src/dst relative to the registered
        // tuple (the registered tuple is the *local view* — "this is my
        // local 4-tuple"). The hash function is direction-symmetric so one
        // hash fits both directions; the equality check afterwards
        // verifies the swap.
        const uint32_t pkt_hash = detail::hash_tuple(pkt_src_ip, pkt_dst_ip,
                                                      pkt_src_port, pkt_dst_port);
        for (std::size_t i = 0; i < n_entries_; ++i) {
            auto& e = entries_[i];
            if (e.conn_hash != pkt_hash) continue;
            // Full tuple compare with swap — reject hash collisions.
            if (pkt_src_ip   == e.dst_ip   && pkt_dst_ip   == e.src_ip   &&
                pkt_src_port == e.dst_port && pkt_dst_port == e.src_port) {
                return &e;
            }
        }
        return nullptr;
    }

    PollerConfig                       cfg_{};
    std::array<PollableEntry, kMaxConn> entries_{};
    std::size_t                         n_entries_{0};
};

// ---------------------------------------------------------------------------
// Primary template: DpdkPoller<P> for users who want single-type stronger
// guarantees. It forwards into the same engine via the void specialization.
// ---------------------------------------------------------------------------

template <class P>
class DpdkPoller {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<DpdkPoller>, core::ErrorInfo>
    create(PollerConfig cfg = {}) noexcept {
        auto impl = DpdkPoller<void>::create(cfg);
        if (!impl) return std::unexpected(impl.error());
        return std::unique_ptr<DpdkPoller>(new DpdkPoller(std::move(*impl)));
    }

    [[nodiscard]] std::expected<void, core::ErrorInfo> add(P* obj) noexcept {
        return impl_->template add<P>(obj);
    }
    [[nodiscard]] std::expected<void, core::ErrorInfo> remove(P* obj) noexcept {
        return impl_->template remove<P>(obj);
    }
    std::size_t poll() noexcept { return impl_->poll(); }

    [[nodiscard]] std::size_t size() const noexcept { return impl_->size(); }
    [[nodiscard]] uint16_t port_id() const noexcept { return impl_->port_id(); }
    [[nodiscard]] uint16_t rx_queue_id() const noexcept { return impl_->rx_queue_id(); }

private:
    explicit DpdkPoller(std::unique_ptr<DpdkPoller<void>> impl) noexcept
        : impl_(std::move(impl)) {}
    std::unique_ptr<DpdkPoller<void>> impl_;
};

// ---------------------------------------------------------------------------
// Concept conformance
// ---------------------------------------------------------------------------

static_assert(eph::net::Poller<DpdkPoller<>>,
              "DpdkPoller<> must satisfy eph::net::Poller");

} // namespace eph::net::dpdk
