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
///     returns. The `poll(std::chrono::nanoseconds)` overload is a
///     debugging / development convenience: it does one immediate burst
///     then spins on `rte_pause()` for ~10 µs before falling back to
///     `std::this_thread::yield()` until the timeout fires. Production
///     HFT colo deployments should keep using the no-arg form.
///
///   - **Routing table**: O(1) open-addressed hash table keyed on the
///     5-tuple `(src_ip, dst_ip, src_port, dst_port, proto)`. The storage
///     is sized at compile time to `kRouteTableSize` (= 2 × `kMaxConnHard`,
///     a power of two so the modulo is a single bitmask), keeping the
///     load factor at or below 50% — linear-probe chains stay short and
///     hot-path lookups compile to a hash-then-memcmp on a single cache
///     line. Tombstones preserve probe chains across remove/add cycles.
///     The compile-time storage costs ~4 KiB per Poller; legitimate for
///     a single-instance HFT lcore. The previous flat linear scan
///     (kMaxConn=16) is gone — the new design is strictly cheaper from
///     N=2 upward and identical-or-better at N=1.
///
///   - **Pollable notification hooks**: every `Pollable` in the DPDK
///     backend (`DpdkTcpStream`, `DpdkUdpSocket`) exposes
///     `notify_attached_(DpdkPoller*)` / `notify_detached_()` so the
///     `add`/`remove` path can clear / set the stream's `attached_to_`
///     pointer without a cross-class friend declaration.
///
///   - **Not thread-safe**: one Poller owns one lcore.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <memory>
#include <thread>
#include <utility>

#include <sys/random.h>   // getrandom(2) — random start for pick_src_port

#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_pause.h>    // rte_pause() — short busy-wait yield for poll(timeout)

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
    // (capped at kMaxConnHard registered entries with a 2x-sized open-
    // addressed table; collision rate is dominated by the table size,
    // not the hash range).
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
    /// @brief Default runtime cap on registered Pollables when the caller
    ///        does not set `PollerConfig::max_connections`. Kept at 16 for
    ///        backwards compatibility — the underlying storage is the
    ///        larger `kMaxConnHard`, so raising this is a no-op cost.
    static constexpr std::size_t kMaxConn = 16;

    /// @brief Hard upper bound on registered Pollables — the compile-time
    ///        storage size of `entries_` and the basis for the open-
    ///        addressed routing table. Bumped from the historical 16 so
    ///        callers can opt-in via `PollerConfig::max_connections` to
    ///        single-lcore fan-outs that previously needed a second
    ///        Poller. Memory cost: ~4 KiB per Poller (entries_ + route
    ///        table), cheap relative to a Poller's lifetime.
    static constexpr std::size_t kMaxConnHard = 64;

    /// @brief Size of the open-addressed routing hash table. Power of two
    ///        so `hash & kRouteTableMask` is a single bitmask. Sized to
    ///        `2 × kMaxConnHard` to cap load factor at 50% — average
    ///        linear-probe chain stays ≤ 2 lookups even at full
    ///        capacity, well within a single 64B cache line.
    static constexpr std::size_t kRouteTableSize = 128;
    static_assert((kRouteTableSize & (kRouteTableSize - 1)) == 0,
                  "kRouteTableSize must be a power of two");
    static_assert(kRouteTableSize >= 2 * kMaxConnHard,
                  "kRouteTableSize must keep load factor <= 50%");
    static constexpr std::size_t kRouteTableMask = kRouteTableSize - 1;

    /// @brief DPDK burst size — matches the canonical 32-mbuf burst used
    ///        across the codebase (TcpSession::poll_rx, microbenchmarks).
    static constexpr uint16_t kBurstSize = 32;

    // ── Factory ──────────────────────────────────────────────────────────

    [[nodiscard]] static std::expected<std::unique_ptr<DpdkPoller>, core::ErrorInfo>
    create(PollerConfig cfg = {}) noexcept {
        [[maybe_unused]] auto* log = detail::poller_logger();
        // Clamp / validate the policy knob before we allocate. Out-of-range
        // values are a config bug — surface immediately rather than letting
        // them lurk until the Nth `add` call.
        if (cfg.max_connections == 0 ||
            cfg.max_connections > kMaxConnHard) {
            SPDLOG_LOGGER_WARN(log,
                "DpdkPoller::create: max_connections={} out of range [1, {}]",
                cfg.max_connections, kMaxConnHard);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "DpdkPoller::create: max_connections out of range"});
        }
        SPDLOG_LOGGER_DEBUG(log,
            "DpdkPoller::create: port={} queue={} max_connections={}",
            cfg.port_id, cfg.rx_queue_id, cfg.max_connections);
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
            SPDLOG_LOGGER_ERROR(log, "DpdkPoller::add: nullptr obj");
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "DpdkPoller::add: nullptr"});
        }
        if (n_entries_ >= cfg_.max_connections) {
            SPDLOG_LOGGER_ERROR(log,
                "DpdkPoller::add: entries table full "
                "(n_entries={}, max_connections={}, kMaxConnHard={})",
                n_entries_, cfg_.max_connections, kMaxConnHard);
            return std::unexpected(core::ErrorInfo{
                core::Error::OutOfMemory,
                "DpdkPoller::add: entries table full"});
        }
        // Retrieve the incoming 5-tuple up front so the duplicate check
        // (against route_table_) and the route-table insertion both reuse
        // the same field reads. 5-tuple duplicate makes routing ambiguous
        // (silent data corruption on the hot path, which is catastrophic
        // in HFT). TCP and UDP Pollables sharing the exact same 4-tuple
        // are legitimate — protocol is part of the key.
        uint32_t new_src_ip = 0, new_dst_ip = 0;
        uint16_t new_src_port = 0, new_dst_port = 0;
        uint8_t  new_proto = 0;
        obj->tuple_for_poller_(&new_src_ip, &new_dst_ip,
                                &new_src_port, &new_dst_port,
                                &new_proto);
        const uint32_t new_hash = detail::hash_tuple(
            new_src_ip, new_dst_ip, new_src_port, new_dst_port, new_proto);

        // Pointer-duplicate check is rare and bounded by max_connections;
        // a linear scan over the packed entries_ slice is cheaper than
        // threading obj-pointer through the route table key.
        for (std::size_t i = 0; i < n_entries_; ++i) {
            if (entries_[i].obj == static_cast<void*>(obj)) {
                SPDLOG_LOGGER_WARN(log,
                    "DpdkPoller::add: pointer already registered obj={} "
                    "at idx={} of n_entries={}",
                    static_cast<void*>(obj), i, n_entries_);
                return std::unexpected(core::ErrorInfo{
                    core::Error::InvalidConfig,
                    "DpdkPoller::add: already registered"});
            }
        }

        // 5-tuple duplicate check via the routing hash table — same probe
        // path the hot lookup uses.
        if (find_route_slot_(new_hash, new_src_ip, new_dst_ip,
                              new_src_port, new_dst_port, new_proto)
                != kInvalidSlot) {
            SPDLOG_LOGGER_WARN(log,
                "DpdkPoller::add: 5-tuple already registered "
                "proto={} src=0x{:08x}:{} dst=0x{:08x}:{}",
                new_proto, new_src_ip, new_src_port,
                new_dst_ip, new_dst_port);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "DpdkPoller::add: 5-tuple already registered"});
        }

        const std::size_t entry_idx = n_entries_;
        PollableEntry& entry = entries_[entry_idx];
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

        // Insert into the routing hash table. Linear probe with tombstone
        // reuse — invariant maintained: load factor ≤ 50%, so a free
        // (Empty or Tombstone) slot exists within the kRouteTableSize
        // probe budget.
        insert_route_(new_hash, new_src_ip, new_dst_ip, new_src_port,
                       new_dst_port, new_proto, entry_idx);

        obj->notify_attached_(this);
        SPDLOG_LOGGER_DEBUG(log,
            "DpdkPoller::add: obj={} tuple_hash=0x{:08x} entries={} idx={}",
            static_cast<void*>(obj), entry.conn_hash, n_entries_, entry_idx);
        return {};
    }

    /// @brief Unregister `obj`. Returns `InvalidConfig` if `obj` is nullptr
    /// (caller programming error); returns `NotFound` if `obj` was never
    /// registered or was already removed (recoverable state mismatch).
    template <DpdkPollable P>
    [[nodiscard]] std::expected<void, core::ErrorInfo> remove(P* obj) noexcept {
        auto* log = detail::poller_logger();
        if (obj == nullptr) {
            SPDLOG_LOGGER_ERROR(log, "DpdkPoller::remove: nullptr obj");
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "DpdkPoller::remove: nullptr"});
        }
        for (std::size_t i = 0; i < n_entries_; ++i) {
            if (entries_[i].obj != static_cast<void*>(obj)) continue;

            // Step 1: tombstone the route_table_ slot pointing at this
            // entry (snapshot the key from entries_[i] before it gets
            // overwritten by the shift-left).
            tombstone_route_(entries_[i].conn_hash,
                              entries_[i].src_ip, entries_[i].dst_ip,
                              entries_[i].src_port, entries_[i].dst_port,
                              entries_[i].proto);

            // Step 2: shift-left the tail of entries_ to preserve the
            // packed-array invariant. entries_ is small (kMaxConnHard)
            // so the copy cost stays in cache. Each shifted entry's
            // index in route_table_ must be decremented to track its
            // new position — Step 3.
            for (std::size_t j = i + 1; j < n_entries_; ++j) {
                entries_[j - 1] = entries_[j];
            }
            --n_entries_;
            entries_[n_entries_] = PollableEntry{};

            // Step 3: walk the route table once and rewrite any slot
            // whose entry_idx pointed to a position > i. This is O(table
            // size) once per remove — amortised over the connection's
            // lifetime, the bookkeeping is well below noise.
            if (i < n_entries_) {  // tail shifted only if i was not last
                shift_down_route_indices_above_(static_cast<uint8_t>(i));
            }

            obj->notify_detached_();
            SPDLOG_LOGGER_DEBUG(log,
                "DpdkPoller::remove: obj={} entries={}",
                static_cast<void*>(obj), n_entries_);
            return {};
        }
        SPDLOG_LOGGER_WARN(log,
            "DpdkPoller::remove: obj={} not registered (n_entries={})",
            static_cast<void*>(obj), n_entries_);
        return std::unexpected(core::ErrorInfo{
            core::Error::NotFound,
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

    /// @brief CPU-yielding burst poll for **debugging / development**
    ///        environments. Production HFT colo deployments should keep
    ///        calling the no-arg `poll()` in a tight busy loop; this
    ///        overload trades latency for CPU cooperation so a developer
    ///        loop doesn't peg a core while waiting for the occasional
    ///        packet.
    ///
    /// Strategy (conservative, three-phase):
    ///   1. **Burst** — call `poll()` once. If any packets dispatched,
    ///      return immediately. The fastest path matches the no-arg
    ///      hot path exactly.
    ///   2. **Spin** — for the first `kSpinBudgetNs` (~10 µs) of the
    ///      timeout window, retry the burst inside an `rte_pause()`
    ///      loop. This is the lowest-latency yield available on x86
    ///      (`PAUSE` instruction) / aarch64 (`yield` hint) and keeps
    ///      packet wakeup well under a microsecond when traffic
    ///      arrives during the spin.
    ///   3. **Yield** — once the spin budget is exhausted, swap to
    ///      `std::this_thread::yield()` so the kernel can schedule
    ///      other work. Continues retrying the burst between yields
    ///      until the total elapsed time exceeds `timeout`.
    ///
    /// Returns the total number of packets processed across all bursts
    /// (could be 0 if the timeout fires with no traffic).
    ///
    /// `timeout` ≤ 0 collapses to a single burst, equivalent to the
    /// no-arg `poll()`. Negative values are treated as 0 so a stale
    /// `now() - deadline` calculation can't hang the caller.
    ///
    /// **Hot path is not affected**: the no-arg `poll()` overload above
    /// stays byte-for-byte unchanged; this overload is opt-in.
    ///
    /// **Thread safety**: same as `poll()` — single-thread driver.
    [[nodiscard]] std::size_t poll(std::chrono::nanoseconds timeout) noexcept {
        [[maybe_unused]] auto* log = detail::poller_logger();
        SPDLOG_LOGGER_TRACE(log,
            "DpdkPoller::poll(timeout): entry timeout={} ns",
            timeout.count());

        // Phase 1: always do at least one burst. If anything came back —
        // including from the test-injection seam — return immediately
        // without waiting. Matches the "if a packet arrived, return
        // immediately" requirement.
        std::size_t total = drain_injected_for_test_() + poll();
        if (total > 0 || timeout.count() <= 0) {
            SPDLOG_LOGGER_TRACE(log,
                "DpdkPoller::poll(timeout): exit fast (n={}, timeout={} ns)",
                total, timeout.count());
            return total;
        }

        // Phase 2 + 3 require a wall-clock measure. Prefer TSC for sub-
        // microsecond resolution; fall back to steady_clock when
        // uncalibrated (matches ReconnectOrchestrator's precedent in
        // eph-net/include/eph/net/reconnect_orchestrator.hpp). The
        // fallback path is a few ns more expensive per loop iteration
        // — acceptable because this overload is opt-in for non-hot
        // codepaths.
        const bool use_tsc = eph::utils::TSC::is_initialized();

        // Spin budget: small enough that a developer loop doesn't burn
        // measurable CPU, large enough that the spin path catches the
        // common "packet arrived in the next few µs" case before falling
        // back to the OS scheduler. 10 µs matches the order-of-magnitude
        // packet inter-arrival in a moderate market data feed.
        constexpr auto kSpinBudget = std::chrono::microseconds(10);
        const auto spin_budget =
            (timeout < kSpinBudget) ? timeout : std::chrono::nanoseconds(kSpinBudget);

        if (use_tsc) {
            // TSC path — preferred. `to_cycles` returns nullopt if a race
            // somehow uninitialised the calibration between the
            // is_initialized() check and here; treat that as a fallback.
            const uint64_t start_tsc = eph::utils::TSC::now();
            const auto spin_cycles_opt = eph::utils::TSC::to_cycles(spin_budget);
            const auto total_cycles_opt = eph::utils::TSC::to_cycles(timeout);
            if (spin_cycles_opt && total_cycles_opt) {
                const uint64_t spin_deadline  = start_tsc + *spin_cycles_opt;
                const uint64_t total_deadline = start_tsc + *total_cycles_opt;

                // Spin phase — rte_pause() is the cheapest yield available
                // and keeps the wakeup latency in the tens of nanoseconds
                // when a packet shows up.
                while (eph::utils::TSC::now() < spin_deadline) {
                    rte_pause();
                    const std::size_t n = drain_injected_for_test_() + poll();
                    if (n > 0) {
                        total += n;
                        SPDLOG_LOGGER_TRACE(log,
                            "DpdkPoller::poll(timeout): exit spin (n={})", total);
                        return total;
                    }
                }

                // Yield phase — kernel-cooperative. Each wakeup retries
                // the burst; we drain the deadline on no-traffic, no-tick
                // pollers without burning a core.
                while (eph::utils::TSC::now() < total_deadline) {
                    std::this_thread::yield();
                    const std::size_t n = drain_injected_for_test_() + poll();
                    if (n > 0) {
                        total += n;
                        SPDLOG_LOGGER_TRACE(log,
                            "DpdkPoller::poll(timeout): exit yield (n={})", total);
                        return total;
                    }
                }
                SPDLOG_LOGGER_TRACE(log,
                    "DpdkPoller::poll(timeout): exit deadline (n={})", total);
                return total;
            }
            // Fall through to steady_clock path if to_cycles failed.
        }

        // steady_clock fallback — used when TSC was never calibrated
        // (typical in unit tests that skip TSC::init()). Slightly more
        // expensive per iteration but correct.
        const auto wall_start = std::chrono::steady_clock::now();
        const auto spin_deadline  = wall_start + spin_budget;
        const auto total_deadline = wall_start + timeout;

        while (std::chrono::steady_clock::now() < spin_deadline) {
            rte_pause();
            const std::size_t n = drain_injected_for_test_() + poll();
            if (n > 0) {
                total += n;
                SPDLOG_LOGGER_TRACE(log,
                    "DpdkPoller::poll(timeout): exit spin/wall (n={})", total);
                return total;
            }
        }
        while (std::chrono::steady_clock::now() < total_deadline) {
            std::this_thread::yield();
            const std::size_t n = drain_injected_for_test_() + poll();
            if (n > 0) {
                total += n;
                SPDLOG_LOGGER_TRACE(log,
                    "DpdkPoller::poll(timeout): exit yield/wall (n={})", total);
                return total;
            }
        }
        SPDLOG_LOGGER_TRACE(log,
            "DpdkPoller::poll(timeout): exit deadline/wall (n={})", total);
        return total;
    }

    // ── ICMP Frag Needed feedback (PMTU discovery) ───────────────────────

    /// @brief Callback invoked for every ICMP Type 3 Code 4 message the
    ///        Poller sees whose embedded 4-tuple + protocol are well-
    ///        formed. The callback is responsible for routing the
    ///        parsed ICMP to the appropriate application receiver
    ///        (typically by walking a shared registry).
    ///
    /// `std::function` rather than a raw `void(*)(void*, ...)` because
    /// the Platform-level dispatch pattern captures a
    /// `shared_ptr<IcmpRegistry>` in the closure, giving the registry
    /// a strong ref so its lifetime extends past Platform destruction
    /// if necessary. Construction typically allocates once (closure
    /// exceeds SBO) — acceptable startup cost, never on the hot path.
    using IcmpFragNeededCallback =
        std::function<void(const eph::dpdk::net::ParsedIcmp&)>;

    /// @brief Install the ICMP callback. **First-install-wins + atomic**
    ///        — later calls return silently without touching the
    ///        existing callback. This is what makes the install
    ///        thread-safe relative to a running `poll()` loop:
    ///
    ///   1. Writer claims the install slot via a CAS on an atomic
    ///      state (`0 → 1`).
    ///   2. Winner writes `icmp_cb_` (no concurrent writer — CAS
    ///      serialized it).
    ///   3. Winner release-stores the state `1 → 2`, publishing the
    ///      callback.
    ///   4. Reader (`poll()` hot path) acquire-loads the state. Only
    ///      `state == 2` means `icmp_cb_` is safe to read; `0` / `1`
    ///      both skip.
    ///
    /// The HFT dynamic-reconnect scenario — control thread calls
    /// `DpdkTcpStream::create_and_attach` repeatedly while the lcore
    /// thread is already polling — is safe: first attach installs
    /// the closure; subsequent attaches' `set_icmp_callback` calls
    /// are no-ops (the closure they'd install is functionally
    /// identical — always `reg_sp->dispatch(parsed)` on the same
    /// shared registry).
    void set_icmp_callback(IcmpFragNeededCallback cb) noexcept {
        uint8_t expected = kIcmpCbStateUnset;
        if (!icmp_cb_state_.compare_exchange_strong(
                expected, kIcmpCbStateInstalling,
                std::memory_order_relaxed)) {
            // State is already Installing or Ready — someone else
            // owns / has owned the install slot. Silently skip.
            return;
        }
        // We own the install slot exclusively; no concurrent writer.
        icmp_cb_ = std::move(cb);
        // Publish: acquire-loaders of state == 2 now see the write
        // above as-if it had happened before their load.
        icmp_cb_state_.store(kIcmpCbStateReady, std::memory_order_release);
    }

    /// @brief Diagnostic counter — number of ICMP Type 3 Code 4 messages
    ///        that were parsed successfully and dispatched via the
    ///        registered callback.
    ///
    /// Atomic read with relaxed ordering: the counter is intended to be
    /// scraped from a separate monitoring thread, so a non-atomic
    /// read of a counter the lcore writes would be a C++ data race
    /// (undefined behaviour) even if observably benign on x86/aarch64.
    [[nodiscard]] uint64_t icmp_frag_needed_dispatched() const noexcept {
        return icmp_frag_needed_dispatched_.load(std::memory_order_relaxed);
    }

    // ── Introspection (test hooks) ───────────────────────────────────────

    [[nodiscard]] std::size_t size() const noexcept { return n_entries_; }
    [[nodiscard]] uint16_t port_id() const noexcept { return cfg_.port_id; }
    [[nodiscard]] uint16_t rx_queue_id() const noexcept { return cfg_.rx_queue_id; }

    /// @brief Number of packets dropped due to hash collision (hash matched
    ///        but full tuple compare failed). Non-zero values indicate either
    ///        extremely unlucky hash distribution or potential attack traffic.
    ///
    /// Atomic read with relaxed ordering — see `icmp_frag_needed_dispatched()`
    /// for the rationale (cross-thread monitoring scrapes must not race
    /// with the lcore's writes).
    [[nodiscard]] uint64_t hash_collision_drops() const noexcept {
        return hash_collision_drops_.load(std::memory_order_relaxed);
    }

    /// @brief Test seam — feed a forged mbuf through the routing path
    ///        without going via `rte_eth_rx_burst`. Returns the dispatch
    ///        target's opaque `void*` (matches the registered Pollable's
    ///        `obj` slot) on hit, or nullptr on miss. The mbuf is NOT
    ///        consumed: the caller retains ownership and must free it.
    ///
    /// This intentionally bypasses the production poll loop's mbuf-free
    /// + ICMP fallback so a unit test can assert "this packet routed to
    /// stream X" deterministically. Production callers should not use
    /// this — `_for_test_` suffix follows the codebase convention.
    [[nodiscard]] void* route_for_test_(rte_mbuf* mbuf) noexcept {
        PollableEntry* e = lookup_by_5tuple_(mbuf);
        return e ? e->obj : nullptr;
    }

    /// @brief Test seam — queue a forged mbuf so the next `poll(timeout)`
    ///        spin / yield iteration picks it up as if it had arrived
    ///        from `rte_eth_rx_burst`. The mbuf is NOT freed by the
    ///        Poller: the test owns the storage and must keep it alive
    ///        until the dispatch returns.
    ///
    /// Capacity = 1 (single slot). A second inject before the first is
    /// drained silently overwrites — tests should drain via `poll(timeout)`
    /// before re-injecting. Production callers must never touch this:
    /// the `_for_test_` suffix and the explicit "single-slot, lossy"
    /// semantics make that obvious.
    ///
    /// The injection is consumed only by the `poll(timeout)` path —
    /// the no-arg `poll()` hot path stays byte-for-byte unchanged.
    void inject_mbuf_for_test_(rte_mbuf* mbuf) noexcept {
        injected_mbuf_for_test_ = mbuf;
    }

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
    ///     cfg.dpdk.tcp_low_level.tuple.src_port = port;
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

    /// @brief One open-addressed routing-table slot.
    ///
    /// State machine: `Empty` (probe terminator on miss) → `Occupied`
    /// (after `add`) → `Tombstone` (after `remove`; probe continues past
    /// it on lookup, but `add` can reuse it). All key fields are inline
    /// so a successful lookup hits the slot's cache line and a single
    /// 13-byte memcmp completes routing.
    struct RouteSlot {
        // Layout-constants for the state byte. Plain enum (not enum class)
        // so the byte fits naturally and we don't need static_cast in
        // memcmp helpers.
        static constexpr uint8_t kEmpty     = 0;
        static constexpr uint8_t kOccupied  = 1;
        static constexpr uint8_t kTombstone = 2;

        uint8_t  state       = kEmpty;
        uint8_t  entry_idx   = kInvalidSlot;  ///< index into entries_
        uint8_t  proto       = 0;
        uint8_t  _pad0       = 0;
        uint16_t src_port    = 0;
        uint16_t dst_port    = 0;
        uint32_t src_ip      = 0;
        uint32_t dst_ip      = 0;
        uint32_t conn_hash   = 0;             ///< cached for cheap rehash on probe
    };
    static_assert(sizeof(RouteSlot) <= 32,
                  "RouteSlot should fit in half a cacheline so two slots "
                  "share one line on the typical-probe path");

    /// @brief Sentinel for "no entry" / "no slot found". We use 0xFF in
    ///        a uint8_t which keeps `entry_idx` packing tight — kMaxConnHard
    ///        is 64, so legitimate indices live in [0, 63].
    static constexpr uint8_t kInvalidSlot = 0xFFu;

    explicit DpdkPoller(PollerConfig cfg) noexcept : cfg_(cfg) {}

    // ── Routing hash table helpers ───────────────────────────────────────

    /// @brief Probe the routing table for the given key. Returns the
    ///        `entries_` index if found, or `kInvalidSlot` on miss.
    ///        Sets `last_lookup_had_collision_` so the lookup hot path
    ///        can fire the WARN diagnostic.
    [[nodiscard]] uint8_t find_route_slot_(uint32_t hash,
                                            uint32_t src_ip,
                                            uint32_t dst_ip,
                                            uint16_t src_port,
                                            uint16_t dst_port,
                                            uint8_t  proto) const noexcept {
        last_lookup_had_collision_ = false;
        // Probe up to kRouteTableSize slots. Linear probing is the
        // simplest collision strategy and works well at our < 50% load
        // factor — the average miss completes in O(1.5) probes.
        std::size_t idx = static_cast<std::size_t>(hash) & kRouteTableMask;
        for (std::size_t step = 0; step < kRouteTableSize; ++step) {
            const RouteSlot& s = route_table_[idx];
            if (s.state == RouteSlot::kEmpty) {
                // Probe terminator: with no Empty in the chain, no later
                // slot can hold this key.
                return kInvalidSlot;
            }
            if (s.state == RouteSlot::kOccupied) {
                if (s.conn_hash == hash &&
                    s.src_ip   == src_ip &&
                    s.dst_ip   == dst_ip &&
                    s.src_port == src_port &&
                    s.dst_port == dst_port &&
                    s.proto    == proto) {
                    return s.entry_idx;
                }
                // Hash matched (or chain continues); record collision so
                // the caller can decide whether to log. Collision is real
                // only when hash matched but tuple did not.
                if (s.conn_hash == hash) {
                    last_lookup_had_collision_ = true;
                }
            }
            // Tombstone: keep probing — this slot used to hold a key
            // before remove(). Skipping past a Tombstone preserves the
            // chain invariant.
            idx = (idx + 1) & kRouteTableMask;
        }
        // Probe budget exhausted with no Empty terminator. Should be
        // impossible at < 50% load factor — the only way this fires is
        // if every slot is Occupied or Tombstone. We treat it as
        // "not found" — the caller will fall through to drop.
        return kInvalidSlot;
    }

    /// @brief Insert a (key → entry_idx) mapping into the routing table.
    ///        Caller has already verified the key is not present (via the
    ///        `find_route_slot_` call in `add`). Tombstones are reused.
    void insert_route_(uint32_t hash,
                        uint32_t src_ip,
                        uint32_t dst_ip,
                        uint16_t src_port,
                        uint16_t dst_port,
                        uint8_t  proto,
                        std::size_t entry_idx) noexcept {
        std::size_t idx = static_cast<std::size_t>(hash) & kRouteTableMask;
        for (std::size_t step = 0; step < kRouteTableSize; ++step) {
            RouteSlot& s = route_table_[idx];
            if (s.state == RouteSlot::kEmpty ||
                s.state == RouteSlot::kTombstone) {
                s.state     = RouteSlot::kOccupied;
                s.entry_idx = static_cast<uint8_t>(entry_idx);
                s.proto     = proto;
                s.src_port  = src_port;
                s.dst_port  = dst_port;
                s.src_ip    = src_ip;
                s.dst_ip    = dst_ip;
                s.conn_hash = hash;
                return;
            }
            idx = (idx + 1) & kRouteTableMask;
        }
        // The capacity invariant (n_entries_ < max_connections, plus
        // kRouteTableSize ≥ 2 × kMaxConnHard) makes this branch
        // unreachable. Logging it loudly is the right defensive action
        // for an invariant violation.
        SPDLOG_LOGGER_ERROR(detail::poller_logger(),
            "DpdkPoller::insert_route_: routing table full at n_entries_={}",
            n_entries_);
    }

    /// @brief Mark the slot for the given key as Tombstone. Used by
    ///        `remove()`. Idempotent — silently returns if the key is
    ///        already gone (defensive).
    void tombstone_route_(uint32_t hash,
                           uint32_t src_ip,
                           uint32_t dst_ip,
                           uint16_t src_port,
                           uint16_t dst_port,
                           uint8_t  proto) noexcept {
        std::size_t idx = static_cast<std::size_t>(hash) & kRouteTableMask;
        for (std::size_t step = 0; step < kRouteTableSize; ++step) {
            RouteSlot& s = route_table_[idx];
            if (s.state == RouteSlot::kEmpty) return;
            if (s.state == RouteSlot::kOccupied &&
                s.conn_hash == hash &&
                s.src_ip   == src_ip &&
                s.dst_ip   == dst_ip &&
                s.src_port == src_port &&
                s.dst_port == dst_port &&
                s.proto    == proto) {
                s.state     = RouteSlot::kTombstone;
                s.entry_idx = kInvalidSlot;
                return;
            }
            idx = (idx + 1) & kRouteTableMask;
        }
    }

    /// @brief After shifting `entries_[i+1..n_entries_]` left by one to
    ///        fill index `removed_idx`, every Occupied route slot whose
    ///        `entry_idx > removed_idx` must be decremented to track the
    ///        new entry position. Single linear pass over the route
    ///        table; called only by `remove()`.
    void shift_down_route_indices_above_(uint8_t removed_idx) noexcept {
        for (auto& s : route_table_) {
            if (s.state == RouteSlot::kOccupied &&
                s.entry_idx > removed_idx &&
                s.entry_idx != kInvalidSlot) {
                --s.entry_idx;
            }
        }
    }

    /// @brief Route an incoming mbuf to the matching PollableEntry by
    ///        5-tuple. Handles both TCP and UDP; protocol is part of the
    ///        key so a TCP packet cannot be misrouted to a UDP Pollable
    ///        that happens to share the same (src_ip, dst_ip, src_port,
    ///        dst_port) — and vice versa.
    ///
    /// Hot path: hash → bitmask → linear probe (≤ 2 slots avg) → memcmp
    /// → return entries_[idx]. No allocation, no vtable, no
    /// `std::function`. Counts hash-collision drops for diagnostics.
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
        // 4 address fields; protocol is the same regardless of direction.
        // We probe the table with the swapped key.
        const uint32_t local_src_ip   = pkt_dst_ip;
        const uint32_t local_dst_ip   = pkt_src_ip;
        const uint16_t local_src_port = pkt_dst_port;
        const uint16_t local_dst_port = pkt_src_port;
        const uint32_t pkt_hash = detail::hash_tuple(
            local_src_ip, local_dst_ip, local_src_port, local_dst_port,
            pkt_proto);

        const uint8_t idx = find_route_slot_(
            pkt_hash, local_src_ip, local_dst_ip,
            local_src_port, local_dst_port, pkt_proto);
        if (idx != kInvalidSlot) {
            return &entries_[idx];
        }

        // Probe ran past at least one Occupied slot whose key did not match
        // (hash collision). The probe-internal counter set by find_route_slot_
        // tells us whether to fire the diagnostic — the function can't return
        // both data and a collision flag, so we use a member set during probe.
        if (last_lookup_had_collision_) {
            // Sustained collision scenarios (adversarial traffic or a
            // pathological hash distribution) must stay observable beyond
            // the first event. Log at WARN on the first collision drop and
            // then every Nth so log volume is bounded but not silent.
            // Must be (power of two) - 1 so the bitmask test is equivalent
            // to `count % interval == 0`.
            static constexpr uint64_t kHashCollisionLogMask = 0x3ffULL;  // 1 / 1024
            const uint64_t cur =
                hash_collision_drops_.load(std::memory_order_relaxed);
            if (cur == 0 || (cur & kHashCollisionLogMask) == 0) {
                SPDLOG_LOGGER_WARN(detail::poller_logger(),
                    "DpdkPoller::lookup_by_5tuple_: hash collision drop #{} "
                    "(pkt_hash=0x{:08x} proto={} src=0x{:08x}:{} dst=0x{:08x}:{}); "
                    "cumulative count via hash_collision_drops()",
                    cur + 1,
                    pkt_hash, pkt_proto, pkt_src_ip, pkt_src_port,
                    pkt_dst_ip, pkt_dst_port);
            }
            hash_collision_drops_.fetch_add(1, std::memory_order_relaxed);
        }
        return nullptr;
    }

    /// @brief Drain the single-slot test-injection seam.
    ///
    /// Called from the `poll(timeout)` overload between burst attempts.
    /// Returns 1 if an injected mbuf was dispatched (or freed via the
    /// ICMP fallback path) and the slot was consumed; 0 if the slot was
    /// empty. The no-arg `poll()` hot path never touches this — keeping
    /// production polling free of any test-only branch.
    ///
    /// Mirrors the dispatch logic in `poll()`: route by 5-tuple → hand
    /// to the matching Pollable; on miss try the ICMP fallback. Unlike
    /// `poll()` we do NOT call `rte_pktmbuf_free` on a miss because the
    /// test owns the mbuf storage.
    std::size_t drain_injected_for_test_() noexcept {
        rte_mbuf* mbuf = injected_mbuf_for_test_;
        if (mbuf == nullptr) return 0;
        injected_mbuf_for_test_ = nullptr;

        const uint64_t cycle_tsc = eph::utils::TSC::now();
        PollableEntry* entry = lookup_by_5tuple_(mbuf);
        if (entry != nullptr) {
            entry->process_burst_fn(entry->obj, &mbuf, 1, cycle_tsc);
            return 1;
        }
        // Routing miss — try ICMP. Either way we consumed the injection,
        // so the test sees the slot drained.
        maybe_dispatch_icmp_(mbuf);
        return 1;
    }

    /// @brief Inspect an un-routed mbuf; if it is an ICMP Frag Needed
    ///        message carrying a valid embedded 4-tuple, fire the
    ///        registered ICMP callback. Silently returns otherwise —
    ///        the caller is responsible for freeing the mbuf regardless.
    ///
    /// Acquire-load pairs with the release-store in
    /// `set_icmp_callback`. `state == kIcmpCbStateReady` (== 2) is
    /// the ONLY state where `icmp_cb_` is safe to read; Unset (0)
    /// and Installing (1) both fast-return.
    void maybe_dispatch_icmp_(rte_mbuf* mbuf) noexcept {
        if (icmp_cb_state_.load(std::memory_order_acquire) !=
                kIcmpCbStateReady) {
            return;
        }
        // Defense-in-depth: state==Ready locks the slot, but the existing
        // `SetIcmpCallbackEmptyIsAlsoInstallOnce` test documents that an
        // empty `std::function` is a valid first-install (it claims the
        // slot and blocks subsequent installs). Without this guard, a
        // genuine ICMP Type 3 Code 4 message arriving after such an
        // empty install would invoke `icmp_cb_(parsed)` on a default-
        // constructed std::function, throwing `std::bad_function_call`
        // — which immediately terminates because this method is
        // `noexcept`. Matches the symmetric `if (cb)` guard already
        // present in `IcmpRegistry::dispatch` (icmp_registry.hpp:244).
        if (!icmp_cb_) [[unlikely]] return;
        auto parsed = eph::dpdk::net::parse_icmp(mbuf);
        if (!parsed || !parsed.is_frag_needed() || !parsed.embedded_valid) {
            return;
        }
        icmp_cb_(parsed);
        icmp_frag_needed_dispatched_.fetch_add(1, std::memory_order_relaxed);
    }

    PollerConfig                            cfg_{};
    /// @brief Packed entry storage. Sized at compile time to the hard
    ///        upper bound; the runtime cap is enforced via
    ///        `cfg_.max_connections` so callers staying at the default
    ///        of 16 see no behavioural change.
    std::array<PollableEntry, kMaxConnHard> entries_{};
    std::size_t                              n_entries_{0};

    /// @brief Open-addressed routing-table storage. `kRouteTableSize`
    ///        slots, indexed by `hash & kRouteTableMask`. Loaded factor
    ///        capped at 50% by static_assert above.
    std::array<RouteSlot, kRouteTableSize>   route_table_{};

    /// @brief Cross-call signal from `find_route_slot_` to the lookup
    ///        hot path: did the probe pass over an Occupied slot whose
    ///        hash matched but tuple did not? Used solely for the
    ///        `hash_collision_drops_` diagnostic counter. Mutable so
    ///        the const probe path can set it.
    mutable bool                             last_lookup_had_collision_{false};
    // Diagnostic counters — atomic to allow safe cross-thread scrape from
    // a monitoring thread while the lcore is in poll(). The lcore is
    // still the only writer, so contention is a non-issue.
    std::atomic<uint64_t>               hash_collision_drops_{0};

    // ── ICMP Frag Needed callback state ──
    //
    // Install-once state machine:
    //   0 = Unset      — no callback ever installed
    //   1 = Installing — a writer claimed the slot, busy writing icmp_cb_
    //   2 = Ready      — icmp_cb_ is committed, safe for readers
    //
    // Exactly one writer ever transitions 0 → 1 (CAS); 1 → 2 is a
    // release-store by the same writer. Readers acquire-load and
    // only touch icmp_cb_ when state == 2.
    static constexpr uint8_t kIcmpCbStateUnset     = 0;
    static constexpr uint8_t kIcmpCbStateInstalling = 1;
    static constexpr uint8_t kIcmpCbStateReady     = 2;

    std::atomic<uint8_t>                icmp_cb_state_{kIcmpCbStateUnset};
    // Only written by the install-slot winner; only read when
    // icmp_cb_state_ == Ready. std::function captures a
    // shared_ptr<IcmpRegistry> in practice (see
    // DpdkTcpStream::create_and_attach), which is what gives the
    // registry lifetime independence from Platform.
    IcmpFragNeededCallback              icmp_cb_{};
    std::atomic<uint64_t>               icmp_frag_needed_dispatched_{0};

    /// @brief Single-slot test-injection seam, drained only by
    ///        `poll(timeout)`. Production `poll()` ignores it. nullptr
    ///        in normal operation; populated by `inject_mbuf_for_test_`
    ///        from the test fixture.
    rte_mbuf*                           injected_mbuf_for_test_{nullptr};
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

    /// @brief Forwards to `DpdkPoller<void>::poll(timeout)` —
    ///        the CPU-yielding debugging form.
    [[nodiscard]] std::size_t poll(std::chrono::nanoseconds timeout) noexcept {
        return impl_->poll(timeout);
    }

    [[nodiscard]] std::size_t size() const noexcept { return impl_->size(); }
    [[nodiscard]] uint16_t port_id() const noexcept { return impl_->port_id(); }
    [[nodiscard]] uint16_t rx_queue_id() const noexcept { return impl_->rx_queue_id(); }

    /// @brief Forwards to `DpdkPoller<void>::hash_collision_drops()` —
    ///        the diagnostic counter for routing-table hash collisions.
    ///        Previously omitted from this primary template; users of
    ///        the typed form (`DpdkPoller<MyStream>`) had no way to
    ///        observe collision pressure that the void specialization
    ///        already exposed.
    [[nodiscard]] uint64_t hash_collision_drops() const noexcept {
        return impl_->hash_collision_drops();
    }
    [[nodiscard]] void* route_for_test_(rte_mbuf* mbuf) noexcept {
        return impl_->route_for_test_(mbuf);
    }
    void inject_mbuf_for_test_(rte_mbuf* mbuf) noexcept {
        impl_->inject_mbuf_for_test_(mbuf);
    }

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
    void set_icmp_callback(IcmpFragNeededCallback cb) noexcept {
        impl_->set_icmp_callback(std::move(cb));
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
