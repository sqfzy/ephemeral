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
///     the matching Pollable based on the 5-tuple / native handle, and
///     returns. There is no `poll(timeout)` overload (the design doc
///     explicitly excludes it).
///
///   - **Routing table**: uses a flat linear scan over the registered
///     entries. For typical HFT deployments with 2-4 connections a
///     cache-friendly linear scan beats a hash map; can be swapped for
///     a real 5-tuple hash if the connection count is large enough to matter.
///
///   - **Pollable notification hooks**: every `Pollable` in the DPDK
///     backend (`DpdkTcpStream`, `DpdkUdpSocket`) exposes
///     `notify_attached_(DpdkPoller*)` / `notify_detached_()` so the
///     `add`/`remove` path can clear / set the stream's `attached_to_`
///     pointer without a cross-class friend declaration.
///
///   - **Not thread-safe**: one Poller owns one lcore.

#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>

#include <sys/random.h>   // getrandom(2) — random start for pick_src_port

#include <rte_ethdev.h>
#include <rte_mbuf.h>

#include <spdlog/spdlog.h>

#include "eph/core/detail/logger.hpp"
#include "eph/core/error.hpp"
#include "eph/dpdk/packet_parse.hpp"
#include "eph/net/concepts.hpp"
#include "eph/net/dpdk/config.hpp"
#include "eph/utils/time.hpp"

namespace eph::net::dpdk {

namespace detail {

/// @brief Lazily-initialized logger for the DPDK poller subsystem.
inline spdlog::logger* poller_logger() {
    static auto* l = ::eph::core::detail::make_logger("net.dpdk.poller");
    return l;
}

/// @brief Direction-symmetric FNV-1a hash of a 5-tuple.
///
/// Symmetric so that an incoming packet with swapped src/dst matches the
/// registered tuple without a second hash computation. Protocol is mixed
/// as an additional field so that a TCP and a UDP Pollable sharing the
/// same (src_ip,
/// dst_ip, src_port, dst_port) land in distinct hash buckets — required
/// to prevent cross-protocol misrouting when NIC flow steering delivers
/// both protocols to the same poll queue.
[[nodiscard]] constexpr uint32_t hash_tuple(uint32_t src_ip, uint32_t dst_ip,
                                              uint16_t src_port,
                                              uint16_t dst_port,
                                              uint8_t  proto) noexcept {
    uint64_t h = 14695981039346656037ULL;  // FNV-1a 64-bit offset basis
    auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ULL; };
    mix(src_ip ^ dst_ip);
    mix(static_cast<uint64_t>(src_ip) + dst_ip);
    mix(src_port ^ dst_port);
    mix(static_cast<uint64_t>(src_port) + dst_port);
    mix(static_cast<uint64_t>(proto));
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
// DpdkPollable — compile-time contract for types registered with DpdkPoller.
// ---------------------------------------------------------------------------

/// @brief Concept for types that can be registered with `DpdkPoller::add()`.
///
/// Every DPDK-backend Pollable (`DpdkTcpStream`, `DpdkUdpSocket`) must
/// satisfy this concept. Declaring it here moves type errors from deep
/// inside the `add()` lambda captures to the `add()` call site.
template <class P>
concept DpdkPollable = requires(P& p, rte_mbuf** mbufs, uint16_t n,
                                uint64_t tsc, DpdkPoller<void>* poller,
                                uint32_t* ip, uint16_t* port, uint8_t* proto) {
    { p.process_burst_(mbufs, n, tsc) } noexcept;
    { p.notify_attached_(poller) } noexcept;
    { p.notify_detached_() } noexcept;
    { p.tuple_for_poller_(ip, ip, port, port, proto) } noexcept;
    { p.on_poll_tick_(tsc) } noexcept;   // periodic per-cycle work (keepalive, timers)
};

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

    /// @brief DPDK burst size — matches the canonical 32-mbuf burst used
    ///        across the codebase (TcpSession::poll_rx, microbenchmarks).
    static constexpr uint16_t kBurstSize = 32;

    // ── Factory ──────────────────────────────────────────────────────────

    [[nodiscard]] static std::expected<std::unique_ptr<DpdkPoller>, core::ErrorInfo>
    create(PollerConfig cfg = {}) noexcept {
        [[maybe_unused]] auto* log = detail::poller_logger();
        SPDLOG_LOGGER_DEBUG(log,
            "DpdkPoller::create: port={} queue={}",
            cfg.port_id, cfg.rx_queue_id);
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

    /// @brief Register `obj` with the Poller. The Pollable's 5-tuple must
    ///        be stable for the lifetime of the registration.
    ///
    /// `add<P>` accepts any concrete type `P` satisfying `eph::net::Pollable`
    /// PLUS the DPDK-backend extension `notify_attached_` /
    /// `notify_detached_` + `connection_tuple_for_poller_()` hooks. Those
    /// extensions live in the `DpdkTcpStream` / `DpdkUdpSocket` headers;
    /// omitting an include at the `add()` call site surfaces a compile
    /// error immediately, no missing-symbol runtime surprises.
    template <DpdkPollable P>
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
        // Retrieve the incoming 5-tuple up front so the duplicate scan can
        // check both obj-pointer duplicates AND 5-tuple duplicates in the
        // same linear pass. Reject either: obj-duplicate makes no sense
        // and 5-tuple duplicate makes routing ambiguous (silent data
        // corruption on the hot path, which is catastrophic in HFT).
        //
        // Note: TCP and UDP Pollables sharing the exact same (src_ip,
        // dst_ip, src_port, dst_port) are legitimate — they live in
        // independent L4 namespaces. Protocol is part of the key so both
        // can coexist without ambiguity.
        uint32_t new_src_ip = 0, new_dst_ip = 0;
        uint16_t new_src_port = 0, new_dst_port = 0;
        uint8_t  new_proto = 0;
        obj->tuple_for_poller_(&new_src_ip, &new_dst_ip,
                                &new_src_port, &new_dst_port,
                                &new_proto);
        const uint32_t new_hash = detail::hash_tuple(
            new_src_ip, new_dst_ip, new_src_port, new_dst_port, new_proto);

        for (std::size_t i = 0; i < n_entries_; ++i) {
            const auto& e = entries_[i];
            if (e.obj == static_cast<void*>(obj)) {
                return std::unexpected(core::ErrorInfo{
                    core::Error::InvalidConfig,
                    "DpdkPoller::add: already registered"});
            }
            // Fast filter on hash; full 5-field compare only on hash hit.
            if (e.conn_hash == new_hash &&
                e.src_ip    == new_src_ip &&
                e.dst_ip    == new_dst_ip &&
                e.src_port  == new_src_port &&
                e.dst_port  == new_dst_port &&
                e.proto     == new_proto) {
                SPDLOG_LOGGER_WARN(log,
                    "DpdkPoller::add: 5-tuple already registered "
                    "proto={} src=0x{:08x}:{} dst=0x{:08x}:{}",
                    new_proto, new_src_ip, new_src_port,
                    new_dst_ip, new_dst_port);
                return std::unexpected(core::ErrorInfo{
                    core::Error::InvalidConfig,
                    "DpdkPoller::add: 5-tuple already registered"});
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
                                    uint16_t* src_port, uint16_t* dst_port,
                                    uint8_t* proto) noexcept {
            static_cast<P*>(p)->tuple_for_poller_(src_ip, dst_ip, src_port, dst_port, proto);
        };
        entry.tick_fn         = +[](void* p, uint64_t tsc) noexcept {
            static_cast<P*>(p)->on_poll_tick_(tsc);
        };
        // Reuse the tuple retrieved above — no second call to tuple_for_poller_.
        entry.src_ip    = new_src_ip;
        entry.dst_ip    = new_dst_ip;
        entry.src_port  = new_src_port;
        entry.dst_port  = new_dst_port;
        entry.proto     = new_proto;
        entry.conn_hash = new_hash;
        ++n_entries_;

        obj->notify_attached_(this);
        SPDLOG_LOGGER_DEBUG(log,
            "DpdkPoller::add: obj={} tuple_hash=0x{:08x} entries={}",
            static_cast<void*>(obj), entry.conn_hash, n_entries_);
        return {};
    }

    /// @brief Unregister `obj`. Returns `InvalidConfig` if not registered.
    template <DpdkPollable P>
    [[nodiscard]] std::expected<void, core::ErrorInfo> remove(P* obj) noexcept {
        [[maybe_unused]] auto* log = detail::poller_logger();
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
        // Always drain the NIC ring, even when no streams are currently
        // attached. A registered-but-empty Poller can still be the
        // destination of unsolicited ICMP (Type 3 Code 4), late-arriving
        // mbufs from a just-detached stream, or other out-of-band traffic
        // — leaving them in the ring causes tail-drops that bleed into
        // unrelated queues on NICs with shared buffers.
        rte_mbuf* mbufs[kBurstSize];
        const uint16_t n = rte_eth_rx_burst(cfg_.port_id, cfg_.rx_queue_id,
                                             mbufs, kBurstSize);

        // Single TSC read per cycle — used both as the rx_tsc passed
        // down to process_burst_fn and as the tick_tsc for on_poll_tick_.
        // Keepalive needs only ms-level precision; the ~µs drift between
        // "just after rx_burst" and "end of dispatch" is irrelevant.
        const uint64_t cycle_tsc = eph::utils::TSC::now();

        std::size_t dispatched = 0;
        for (uint16_t i = 0; i < n; ++i) {
            PollableEntry* entry = lookup_by_5tuple_(mbufs[i]);
            if (entry != nullptr) {
                entry->process_burst_fn(entry->obj, &mbufs[i], 1, cycle_tsc);
                ++dispatched;
            } else {
                // No routing match. Before falling back to drop, check
                // whether the mbuf is an ICMP Frag Needed (Type 3
                // Code 4) message targeting one of the registered
                // streams. Non-ICMP unmatched packets in DPDK PMD
                // mode have no kernel stack to fall back to; freeing
                // the mbuf is the only sane disposal.
                maybe_dispatch_icmp_(mbufs[i]);
                rte_pktmbuf_free(mbufs[i]);
            }
        }

        // Periodic per-cycle tick — runs regardless of whether the burst
        // delivered any mbufs. Idle connections with
        // `keepalive_interval > 0` rely on this to fire probes even
        // when RX is silent. The tick thunks are inlinable function
        // pointers; UDP implements `on_poll_tick_` as a no-op which
        // GCC14 compiles out entirely.
        //
        // Reverse iteration: defensive against a tick_fn that somehow
        // triggers `Poller::remove` for some entry (none currently do,
        // but a future extension might). Removal shifts the tail left
        // into the vacated slot; walking backwards means such a shift
        // cannot cause us to skip a not-yet-ticked entry.
        for (std::size_t i = n_entries_; i-- > 0; ) {
            if (entries_[i].tick_fn != nullptr) {
                entries_[i].tick_fn(entries_[i].obj, cycle_tsc);
            }
        }
        return dispatched;
    }

    // ── ICMP Frag Needed feedback (PMTU discovery) ───────────────────────

    /// @brief User-provided callback fired for every ICMP Type 3 Code 4
    ///        message the Poller sees whose embedded 4-tuple + protocol
    ///        are well-formed. The callback dispatches to the right
    ///        stream; the Poller itself only handles parsing.
    ///
    /// Single callback per Poller. Multi-stream users need to build
    /// their own dispatcher or rely on single-stream topology (which
    /// is the common HFT case).
    using IcmpFragNeededCallback = void(*)(void* user,
                                            uint32_t embedded_src_ip,
                                            uint32_t embedded_dst_ip,
                                            uint16_t embedded_src_port,
                                            uint16_t embedded_dst_port,
                                            uint8_t  embedded_proto,
                                            uint16_t next_hop_mtu) noexcept;

    /// @brief Register an ICMP Frag Needed callback. Pass nullptr to
    ///        disable. `user` is opaque — passed back to the callback
    ///        verbatim so the caller can route to the right receiver.
    void set_icmp_callback(IcmpFragNeededCallback cb, void* user) noexcept {
        icmp_cb_   = cb;
        icmp_user_ = user;
    }

    /// @brief Diagnostic counter — number of ICMP Type 3 Code 4 messages
    ///        that were parsed successfully and dispatched via the
    ///        registered callback.
    [[nodiscard]] uint64_t icmp_frag_needed_dispatched() const noexcept {
        return icmp_frag_needed_dispatched_;
    }

    // ── Introspection (test hooks) ───────────────────────────────────────

    [[nodiscard]] std::size_t size() const noexcept { return n_entries_; }
    [[nodiscard]] uint16_t port_id() const noexcept { return cfg_.port_id; }
    [[nodiscard]] uint16_t rx_queue_id() const noexcept { return cfg_.rx_queue_id; }

    /// @brief Number of packets dropped due to hash collision (hash matched
    ///        but full tuple compare failed). Non-zero values indicate either
    ///        extremely unlucky hash distribution or potential attack traffic.
    [[nodiscard]] uint64_t hash_collision_drops() const noexcept { return hash_collision_drops_; }

    // ── Source port selection (client-side helper) ───────────────────────

    /// @brief Suggest an unused source port for a new TCP client connection.
    ///
    /// Scans the currently registered Pollables and returns a port in
    /// `[range_begin, range_end]` such that the 4-tuple
    /// `(src_ip, dst_ip, dst_port, result)` does not conflict with any
    /// existing entry. Selection is random-start linear probe.
    ///
    /// Selection policy:
    ///   - If `preferred != 0` and is within range and not in use, return
    ///     it directly (soft-preference fast path).
    ///   - Otherwise, `getrandom(2)` seeds a random starting point; we
    ///     probe forward modulo the range until the first non-conflicting
    ///     port is found. Random start spreads re-picks across the entire
    ///     range, which is what lets us avoid a separate 2MSL grace window
    ///     — a recently-released port is, on a 28k-wide range, very
    ///     unlikely to be picked immediately after release.
    ///   - If every port in the range conflicts, returns `OutOfMemory`.
    ///
    /// Thread safety: advisory query only. The returned port can become
    /// stale the instant another thread modifies the Poller.
    /// `DpdkPoller` itself is not MT-safe; typical usage is a single
    /// driver thread, and the authoritative check is `add()` which
    /// rejects duplicate 5-tuples. Callers can retry `pick_src_port` on
    /// add() failure.
    ///
    /// Usage:
    ///     auto port = poller->pick_src_port(src_ip, dst_ip, 443).value();
    ///     cfg.legacy.tuple.src_port = port;
    ///     auto stream = DpdkTcpStream::create(std::move(cfg)).value();
    ///     auto add_r  = poller->add(stream.get());  // authoritative
    ///
    /// @param src_ip       Source IPv4 (host order) — must match cfg.tuple.src_ip
    /// @param dst_ip       Destination IPv4 (host order)
    /// @param dst_port     Destination port (host order)
    /// @param range_begin  Inclusive lower bound (default 32768, Linux ephemeral)
    /// @param range_end    Inclusive upper bound (default 60999, Linux ephemeral)
    /// @param preferred    Optional soft preference (0 = no preference)
    [[nodiscard]] std::expected<uint16_t, core::ErrorInfo>
    pick_src_port(uint32_t src_ip,
                  uint32_t dst_ip,
                  uint16_t dst_port,
                  uint16_t range_begin = 32768,
                  uint16_t range_end   = 60999,
                  uint16_t preferred   = 0) const noexcept {
        auto* log = detail::poller_logger();

        if (range_begin < 1024) {
            SPDLOG_LOGGER_WARN(log,
                "DpdkPoller::pick_src_port: range_begin={} < 1024",
                range_begin);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "DpdkPoller::pick_src_port: range_begin must be >= 1024"});
        }
        if (range_begin > range_end) {
            SPDLOG_LOGGER_WARN(log,
                "DpdkPoller::pick_src_port: inverted range [{}, {}]",
                range_begin, range_end);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "DpdkPoller::pick_src_port: range_begin > range_end"});
        }
        if (preferred != 0 &&
            (preferred < range_begin || preferred > range_end)) {
            SPDLOG_LOGGER_WARN(log,
                "DpdkPoller::pick_src_port: preferred={} outside [{}, {}]",
                preferred, range_begin, range_end);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "DpdkPoller::pick_src_port: preferred out of range"});
        }

        // Is a (src_ip, dst_ip, dst_port, candidate) 4-tuple already in
        // use by a registered Pollable? Linear scan over entries_.
        //
        // NOTE: This check is intentionally 4-tuple (protocol-agnostic),
        // NOT 5-tuple like the routing table. Picking an ephemeral source
        // port is almost exclusively a TCP-client concern, and the
        // over-restriction (refusing to pick a port that is "free for TCP
        // but in use by an unrelated UDP Pollable") is conservative —
        // never causes misrouting, only blocks a rare cross-protocol
        // reuse case. A proto-aware variant can be added later if a
        // real workload demands it.
        auto is_in_use = [&](uint16_t candidate) noexcept -> bool {
            for (std::size_t i = 0; i < n_entries_; ++i) {
                const auto& e = entries_[i];
                if (e.src_ip   == src_ip   &&
                    e.dst_ip   == dst_ip   &&
                    e.dst_port == dst_port &&
                    e.src_port == candidate) {
                    return true;
                }
            }
            return false;
        };

        // Soft preference fast path.
        if (preferred != 0 && !is_in_use(preferred)) {
            SPDLOG_LOGGER_DEBUG(log,
                "DpdkPoller::pick_src_port: preferred={} accepted", preferred);
            return preferred;
        }

        // Random-start linear probe over [range_begin, range_end].
        const uint32_t range =
            static_cast<uint32_t>(range_end - range_begin) + 1u;
        uint32_t seed = 0;
        if (::getrandom(&seed, sizeof(seed), 0) !=
            static_cast<ssize_t>(sizeof(seed))) {
            // getrandom on Linux ≥ 3.17 never fails for small reads;
            // if it does, fall back to 0 and log loudly.
            SPDLOG_LOGGER_ERROR(log,
                "DpdkPoller::pick_src_port: getrandom failed, seed=0");
            seed = 0;
        }
        const uint32_t start = seed % range;
        for (uint32_t i = 0; i < range; ++i) {
            const uint16_t candidate = static_cast<uint16_t>(
                range_begin + ((start + i) % range));
            if (!is_in_use(candidate)) {
                SPDLOG_LOGGER_DEBUG(log,
                    "DpdkPoller::pick_src_port: selected port={} "
                    "after {} probe(s)", candidate, i + 1);
                return candidate;
            }
        }

        SPDLOG_LOGGER_WARN(log,
            "DpdkPoller::pick_src_port: no free port in [{}, {}] for "
            "src_ip=0x{:08x} dst_ip=0x{:08x} dst_port={} entries={}",
            range_begin, range_end, src_ip, dst_ip, dst_port, n_entries_);
        return std::unexpected(core::ErrorInfo{
            core::Error::OutOfMemory,
            "DpdkPoller::pick_src_port: no free port in range"});
    }

private:
    /// @brief Per-registered-Pollable state. Pure POD — kept in a fixed-
    ///        size array so the hot path has no indirection through a
    ///        vector header. The routing key is a 5-tuple (4-tuple +
    ///        IP protocol number) to disambiguate TCP and UDP Pollables
    ///        that share the same (src_ip, dst_ip, src_port, dst_port).
    struct PollableEntry {
        void*    obj               = nullptr;
        void   (*process_burst_fn)(void*, rte_mbuf**, uint16_t, uint64_t) noexcept = nullptr;
        void   (*detach_fn)(void*) noexcept = nullptr;
        void   (*tuple_fn)(void*, uint32_t*, uint32_t*, uint16_t*, uint16_t*, uint8_t*) noexcept = nullptr;
        void   (*tick_fn)(void*, uint64_t) noexcept = nullptr;  ///< per-poll-cycle periodic hook
        uint32_t conn_hash         = 0;
        uint32_t src_ip            = 0;
        uint32_t dst_ip            = 0;
        uint16_t src_port          = 0;
        uint16_t dst_port          = 0;
        uint8_t  proto             = 0;  ///< IP proto (kIpProtoTcp=6 / kIpProtoUdp=17)
    };
    // Sanity: adding fields to the entry blows past a single 64B cacheline
    // on x86-64 once we cross this threshold, which regresses the linear
    // scan. If this assert fails, rethink the layout (bitpack / split
    // hot vs cold) before raising the bound.
    static_assert(sizeof(PollableEntry) <= 64,
                  "PollableEntry must fit in one 64B cacheline");

    explicit DpdkPoller(PollerConfig cfg) noexcept : cfg_(cfg) {}

    /// @brief Route an incoming mbuf to the matching PollableEntry by
    ///        5-tuple. Handles both TCP and UDP; protocol is part of the
    ///        key so a TCP packet cannot be misrouted to a UDP Pollable
    ///        that happens to share the same (src_ip, dst_ip, src_port,
    ///        dst_port) — and vice versa.
    PollableEntry* lookup_by_5tuple_(rte_mbuf* mbuf) noexcept {
        // L2+L3 parse first so we can dispatch by protocol without doing
        // the TCP or UDP parse twice in the collision / miss path.
        auto ip_hdr = eph::dpdk::net::parse_ip_header(mbuf);
        if (!ip_hdr) return nullptr;

        uint32_t pkt_src_ip = 0, pkt_dst_ip = 0;
        uint16_t pkt_src_port = 0, pkt_dst_port = 0;
        const uint8_t pkt_proto = ip_hdr.proto;

        if (pkt_proto == eph::dpdk::net::kIpProtoTcp) {
            auto parsed = eph::dpdk::net::parse_tcp_from_ip(mbuf, ip_hdr);
            if (!parsed.tcp) return nullptr;
            pkt_src_ip   = parsed.src_ip();
            pkt_dst_ip   = parsed.dst_ip();
            pkt_src_port = parsed.src_port();
            pkt_dst_port = parsed.dst_port();
        } else if (pkt_proto == eph::dpdk::net::kIpProtoUdp) {
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
        // local 5-tuple"). The hash function is direction-symmetric on the
        // 4 address fields; protocol is a single byte and is the same
        // regardless of direction, so the resulting hash fits both
        // directions. The equality check afterwards verifies the swap
        // and the protocol match.
        const uint32_t pkt_hash = detail::hash_tuple(
            pkt_src_ip, pkt_dst_ip, pkt_src_port, pkt_dst_port, pkt_proto);
        for (std::size_t i = 0; i < n_entries_; ++i) {
            auto& e = entries_[i];
            if (e.conn_hash != pkt_hash) continue;
            // Full 5-tuple compare with src/dst swap — reject hash collisions
            // AND cross-protocol collisions.
            if (pkt_src_ip   == e.dst_ip   && pkt_dst_ip   == e.src_ip   &&
                pkt_src_port == e.dst_port && pkt_dst_port == e.src_port &&
                pkt_proto    == e.proto) {
                return &e;
            }
            // Hash matched but tuple didn't — genuine hash collision. Warn
            // once per Poller instance so the first occurrence is visible
            // in logs, then rely on the running counter exposed via
            // hash_collision_drops() for cumulative tracking.
            if (hash_collision_drops_ == 0) {
                SPDLOG_LOGGER_WARN(detail::poller_logger(),
                    "DpdkPoller::lookup_by_5tuple_: first hash collision "
                    "(pkt_hash=0x{:08x} proto={} src=0x{:08x}:{} dst=0x{:08x}:{}); "
                    "subsequent collisions tracked via hash_collision_drops()",
                    pkt_hash, pkt_proto, pkt_src_ip, pkt_src_port,
                    pkt_dst_ip, pkt_dst_port);
            }
            ++hash_collision_drops_;
        }
        return nullptr;
    }

    /// @brief Inspect an un-routed mbuf; if it is an ICMP Frag Needed
    ///        message carrying a valid embedded 4-tuple, fire the
    ///        registered ICMP callback. Silently returns otherwise —
    ///        the caller is responsible for freeing the mbuf regardless.
    void maybe_dispatch_icmp_(rte_mbuf* mbuf) noexcept {
        if (icmp_cb_ == nullptr) return;
        auto parsed = eph::dpdk::net::parse_icmp(mbuf);
        if (!parsed || !parsed.is_frag_needed() || !parsed.embedded_valid) {
            return;
        }
        icmp_cb_(icmp_user_,
                 parsed.embedded_src_ip,  parsed.embedded_dst_ip,
                 parsed.embedded_src_port, parsed.embedded_dst_port,
                 parsed.embedded_proto,    parsed.next_hop_mtu);
        ++icmp_frag_needed_dispatched_;
    }

    PollerConfig                       cfg_{};
    std::array<PollableEntry, kMaxConn> entries_{};
    std::size_t                         n_entries_{0};
    uint64_t                            hash_collision_drops_{0};

    // ── ICMP Frag Needed callback state ──
    IcmpFragNeededCallback              icmp_cb_{nullptr};
    void*                               icmp_user_{nullptr};
    uint64_t                            icmp_frag_needed_dispatched_{0};
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

    [[nodiscard]] std::expected<uint16_t, core::ErrorInfo>
    pick_src_port(uint32_t src_ip, uint32_t dst_ip, uint16_t dst_port,
                  uint16_t range_begin = 32768,
                  uint16_t range_end   = 60999,
                  uint16_t preferred   = 0) const noexcept {
        return impl_->pick_src_port(src_ip, dst_ip, dst_port,
                                    range_begin, range_end, preferred);
    }

    using IcmpFragNeededCallback =
        typename DpdkPoller<void>::IcmpFragNeededCallback;
    void set_icmp_callback(IcmpFragNeededCallback cb, void* user) noexcept {
        impl_->set_icmp_callback(cb, user);
    }
    [[nodiscard]] uint64_t icmp_frag_needed_dispatched() const noexcept {
        return impl_->icmp_frag_needed_dispatched();
    }

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
