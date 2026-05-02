#pragma once

/// @file detail/queue_allocator.hpp
/// Hugepage-backed greedy queue-pair allocator for the daemon-led
/// `Platform::create` path (S5 of the reshape plan).
///
/// Background: pre-S5 the daemon (`Platform::serve_nic`) configured
/// `cfg.total_queues` queue-pairs on the NIC and every secondary
/// (`Platform::create`) blindly claimed queues `0..cfg.queues-1` —
/// fine for one secondary, racy under multiple. S5 replaces the
/// static placeholder with a daemon-mediated allocator: secondaries
/// send `eph_queue_claim` IPC requests, the daemon's primary owns
/// the live pool in a hugepage memzone, and replies with a granted
/// `[lo, hi)` half-open queue range. Release on Platform destruction
/// is fire-and-forget via `eph_queue_release`.
///
/// Design notes (vs. plan §S5):
///   * **Greedy first-fit** — the daemon scans the bitmap left-to-right
///     and picks the first contiguous free run of `count` queues. No
///     in-line redistribution (plan decision #9): once granted, a
///     range is stable for the lifetime of the secondary process.
///   * **ABA-safe via generation counter** — every claim bumps a
///     monotonic generation. `release` validates the caller's
///     captured generation matches the slot's current one, so a
///     stale release from a previous incarnation cannot free a
///     queue that has been re-claimed by a different secondary.
///   * **Mutex-serialized claims** — the entire claim path
///     (lock + scan + flip bits + bump generation + unlock) runs
///     under a single primitive `pthread_mutex_t` carried in the
///     hugepage struct. Both the IPC handler thread (cold) and any
///     diagnostic eph-nicctl reader contend on this lock; the
///     contention floor is irrelevant since claim/release fire only
///     on Platform create/destroy.
///   * **No secondary-side memzone attach** — secondaries don't
///     `rte_memzone_lookup` the allocator; they go through the IPC
///     path, which the daemon dispatches. `attach_secondary` is
///     retained as a hidden seam for diagnostic CLIs that want to
///     read the pool dump from outside the daemon.
///
/// Header-only, all DPDK API calls live in this file. Hot path: NONE
/// — every entry is invoked exactly twice per Platform lifetime
/// (claim on create, release on destroy).

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <expected>
#include <pthread.h>
#include <span>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_memzone.h>

#include "eph/core/error.hpp"

namespace eph::dpdk::detail {

// ─────────────────────────────────────────────────────────────────────────────
// Public types
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Half-open queue range `[lo, hi)` granted by `claim` and
/// returned to a secondary in the IPC reply.
///
/// `generation` is the slot's monotonic counter at the moment of
/// claim; `release` later checks this value before clearing the
/// bitmap so a stale release (e.g. from a process that already
/// recycled the queues) can't free a slot that has been re-issued.
struct QueueRange {
    uint16_t lo         = 0;     ///< inclusive
    uint16_t hi         = 0;     ///< exclusive
    uint64_t generation = 0;     ///< ABA guard against stale releases

    [[nodiscard]] friend bool operator==(const QueueRange&,
                                         const QueueRange&) = default;

    [[nodiscard]] uint16_t count() const noexcept {
        return static_cast<uint16_t>(hi - lo);
    }
    [[nodiscard]] bool empty() const noexcept { return lo >= hi; }
};

/// @brief Hard upper bound on `total_queues`. 256 is well above the
/// HFT-relevant NIC max (ENA = 32, CX-5 = 63, E810 = 256) and keeps
/// the bitmap a flat `std::array<uint64_t, 4>` so the entire header
/// fits inside a 64 B cache line plus the scratch words.
inline constexpr uint16_t kMaxAllocatorQueues = 256;

namespace queue_allocator_impl {

/// @brief Hugepage struct shared between primary (daemon) and any
/// diagnostic readers. **MUST be trivially copyable + standard-layout**
/// so secondaries can map it directly without constructor/dtor calls.
///
/// Layout:
///   * `magic` / `version`  — sanity headers, validated on attach.
///   * `total_queues`       — pool capacity, fixed at primary create.
///   * `mutex`              — pthread mutex that serializes claim().
///   * `bitmap[]`           — 256 bits = 4 uint64_t. `bit(i)` set iff
///                            queue i is currently claimed.
///   * `generation`         — monotonic counter, bumped on every claim.
///                            ABA guard against stale releases.
///   * `live_releases_drop` — dropped-stale counter (diagnostic).
struct alignas(64) Header {
    static constexpr uint32_t kMagic   = 0x51414c43;  // 'QALC'
    static constexpr uint16_t kVersion = 1;

    uint32_t magic;
    uint16_t version;
    uint16_t total_queues;
    /// pthread_mutex_t with PROCESS_SHARED attribute; safe across
    /// processes that share the hugepage memzone.
    pthread_mutex_t mutex;
    /// One bit per queue, bit i set = queue i claimed. 4 words = 256 bits.
    std::array<uint64_t, kMaxAllocatorQueues / 64> bitmap;
    /// Per-queue "generation at last claim". Lets release() detect
    /// the ABA case where a stale release msg arrives after the
    /// queue has been freed-and-reclaimed by a different secondary
    /// (which gets a different generation). Without this, only the
    /// bitmap-state invariant ("all bits in the released range must
    /// be currently set") would catch ABA, and that fails when the
    /// re-claim happens to land on the EXACT same range as the
    /// original — bits go set→clear→set with the same indices.
    std::array<uint64_t, kMaxAllocatorQueues> claim_gen;
    /// Monotonic counter bumped on every successful claim.
    std::atomic<uint64_t> generation;
    /// Diagnostic — count of release() calls that bounced on stale
    /// generation (a benign race signal, not an error).
    std::atomic<uint64_t> stale_releases;
};

// NOTE: not asserting `is_standard_layout_v<Header>` because
// `std::atomic<uint64_t>` is not standard-layout on libstdc++. Layout
// stability across processes is provided by:
//   * `alignas(64)` pinning the cache-line offset of every field,
//   * fixed-width member types (uint32/16, atomic<uint64>),
//   * `pthread_mutex_t` from the same glibc on every peer.
// init_header zeroes fields by value-store instead of memset to
// respect atomic's no-aliasing contract.

/// @brief Build the memzone name for the allocator: `eph_qalloc/<prefix>`.
/// Mirrors `MpRegistry`'s naming convention so multiple per-NIC
/// daemons coexist without collision.
[[nodiscard]] inline std::expected<std::string, core::ErrorInfo>
build_allocator_memzone_name(std::string_view file_prefix) noexcept {
    if (file_prefix.empty()) {
        return std::unexpected(core::ErrorInfo{
            core::Error::InvalidConfig,
            "QueueAllocator: file_prefix must be non-empty"});
    }
    // RTE_MEMZONE_NAMESIZE = 32 including NUL. "eph_qalloc/" is 11 chars,
    // leaving 20 for the prefix.
    if (file_prefix.size() > 20) {
        return std::unexpected(core::ErrorInfo{
            core::Error::InvalidConfig,
            "QueueAllocator: file_prefix > 20 bytes (memzone name overflow)"});
    }
    std::string name = "eph_qalloc/";
    name.append(file_prefix);
    return name;
}

/// @brief Initialize the hugepage Header in-place. Called once by
/// the primary right after `rte_memzone_reserve_aligned` returns.
/// Configures the mutex with PROCESS_SHARED so cross-proc claims
/// (if ever needed for direct attach) are safe.
///
/// We can't use `memset(hdr, 0, sizeof(*hdr))` because Header
/// contains `std::atomic<uint64_t>` members which aren't trivially-
/// copyable (the standard's CppCon "no aliasing" wording forbids
/// memset on them). Field-by-field zeroing is also clearer about
/// intent.
inline std::expected<void, core::ErrorInfo>
init_header(Header* hdr, uint16_t total_queues) noexcept {
    hdr->magic        = Header::kMagic;
    hdr->version      = Header::kVersion;
    hdr->total_queues = total_queues;
    for (auto& w : hdr->bitmap)    w = 0;
    for (auto& g : hdr->claim_gen) g = 0;
    hdr->generation.store(0, std::memory_order_relaxed);
    hdr->stale_releases.store(0, std::memory_order_relaxed);

    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) {
        return std::unexpected(core::ErrorInfo{
            core::Error::OutOfMemory,
            "QueueAllocator: pthread_mutexattr_init failed"});
    }
    // PROCESS_SHARED: the mutex sits in hugepage shared memory and
    // any process mapping the memzone can lock it.
    (void)pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    if (pthread_mutex_init(&hdr->mutex, &attr) != 0) {
        pthread_mutexattr_destroy(&attr);
        return std::unexpected(core::ErrorInfo{
            core::Error::OutOfMemory,
            "QueueAllocator: pthread_mutex_init failed"});
    }
    pthread_mutexattr_destroy(&attr);

    // bitmap, generation, stale_releases are zero-initialized by memset.
    return {};
}

[[nodiscard]] inline bool
bit_is_set(const Header& hdr, uint16_t i) noexcept {
    return (hdr.bitmap[i / 64] >> (i % 64)) & 1ULL;
}

inline void set_bit(Header& hdr, uint16_t i) noexcept {
    hdr.bitmap[i / 64] |= (1ULL << (i % 64));
}

inline void clear_bit(Header& hdr, uint16_t i) noexcept {
    hdr.bitmap[i / 64] &= ~(1ULL << (i % 64));
}

} // namespace queue_allocator_impl

// ─────────────────────────────────────────────────────────────────────────────
// QueueAllocator
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Hugepage-backed greedy queue-pair allocator.
///
/// Move-only RAII wrapper around the shared Header memzone. The
/// primary (daemon) owns the memzone and frees it on destruction;
/// secondary diagnostic attaches do NOT free the memzone.
class QueueAllocator {
public:
    QueueAllocator() noexcept = default;

    QueueAllocator(const QueueAllocator&)            = delete;
    QueueAllocator& operator=(const QueueAllocator&) = delete;

    QueueAllocator(QueueAllocator&& o) noexcept
        : hdr_(o.hdr_), mz_(o.mz_), owns_memzone_(o.owns_memzone_) {
        o.hdr_           = nullptr;
        o.mz_            = nullptr;
        o.owns_memzone_  = false;
    }

    QueueAllocator& operator=(QueueAllocator&& o) noexcept {
        if (this != &o) {
            release_memzone_();
            hdr_           = o.hdr_;
            mz_            = o.mz_;
            owns_memzone_  = o.owns_memzone_;
            o.hdr_           = nullptr;
            o.mz_            = nullptr;
            o.owns_memzone_  = false;
        }
        return *this;
    }

    ~QueueAllocator() noexcept { release_memzone_(); }

    /// @brief True iff this handle owns a live attachment. Use as
    /// `if (alloc) { ... }` after factory return.
    [[nodiscard]] explicit operator bool() const noexcept {
        return hdr_ != nullptr;
    }

    /// @brief Initialize on the primary side: configure pool of
    /// `total_queues` queues (all initially free). Reserves the
    /// hugepage memzone `eph_qalloc/<file_prefix>` and seeds the
    /// header. Should be called exactly once per NIC by the daemon.
    [[nodiscard]] static std::expected<QueueAllocator, std::string>
    create_primary(std::string_view file_prefix,
                   uint16_t         total_queues) noexcept {
        if (total_queues == 0 || total_queues > kMaxAllocatorQueues) {
            SPDLOG_ERROR(
                "QueueAllocator::create_primary: total_queues={} out of "
                "range (must be 1..{})", total_queues, kMaxAllocatorQueues);
            return std::unexpected(std::string{
                "QueueAllocator::create_primary: total_queues out of range"});
        }
        auto name_r =
            queue_allocator_impl::build_allocator_memzone_name(file_prefix);
        if (!name_r) {
            return std::unexpected(std::string{name_r.error().detail});
        }
        const std::string& name = *name_r;

        // Stale-memzone reset contract — same as MpRegistry: a previous
        // run's daemon may have left an old segment if it died without
        // cleanup. Always reset on primary create.
        if (auto* old = rte_memzone_lookup(name.c_str())) {
            SPDLOG_INFO(
                "QueueAllocator: primary found stale memzone '{}' from a "
                "previous run; freeing before re-reserving", name);
            (void)rte_memzone_free(old);
        }

        const auto* mz = rte_memzone_reserve_aligned(
            name.c_str(),
            sizeof(queue_allocator_impl::Header),
            SOCKET_ID_ANY,
            /*flags=*/0,
            /*align=*/64);
        if (mz == nullptr) {
            SPDLOG_ERROR(
                "QueueAllocator: rte_memzone_reserve_aligned('{}', size={}) "
                "failed (rte_errno={}: {})",
                name, sizeof(queue_allocator_impl::Header),
                rte_errno, rte_strerror(rte_errno));
            return std::unexpected(std::string{
                "QueueAllocator::create_primary: memzone reserve failed"});
        }

        auto* hdr = static_cast<queue_allocator_impl::Header*>(mz->addr);
        if (auto r = queue_allocator_impl::init_header(hdr, total_queues);
            !r) {
            SPDLOG_ERROR(
                "QueueAllocator::create_primary: init_header failed: {}",
                r.error().detail);
            (void)rte_memzone_free(mz);
            return std::unexpected(std::string{r.error().detail});
        }

        SPDLOG_INFO(
            "QueueAllocator: primary reserved memzone '{}' "
            "(magic=0x{:08x} ver={} total_queues={})",
            name, queue_allocator_impl::Header::kMagic,
            queue_allocator_impl::Header::kVersion, total_queues);

        QueueAllocator a;
        a.hdr_          = hdr;
        a.mz_           = mz;
        a.owns_memzone_ = true;
        return a;
    }

    /// @brief Diagnostic attach for `eph-nicctl` peers that want to
    /// dump the pool state from outside the daemon. NOT used by
    /// `Platform::create` — secondaries go through the IPC path.
    /// Returned handle does NOT own the memzone (release_memzone_
    /// is a no-op).
    [[nodiscard]] static std::expected<QueueAllocator, std::string>
    attach_secondary(std::string_view file_prefix) noexcept {
        auto name_r =
            queue_allocator_impl::build_allocator_memzone_name(file_prefix);
        if (!name_r) {
            return std::unexpected(std::string{name_r.error().detail});
        }
        const auto* mz = rte_memzone_lookup(name_r->c_str());
        if (mz == nullptr) {
            SPDLOG_ERROR(
                "QueueAllocator::attach_secondary: memzone '{}' not found "
                "(rte_errno={}) — daemon not running on this NIC?",
                *name_r, rte_errno);
            return std::unexpected(std::string{
                "QueueAllocator::attach_secondary: memzone not found"});
        }
        auto* hdr = static_cast<queue_allocator_impl::Header*>(mz->addr);
        if (hdr->magic != queue_allocator_impl::Header::kMagic) {
            SPDLOG_ERROR(
                "QueueAllocator::attach_secondary: magic mismatch "
                "(found 0x{:08x}, expected 0x{:08x})",
                hdr->magic, queue_allocator_impl::Header::kMagic);
            return std::unexpected(std::string{
                "QueueAllocator::attach_secondary: header magic mismatch"});
        }
        if (hdr->version != queue_allocator_impl::Header::kVersion) {
            SPDLOG_ERROR(
                "QueueAllocator::attach_secondary: version mismatch "
                "(found {}, expected {})",
                hdr->version, queue_allocator_impl::Header::kVersion);
            return std::unexpected(std::string{
                "QueueAllocator::attach_secondary: header version mismatch"});
        }
        QueueAllocator a;
        a.hdr_          = hdr;
        a.mz_           = mz;
        a.owns_memzone_ = false;   // diagnostic-attach does not free
        return a;
    }

    /// @brief Greedy first-fit: claim `count` contiguous free queues.
    /// Returns the granted `QueueRange` on success, or an error string
    /// on `count == 0` / pool exhausted.
    ///
    /// Generation counter is bumped on success, so the returned
    /// `range.generation` is unique among every claim ever served by
    /// this allocator instance.
    ///
    /// CAS-equivalent serialization is provided by the pthread mutex
    /// inside the Header — multiple concurrent IPC handler invocations
    /// are serialized atomically.
    [[nodiscard]] std::expected<QueueRange, std::string>
    claim(uint16_t count) noexcept {
        if (hdr_ == nullptr) {
            return std::unexpected(std::string{
                "QueueAllocator::claim: not initialized"});
        }
        if (count == 0) {
            return std::unexpected(std::string{
                "QueueAllocator::claim: count must be >= 1"});
        }
        if (count > hdr_->total_queues) {
            SPDLOG_DEBUG(
                "QueueAllocator::claim: count={} > total_queues={} — "
                "QueuePoolExhausted (request larger than pool capacity)",
                count, hdr_->total_queues);
            return std::unexpected(std::string{"QueuePoolExhausted"});
        }

        pthread_mutex_lock(&hdr_->mutex);
        // Greedy first-fit scan. On the first run-end check we either
        // accept or restart at i. The bitmap scan is O(total_queues);
        // capped at 256 it's a few cache-line reads.
        const uint16_t total = hdr_->total_queues;
        uint16_t run_start   = 0;
        uint16_t run_len     = 0;
        for (uint16_t i = 0; i < total; ++i) {
            if (queue_allocator_impl::bit_is_set(*hdr_, i)) {
                run_start = static_cast<uint16_t>(i + 1);
                run_len   = 0;
                continue;
            }
            ++run_len;
            if (run_len >= count) {
                // Claim [run_start, run_start + count).
                const uint16_t lo = run_start;
                const uint16_t hi = static_cast<uint16_t>(run_start + count);
                const uint64_t gen = hdr_->generation.fetch_add(
                    1, std::memory_order_acq_rel) + 1;
                for (uint16_t j = lo; j < hi; ++j) {
                    queue_allocator_impl::set_bit(*hdr_, j);
                    // ABA-safe: stamp every claimed queue with the
                    // generation we just minted. release() rejects any
                    // mismatch.
                    hdr_->claim_gen[j] = gen;
                }
                pthread_mutex_unlock(&hdr_->mutex);
                SPDLOG_INFO(
                    "QueueAllocator::claim: granted [{}, {}) gen={} "
                    "(count={}, pool={}/{} now free)",
                    lo, hi, gen, count,
                    free_queues_unlocked_(),
                    total);
                return QueueRange{lo, hi, gen};
            }
        }
        pthread_mutex_unlock(&hdr_->mutex);
        SPDLOG_WARN(
            "QueueAllocator::claim: pool exhausted (count={}, total={}, "
            "free={}, largest_run={}) — QueuePoolExhausted",
            count, total, free_queues_unlocked_(), largest_free_run_unlocked_());
        return std::unexpected(std::string{"QueuePoolExhausted"});
    }

    /// @brief Release a previously-claimed range. Idempotent.
    /// Verifies that ALL bits in `range` are still set AND the
    /// allocator's generation has moved past `range.generation` by
    /// at most the number of claims since this one — but rather
    /// than tracking per-bit generation, we use a simpler invariant:
    /// the released range MUST have all bits currently claimed; a
    /// stale release whose queues have already been reclaimed by
    /// someone else would find at least some bits in the range
    /// unset (because at least one claim freed the slot in between).
    ///
    /// The generation field is checked for monotonicity to detect
    /// pathological cases (release with gen > current gen = wire
    /// corruption); on mismatch we increment `stale_releases` and
    /// silently drop.
    void release(QueueRange range) noexcept {
        if (hdr_ == nullptr) {
            SPDLOG_DEBUG(
                "QueueAllocator::release: not initialized — silent no-op "
                "(range=[{},{}) gen={})",
                range.lo, range.hi, range.generation);
            return;
        }
        if (range.empty()) {
            SPDLOG_DEBUG(
                "QueueAllocator::release: empty range — silent no-op "
                "(lo={} hi={} gen={})", range.lo, range.hi, range.generation);
            return;
        }
        if (range.hi > hdr_->total_queues) {
            SPDLOG_WARN(
                "QueueAllocator::release: range.hi={} > total_queues={} — "
                "rejecting (likely cross-version IPC mismatch)",
                range.hi, hdr_->total_queues);
            hdr_->stale_releases.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const uint64_t now_gen = hdr_->generation.load(std::memory_order_acquire);
        if (range.generation > now_gen) {
            SPDLOG_WARN(
                "QueueAllocator::release: caller generation={} > current={} "
                "— wire corruption / cross-version IPC mismatch, rejecting",
                range.generation, now_gen);
            hdr_->stale_releases.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        pthread_mutex_lock(&hdr_->mutex);
        // Stale-detection invariants (BOTH must hold):
        //   1. Every queue in the range must currently be claimed
        //      (bit set). Catches the simple "double-release" case.
        //   2. Every queue's `claim_gen[j]` must equal the caller's
        //      `range.generation`. Catches the ABA case where the
        //      same range was freed-and-reclaimed by a different
        //      secondary; the new claim mints a fresh generation,
        //      so the old caller's release sees a mismatch and
        //      bounces.
        bool stale = false;
        for (uint16_t j = range.lo; j < range.hi; ++j) {
            if (!queue_allocator_impl::bit_is_set(*hdr_, j)) {
                stale = true; break;
            }
            if (hdr_->claim_gen[j] != range.generation) {
                stale = true; break;
            }
        }
        if (stale) {
            pthread_mutex_unlock(&hdr_->mutex);
            hdr_->stale_releases.fetch_add(1, std::memory_order_relaxed);
            SPDLOG_DEBUG(
                "QueueAllocator::release: stale release rejected — "
                "range=[{},{}) gen={} (bit cleared or claim_gen "
                "mismatch — likely an ABA recycle)",
                range.lo, range.hi, range.generation);
            return;
        }
        for (uint16_t j = range.lo; j < range.hi; ++j) {
            queue_allocator_impl::clear_bit(*hdr_, j);
            // Leave claim_gen[j] as the last-claimed generation; a
            // future re-claim overwrites it. Zeroing it would make a
            // legitimate "claim, release, claim-again" indistinguishable
            // from a stale release for the second claim.
        }
        pthread_mutex_unlock(&hdr_->mutex);
        SPDLOG_INFO(
            "QueueAllocator::release: freed [{}, {}) gen={} "
            "(pool {}/{} now claimed)",
            range.lo, range.hi, range.generation,
            hdr_->total_queues - free_queues_unlocked_(),
            hdr_->total_queues);
    }

    /// @brief Bitmap of currently-claimed queues. Bit `i` set ⇔ queue
    /// `i` is claimed. Cold getter; takes a snapshot under the mutex.
    [[nodiscard]] uint64_t owned_queues_bitmap() const noexcept {
        if (hdr_ == nullptr) return 0;
        // Cap the readout at 64 bits to fit the signature; callers that
        // need >64 queues should use `dump()`. Most HFT NICs (ENA, CX-5)
        // are well within 64.
        pthread_mutex_lock(&hdr_->mutex);
        const uint64_t snap = hdr_->bitmap[0];
        pthread_mutex_unlock(&hdr_->mutex);
        return snap;
    }

    /// @brief Diagnostic snapshot of pool state for eph-nicctl.
    struct PoolState {
        uint16_t total              = 0;
        uint16_t free               = 0;
        uint16_t largest_free_run   = 0;
        uint64_t generation         = 0;
        uint64_t stale_releases     = 0;
    };

    [[nodiscard]] PoolState dump() const noexcept {
        if (hdr_ == nullptr) return {};
        pthread_mutex_lock(&hdr_->mutex);
        PoolState s;
        s.total            = hdr_->total_queues;
        s.free             = free_queues_unlocked_();
        s.largest_free_run = largest_free_run_unlocked_();
        s.generation       = hdr_->generation.load(std::memory_order_relaxed);
        s.stale_releases   = hdr_->stale_releases.load(std::memory_order_relaxed);
        pthread_mutex_unlock(&hdr_->mutex);
        return s;
    }

    /// @brief Internal-only access to the underlying Header. Used by
    /// the IPC handler thunk (which already serializes via DPDK's
    /// per-action mutex) to skip the expense of a doubled lock.
    /// Returns nullptr on a moved-from instance.
    [[nodiscard]] queue_allocator_impl::Header* header_() noexcept { return hdr_; }
    [[nodiscard]] const queue_allocator_impl::Header* header_() const noexcept { return hdr_; }

private:
    void release_memzone_() noexcept {
        if (owns_memzone_ && mz_ != nullptr) {
            // Destroy the mutex first so a spurious cross-process holder
            // doesn't dangle on a now-freed memzone. pthread_mutex_destroy
            // is a no-op on a healthy-state mutex; OK to ignore retval.
            if (hdr_ != nullptr) {
                (void)pthread_mutex_destroy(&hdr_->mutex);
            }
            const int rc = rte_memzone_free(mz_);
            if (rc != 0) {
                SPDLOG_WARN(
                    "QueueAllocator: rte_memzone_free failed (rc={}, "
                    "rte_errno={}) — memzone may leak until next daemon "
                    "restart",
                    rc, rte_errno);
            }
        }
        hdr_           = nullptr;
        mz_            = nullptr;
        owns_memzone_  = false;
    }

    /// @brief Unlocked count of free queues. MUST be called with the
    /// header mutex already held.
    [[nodiscard]] uint16_t free_queues_unlocked_() const noexcept {
        if (hdr_ == nullptr) return 0;
        uint16_t free_count = 0;
        for (uint16_t i = 0; i < hdr_->total_queues; ++i) {
            if (!queue_allocator_impl::bit_is_set(*hdr_, i)) ++free_count;
        }
        return free_count;
    }

    /// @brief Unlocked length of the longest contiguous free run.
    /// MUST be called with the header mutex held.
    [[nodiscard]] uint16_t largest_free_run_unlocked_() const noexcept {
        if (hdr_ == nullptr) return 0;
        uint16_t best = 0;
        uint16_t cur  = 0;
        for (uint16_t i = 0; i < hdr_->total_queues; ++i) {
            if (queue_allocator_impl::bit_is_set(*hdr_, i)) {
                cur = 0;
            } else {
                ++cur;
                if (cur > best) best = cur;
            }
        }
        return best;
    }

    queue_allocator_impl::Header* hdr_           = nullptr;
    const struct rte_memzone*     mz_            = nullptr;
    bool                          owns_memzone_  = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// IPC payload structs and handler glue
// ─────────────────────────────────────────────────────────────────────────────
//
// Wire-format types: both `QueueClaimRequest` and `QueueClaimReply`
// must be trivially copyable (rte_mp_msg payload is `uint8_t[256]`).
// Versioned via the leading `version` byte so a future wire-format
// shift can be detected and rejected up-front rather than silently
// misinterpreting bytes.

inline constexpr std::string_view kQueueClaimActionName   = "eph_queue_claim";
inline constexpr std::string_view kQueueReleaseActionName = "eph_queue_release";

/// @brief Wire format for `eph_queue_claim` request — secondary →
/// daemon. `requester_pid` is diagnostic only (logged on grant /
/// reject).
struct alignas(8) QueueClaimRequest {
    uint8_t  version;
    uint8_t  reserved0;
    uint16_t count;
    int32_t  requester_pid;
};
static_assert(std::is_trivially_copyable_v<QueueClaimRequest>);
static_assert(sizeof(QueueClaimRequest) == 8);

/// @brief Wire format for `eph_queue_claim` reply — daemon → secondary.
/// `error[]` is non-empty iff `ok == 0`; on failure the secondary
/// surfaces the string verbatim back to the user.
struct alignas(8) QueueClaimReply {
    uint8_t  version;
    uint8_t  ok;            ///< 1 = success, 0 = failed
    uint16_t lo;
    uint16_t hi;
    uint16_t reserved0;
    uint64_t generation;
    char     error[64];     ///< NUL-padded; empty on success
};
static_assert(std::is_trivially_copyable_v<QueueClaimReply>);

/// @brief Wire format for `eph_queue_release` — secondary → daemon.
/// Fire-and-forget, no reply. Carries the granted range + generation
/// so daemon can reject stale releases.
struct alignas(8) QueueReleaseRequest {
    uint8_t  version;
    uint8_t  reserved0;
    uint16_t lo;
    uint16_t hi;
    uint16_t reserved1;
    uint64_t generation;
    int32_t  requester_pid;
    uint32_t reserved2;
};
static_assert(std::is_trivially_copyable_v<QueueReleaseRequest>);

/// @brief Process-level pointer to the daemon's QueueAllocator.
/// Set by `Platform::serve_nic` before the IPC actions register;
/// cleared on daemon shutdown. The action thunks load via this
/// global so they don't need access to the Platform pimpl.
inline std::atomic<QueueAllocator*> g_active_queue_allocator{nullptr};

/// @brief Process-level pointer to the daemon's port_id, used by the
/// claim handler to refresh RETA on every grant/release. Set by
/// `Platform::serve_nic`; nullopt-equivalent value 0xFFFF means
/// "RETA refresh inactive (e.g. virtual PMD test mode)".
inline std::atomic<uint16_t> g_active_qalloc_port_id{0xFFFF};

/// @brief Refresh the NIC's RETA so RSS hash buckets only point at
/// currently-claimed queues. Called by the claim/release handlers
/// after a successful state transition.
///
/// Algorithm:
///   * Compute the current set of claimed queues from the bitmap.
///   * If the set is empty, point all RETA buckets at queue 0
///     (the "sink queue" — daemon doesn't poll it, packets accumulate
///     until a peer attaches; NIC drops on overflow as a backstop).
///   * Otherwise round-robin all RETA buckets across the claimed
///     queue set so RSS spreads evenly across attached peers.
///
/// Returns `true` on success or on a benign `-ENOTSUP` from the PMD
/// (virtual / null PMD on test path); `false` only on hard failure.
/// Hard failure is logged but does not abort the IPC handler — the
/// allocator state stays consistent and the next claim/release just
/// retries.
[[nodiscard]] inline bool
refresh_reta_for_claimed_(QueueAllocator& alloc, uint16_t port_id) noexcept;

/// @brief DPDK rte_mp_t handler for `eph_queue_claim`. Validates
/// payload, calls `claim`, refreshes RETA, replies.
inline int
on_queue_claim_thunk(const struct rte_mp_msg* msg,
                     const void*              peer);

/// @brief DPDK rte_mp_t handler for `eph_queue_release`. Validates
/// payload, calls `release`, refreshes RETA. No reply (one-way).
inline int
on_queue_release_thunk(const struct rte_mp_msg* msg,
                       const void*              peer);

} // namespace eph::dpdk::detail


// ─────────────────────────────────────────────────────────────────────────────
// IPC handler implementations — defined out-of-class to keep the public
// QueueAllocator surface above small. Implementations need
// `mp_ipc.hpp`'s pack/parse helpers, included here.
// ─────────────────────────────────────────────────────────────────────────────

#include <rte_ethdev.h>

#include "eph/dpdk/detail/mp_ipc.hpp"

namespace eph::dpdk::detail {

[[nodiscard]] inline bool
refresh_reta_for_claimed_(QueueAllocator& alloc, uint16_t port_id) noexcept {
    if (port_id == 0xFFFF) {
        SPDLOG_DEBUG("refresh_reta_for_claimed_: port_id sentinel — skip "
                     "(test / virtual PMD mode)");
        return true;
    }
    rte_eth_dev_info dev_info{};
    int rc = rte_eth_dev_info_get(port_id, &dev_info);
    if (rc != 0) {
        SPDLOG_DEBUG("refresh_reta_for_claimed_: rte_eth_dev_info_get(port={}) "
                     "rc={} — skip RETA refresh (PMD may not support it from "
                     "this proc; allocator state still consistent)",
                     port_id, rc);
        return true;   // benign — same degrade-on-failure as configure_rss
    }
    uint16_t reta_size = dev_info.reta_size;
    if (reta_size == 0) reta_size = 128;  // common default
    reta_size = std::min(
        reta_size, static_cast<uint16_t>(RTE_ETH_RSS_RETA_SIZE_512));

    // Build the list of currently-claimed queue ids.
    std::array<uint16_t, kMaxAllocatorQueues> claimed{};
    uint16_t                                  nclaimed = 0;
    {
        const auto* hdr = alloc.header_();
        if (hdr == nullptr) return false;
        for (uint16_t i = 0; i < hdr->total_queues; ++i) {
            if (queue_allocator_impl::bit_is_set(*hdr, i)) {
                claimed[nclaimed++] = i;
            }
        }
    }

    rte_eth_rss_reta_entry64
        reta[RTE_ETH_RSS_RETA_SIZE_512 / RTE_ETH_RETA_GROUP_SIZE]{};
    for (uint16_t i = 0; i < reta_size; ++i) {
        const uint16_t group = i / RTE_ETH_RETA_GROUP_SIZE;
        const uint16_t bit   = i % RTE_ETH_RETA_GROUP_SIZE;
        reta[group].mask |= (1ULL << bit);
        // Empty claimed set → all buckets point at queue 0 (sink queue).
        // Otherwise round-robin across claimed queues.
        const uint16_t target = (nclaimed == 0)
            ? 0
            : claimed[i % nclaimed];
        reta[group].reta[bit] = target;
    }
    rc = rte_eth_dev_rss_reta_update(port_id, reta, reta_size);
    if (rc == -ENOTSUP || rc == -EOPNOTSUPP) {
        SPDLOG_WARN(
            "refresh_reta_for_claimed_: rte_eth_dev_rss_reta_update returned "
            "ENOTSUP on port={} — PMD does not support runtime RETA changes; "
            "RSS distribution stays at the bring-up configuration. Allocator "
            "remains usable; pool exhaustion + claim/release still works.",
            port_id);
        return true;
    }
    if (rc != 0) {
        SPDLOG_ERROR(
            "refresh_reta_for_claimed_: rte_eth_dev_rss_reta_update(port={}, "
            "reta_size={}) rc={} (rte_errno={}: {}) — RSS distribution may be "
            "stale, but allocator state stays consistent",
            port_id, reta_size, rc, rte_errno, rte_strerror(rte_errno));
        return false;
    }
    SPDLOG_DEBUG(
        "refresh_reta_for_claimed_: port={} reta_size={} nclaimed={} "
        "(buckets now point at {})",
        port_id, reta_size, nclaimed,
        nclaimed == 0 ? "queue 0 (sink)" : "claimed-set round-robin");
    return true;
}

inline int
on_queue_claim_thunk(const struct rte_mp_msg* msg,
                     const void*              peer) {
    auto* alloc = g_active_queue_allocator.load(std::memory_order_acquire);
    QueueClaimReply reply{};
    reply.version = 1;
    reply.ok      = 0;

    if (alloc == nullptr) {
        std::strncpy(reply.error, "daemon not ready (allocator inactive)",
                     sizeof(reply.error) - 1);
        SPDLOG_ERROR(
            "on_queue_claim_thunk: g_active_queue_allocator is null — "
            "daemon shutdown race? Replying with error.");
        (void)mp_ipc_reply_send(kQueueClaimActionName, reply, peer);
        return 0;
    }
    auto parsed = parse_payload<QueueClaimRequest>(msg);
    if (!parsed) {
        std::strncpy(reply.error, "invalid claim payload",
                     sizeof(reply.error) - 1);
        (void)mp_ipc_reply_send(kQueueClaimActionName, reply, peer);
        return 0;
    }
    const auto& req = *parsed;
    if (req.version != 1) {
        SPDLOG_ERROR(
            "on_queue_claim_thunk: version={} unsupported (expected 1) "
            "— rejecting", req.version);
        std::strncpy(reply.error, "unsupported claim wire version",
                     sizeof(reply.error) - 1);
        (void)mp_ipc_reply_send(kQueueClaimActionName, reply, peer);
        return 0;
    }

    auto claim_r = alloc->claim(req.count);
    if (!claim_r) {
        const std::string& err = claim_r.error();
        // Truncate to fit the wire field; the secondary's caller sees
        // either the full error or "QueuePoolExhausted" for that exact
        // case.
        std::strncpy(reply.error, err.c_str(), sizeof(reply.error) - 1);
        SPDLOG_INFO(
            "on_queue_claim_thunk: claim(count={} requester_pid={}) "
            "rejected: {}", req.count, req.requester_pid, err);
        (void)mp_ipc_reply_send(kQueueClaimActionName, reply, peer);
        return 0;
    }

    reply.ok         = 1;
    reply.lo         = claim_r->lo;
    reply.hi         = claim_r->hi;
    reply.generation = claim_r->generation;

    // RETA refresh after successful claim. Best-effort — refresh
    // failure is logged but the claim still succeeds (RSS distribution
    // may be stale until the next claim/release rebuilds it).
    const uint16_t port_id =
        g_active_qalloc_port_id.load(std::memory_order_acquire);
    (void)refresh_reta_for_claimed_(*alloc, port_id);

    SPDLOG_INFO(
        "on_queue_claim_thunk: granted range=[{}, {}) gen={} to pid={}",
        reply.lo, reply.hi, reply.generation, req.requester_pid);
    auto sr = mp_ipc_reply_send(kQueueClaimActionName, reply, peer);
    if (!sr) {
        // The grant is committed in the bitmap; the secondary will time
        // out, retry, or surface a failure. Roll the claim back so the
        // pool isn't permanently leaked.
        SPDLOG_ERROR(
            "on_queue_claim_thunk: reply send failed: {} — rolling back "
            "claim [{}, {}) to avoid pool leak",
            sr.error().detail, reply.lo, reply.hi);
        alloc->release(QueueRange{reply.lo, reply.hi, reply.generation});
        (void)refresh_reta_for_claimed_(*alloc, port_id);
    }
    return 0;
}

inline int
on_queue_release_thunk(const struct rte_mp_msg* msg,
                       const void*              /*peer*/) {
    auto* alloc = g_active_queue_allocator.load(std::memory_order_acquire);
    if (alloc == nullptr) {
        SPDLOG_DEBUG(
            "on_queue_release_thunk: allocator inactive — drop release msg");
        return 0;
    }
    auto parsed = parse_payload<QueueReleaseRequest>(msg);
    if (!parsed) {
        SPDLOG_ERROR(
            "on_queue_release_thunk: parse_payload failed — drop msg");
        return 0;
    }
    const auto& req = *parsed;
    if (req.version != 1) {
        SPDLOG_ERROR(
            "on_queue_release_thunk: version={} unsupported (expected 1) "
            "— drop msg", req.version);
        return 0;
    }
    QueueRange range{req.lo, req.hi, req.generation};
    SPDLOG_DEBUG(
        "on_queue_release_thunk: release request range=[{}, {}) gen={} "
        "from pid={}", range.lo, range.hi, range.generation,
        req.requester_pid);
    alloc->release(range);
    const uint16_t port_id =
        g_active_qalloc_port_id.load(std::memory_order_acquire);
    (void)refresh_reta_for_claimed_(*alloc, port_id);
    return 0;
}

} // namespace eph::dpdk::detail
