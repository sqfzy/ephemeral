#pragma once

/// @file detail/mp_registry.hpp
/// Cross-process MP registry — a POD-only header laid out in a DPDK
/// hugepage memzone, attached by every process that shares a NIC.
///
/// Purpose: turn the multi-process consensus burden ("which queue
/// range / src_port range does THIS process own?") from a contract
/// the user has to enforce by hand into a contract the library
/// validates at `Platform::create_*` time. The user declares the
/// full topology (`MpTopology` — every process's queue/port slot)
/// once on each peer; the primary writes that topology into the
/// memzone, and every secondary that attaches cross-validates its
/// declared spec against the primary's view.
///
/// Restart contract (Q4 of the reshape decision): primary always
/// resets the registry on `create_primary` (writes a fresh header
/// + zeroed `procs[].claimed`). The user contract is "stop every
/// secondary before restarting the primary" — same rule DPDK
/// already imposes on the shared mempool. No epoch / generation
/// counter; if a stale secondary survives a primary restart it
/// will see fresh slots and CAS-claim into them, which is the
/// well-defined "secondary that started before primary, then primary
/// restarted" failure surface — diagnosable from logs.
///
/// Synchronization: `std::atomic<uint8_t>` lock-free (verified by
/// static_assert) is the only synchronization primitive in the
/// memzone. Cross-process `std::atomic<T>` for lock-free trivially-
/// copyable T is well-defined as long as both processes use the same
/// ABI — which is guaranteed since they're built from the same
/// `eph-net-dpdk` header by the same toolchain.
///
/// Hot path: NONE. Every operation here is on the cold
/// `Platform::create_*` path or the `~Platform` teardown path, called
/// at most once per process lifecycle.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string_view>
#include <type_traits>
#include <utility>

#include <spdlog/spdlog.h>

#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_lcore.h>
#include <rte_memzone.h>

#include "eph/core/error.hpp"
#include "eph/dpdk/mp_topology.hpp"

namespace eph::dpdk::detail {

// ─────────────────────────────────────────────────────────────────────────────
// Memzone naming
// ─────────────────────────────────────────────────────────────────────────────
//
// DPDK caps memzone names at `RTE_MEMZONE_NAMESIZE` (32 bytes including
// the trailing '\0'). We reserve the prefix `eph_mp/` (7 bytes), leaving
// 32 - 7 - 1 = 24 bytes for the user-supplied `file_prefix`. Anything
// longer is rejected up-front with `Error::InvalidConfig` so the user
// gets a clear diagnostic instead of a silent strncpy truncation that
// would let primary and secondary disagree on the memzone name.

inline constexpr std::string_view kMpRegistryNamePrefix = "eph_mp/";
inline constexpr size_t kMpRegistryNameCap = 32;  // RTE_MEMZONE_NAMESIZE
inline constexpr size_t kMpRegistryFilePrefixMax =
    kMpRegistryNameCap - kMpRegistryNamePrefix.size() - 1;  // 24

// 4-byte magic written at offset 0 of every registry. Spelled as the
// ASCII bytes 'E','M','P','R' in memory order so a hex dump of the
// memzone shows `45 4D 50 52  ..` and is greppable.
inline constexpr uint32_t kMpRegistryMagic =
    (static_cast<uint32_t>('E') << 0)  |
    (static_cast<uint32_t>('M') << 8)  |
    (static_cast<uint32_t>('P') << 16) |
    (static_cast<uint32_t>('R') << 24);

/// Schema version. Bumped when ProcSlot layout changes in a way
/// that would corrupt cross-process reads if mixed with an older
/// version. Hard-rejected at attach time (no compat layer); peers
/// must rebuild from the same eph-net-dpdk source.
///
/// History:
///   v1: claimed/tag/queue_lo/queue_hi/port_lo/port_hi
///   v2: + lcore_mask (uint64_t) — cross-process lcore conflict
///       detection, fix for "two procs same lcore silently steals
///       CPU" mental model gap.
inline constexpr uint32_t kMpRegistryVersion = 2;

inline constexpr size_t kMpRegistryTagCap = 32;

/// Sentinel for `MpRegistryHandle::self_index_` meaning "this handle
/// does not own a process slot". Used by `attach_secondary_readonly`
/// (look-only handle) and as the default for moved-from / inert
/// instances. Distinct from any valid index because the registry caps
/// at `MpTopology::kMaxProcs == 64` so 0xFF is unreachable.
inline constexpr uint8_t kMpRegistrySelfIndexUnset = 0xFF;

// ─────────────────────────────────────────────────────────────────────────────
// POD layout — must remain trivially copyable + standard layout
// ─────────────────────────────────────────────────────────────────────────────
//
// `ProcSlot` and `MpRegistryHeader` are stored directly in shared
// hugepage memory and accessed by multiple processes. Both must be
// trivially copyable (no implicit construction side effects) and have
// stable layout across the build tree. Asserts at the bottom of the
// header pin both contracts at compile time — any future field
// reorder / type change will fail to compile rather than silently
// shifting the cross-process layout.

struct ProcSlot {
    /// 0 = free, 1 = claimed by an active process. Only the owner
    /// process writes to its own slot (via CAS to claim, plain store
    /// to release on graceful shutdown). Cross-process readers do
    /// `load(memory_order_acquire)`.
    std::atomic<uint8_t> claimed;

    /// Human-readable label copied from `ProcSpec::tag` for debug /
    /// dump output. Always null-terminated.
    char tag[kMpRegistryTagCap];

    uint16_t queue_lo;
    uint16_t queue_hi;
    uint32_t port_lo;
    uint32_t port_hi;
    /// Bitmask mirror of `ProcSpec::lcore_mask`. v2 schema.
    /// Used by `attach_secondary` to reject overlapping lcore
    /// declarations — two procs claiming bit N would silently steal
    /// CPU from each other under the OS scheduler.
    uint64_t lcore_mask;
};

struct alignas(64) MpRegistryHeader {
    uint32_t magic;
    uint32_t version;
    /// Number of populated `procs[]` entries declared by the primary.
    /// Caps at `MpTopology::kMaxProcs`. Secondaries verify their own
    /// `topo.procs.size() == header.total_procs`.
    uint32_t total_procs;
    /// Padding so `file_prefix` starts on an 8-byte boundary.
    uint32_t _pad0;
    /// Null-terminated copy of `PlatformConfig::file_prefix`. Carried
    /// in the header so secondaries can sanity-check that they're
    /// looking at the right primary (a name collision would be a
    /// configuration bug, not a runtime hazard, but easier to spot
    /// here than via "queue range mismatch").
    char file_prefix[kMpRegistryFilePrefixMax];

    std::array<ProcSlot, MpTopology::kMaxProcs> procs;
};

static_assert(std::atomic<uint8_t>::is_always_lock_free,
              "ProcSlot::claimed must be lock-free for cross-process atomics");
static_assert(std::is_trivially_copyable_v<ProcSlot>,
              "ProcSlot must be trivially copyable for memzone storage");
static_assert(std::is_trivially_copyable_v<MpRegistryHeader>,
              "MpRegistryHeader must be trivially copyable for memzone storage");
static_assert(alignof(MpRegistryHeader) >= 64,
              "MpRegistryHeader requires cacheline alignment");

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Compose the memzone name `eph_mp/<file_prefix>`. Returns
/// `InvalidConfig` if `file_prefix` is empty or is `kMpRegistryFilePrefixMax`
/// bytes or longer. The strict-`>=` check (rather than `>`) reserves the
/// final byte of `MpRegistryHeader::file_prefix[kMpRegistryFilePrefixMax]`
/// for the NUL terminator that `init_mp_registry_header` and the strncmp
/// in `attach_secondary*` rely on — accepting `kMpRegistryFilePrefixMax`
/// bytes here would let `init_*` silently truncate the last byte and let
/// two distinct prefixes that share the first `kMpRegistryFilePrefixMax-1`
/// bytes hash-collide on strncmp.
[[nodiscard]] inline std::expected<std::array<char, kMpRegistryNameCap>,
                                   core::ErrorInfo>
build_mp_registry_name(std::string_view file_prefix) noexcept {
    if (file_prefix.empty()) {
        SPDLOG_ERROR(
            "MpRegistry::build_name: file_prefix is empty — "
            "PlatformConfig.file_prefix must be set for MP-IPC");
        return std::unexpected(core::ErrorInfo{
            core::Error::InvalidConfig,
            "MpRegistry: file_prefix must be non-empty"});
    }
    if (file_prefix.size() >= kMpRegistryFilePrefixMax) {
        SPDLOG_ERROR(
            "MpRegistry::build_name: file_prefix='{}' size={} >= max {} "
            "(RTE_MEMZONE_NAMESIZE - len(\"eph_mp/\") - 1; final byte "
            "reserved for header NUL)",
            file_prefix, file_prefix.size(), kMpRegistryFilePrefixMax);
        return std::unexpected(core::ErrorInfo{
            core::Error::InvalidConfig,
            "MpRegistry: file_prefix must be < 24 bytes "
            "(RTE_MEMZONE_NAMESIZE - len(\"eph_mp/\") - 1; the final "
            "byte is reserved for the header NUL terminator)"});
    }

    std::array<char, kMpRegistryNameCap> buf{};
    std::memcpy(buf.data(), kMpRegistryNamePrefix.data(),
                kMpRegistryNamePrefix.size());
    std::memcpy(buf.data() + kMpRegistryNamePrefix.size(),
                file_prefix.data(), file_prefix.size());
    // remaining bytes (incl. trailing NUL) are already zero-initialised
    return buf;
}

/// @brief Initialize a fresh registry header in `dst` from `topo` +
/// `file_prefix`. Caller has already established `dst` points at a
/// freshly reserved memzone of >= sizeof(MpRegistryHeader) bytes.
inline void
init_mp_registry_header(MpRegistryHeader* dst,
                        std::string_view file_prefix,
                        MpTopology const& topo) noexcept {
    // Wipe slot before writing — the memzone may have residue from a
    // previous run if DPDK retained the hugepage segment. We can't use
    // memset on the whole struct because `std::atomic<uint8_t>` is not
    // trivially default-constructible in some libstdc++ debug builds;
    // explicit field assignment keeps us within well-defined territory.
    dst->magic        = kMpRegistryMagic;
    dst->version      = kMpRegistryVersion;
    dst->total_procs  = topo.total_procs;
    dst->_pad0        = 0;
    std::memset(dst->file_prefix, 0, kMpRegistryFilePrefixMax);
    std::memcpy(dst->file_prefix, file_prefix.data(),
                std::min(file_prefix.size(), kMpRegistryFilePrefixMax - 1));

    for (size_t i = 0; i < dst->procs.size(); ++i) {
        ProcSlot& s = dst->procs[i];
        s.claimed.store(0, std::memory_order_relaxed);
        std::memset(s.tag, 0, kMpRegistryTagCap);
        s.queue_lo   = 0;
        s.queue_hi   = 0;
        s.port_lo    = 0;
        s.port_hi    = 0;
        s.lcore_mask = 0;
    }

    for (uint8_t i = 0; i < topo.total_procs; ++i) {
        ProcSlot& s = dst->procs[i];
        const auto& src = topo.procs[i];
        std::memcpy(s.tag, src.tag.data(),
                    std::min(src.tag.size(), kMpRegistryTagCap - 1));
        s.queue_lo   = src.queue_lo;
        s.queue_hi   = src.queue_hi;
        s.port_lo    = src.port_lo;
        s.port_hi    = src.port_hi;
        s.lcore_mask = src.lcore_mask;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// MpRegistryHandle — RAII owner of a registry attachment
// ─────────────────────────────────────────────────────────────────────────────
//
// One handle per `Platform` instance:
//   - primary: handle owns the memzone (frees on destructor)
//   - secondary: handle is a non-owning attachment; destructor only
//     clears `procs[self_index].claimed`
//
// Move-only. Moved-from instances are inert: `bool(handle) == false`
// and the destructor is a no-op.

class MpRegistryHandle {
public:
    MpRegistryHandle() noexcept = default;

    MpRegistryHandle(const MpRegistryHandle&) = delete;
    MpRegistryHandle& operator=(const MpRegistryHandle&) = delete;

    MpRegistryHandle(MpRegistryHandle&& o) noexcept
        : hdr_(o.hdr_), mz_(o.mz_),
          owns_memzone_(o.owns_memzone_),
          self_index_(o.self_index_) {
        o.reset_();
    }

    MpRegistryHandle& operator=(MpRegistryHandle&& o) noexcept {
        if (this != &o) {
            release_();
            hdr_          = o.hdr_;
            mz_           = o.mz_;
            owns_memzone_ = o.owns_memzone_;
            self_index_   = o.self_index_;
            o.reset_();
        }
        return *this;
    }

    ~MpRegistryHandle() { release_(); }

    [[nodiscard]] explicit operator bool() const noexcept {
        return hdr_ != nullptr;
    }

    [[nodiscard]] MpRegistryHeader const* header() const noexcept {
        return hdr_;
    }
    [[nodiscard]] uint8_t self_index() const noexcept { return self_index_; }
    [[nodiscard]] ProcSlot const& self() const noexcept {
        return hdr_->procs[self_index_];
    }
    /// @brief True iff this handle owns a claimed slot (`self_index_`
    /// is a valid index, not the unset sentinel). Read-only handles
    /// returned by `attach_secondary_readonly` report `false` until a
    /// successful `try_claim_free_slot()` upgrades them.
    [[nodiscard]] bool owns_slot() const noexcept {
        return hdr_ != nullptr && self_index_ != kMpRegistrySelfIndexUnset;
    }

    /// @brief Lock-free count of currently-claimed slots in this
    /// registry (i.e. live processes attached to it). Scans
    /// `procs[0..total_procs)` with `memory_order_acquire`. Returns
    /// 0 for an inert handle (`hdr_ == nullptr`).
    ///
    /// **Race window**: a peer may CAS-claim a slot or release one
    /// during the scan, so the result is a snapshot rather than a
    /// strict invariant. For the primary-teardown gate this is
    /// acceptable: if a peer releases its slot mid-scan, that peer
    /// is already past its own `~Platform::Impl::cleanup()` and no
    /// longer touches shared io_cq state — even if we incorrectly
    /// conclude "I'm last alive" and proceed to `rte_eth_dev_stop`,
    /// the released peer cannot fault on the resulting NULL writes
    /// (it has no live `rte_eth_rx_burst` in flight).
    [[nodiscard]] uint32_t count_alive_procs() const noexcept {
        if (hdr_ == nullptr) return 0;
        const uint32_t total = hdr_->total_procs;
        uint32_t alive = 0;
        for (uint32_t i = 0; i < total; ++i) {
            if (hdr_->procs[i].claimed.load(
                    std::memory_order_acquire) != 0) {
                ++alive;
            }
        }
        return alive;
    }

    /// @brief True iff this handle owns the only currently-claimed
    /// slot. Equivalent to `owns_slot() && count_alive_procs() == 1`.
    ///
    /// Used by `Platform::Impl::cleanup()` (primary path) to gate
    /// `rte_eth_dev_stop` / `rte_eth_dev_close` / `rte_mempool_free`:
    /// if other peers still hold slots, the primary defers global
    /// port teardown so the still-attached peers don't fault on
    /// shared io_cq nullification (DPDK MP teardown protocol —
    /// the port is system-owned, primary destruction ≠ everyone
    /// detached).
    [[nodiscard]] bool is_last_alive_proc() const noexcept {
        return owns_slot() && count_alive_procs() == 1;
    }

    /// @brief Drop slot-ownership without clearing the slot's
    /// `claimed` flag in shared memory. After this call,
    /// `owns_slot()` is `false` and the destructor will NOT
    /// release the slot. Used by the autojoin path to transfer
    /// ownership from the read-only handle (which CAS-claimed the
    /// slot via `try_claim_free_slot`) to the full handle returned
    /// by `attach_secondary(..., already_claimed=true)`. The slot
    /// stays in `claimed=1` state across the transition so no
    /// other peer can race-claim it; the new handle assumes
    /// teardown responsibility.
    void disarm_slot() noexcept {
        self_index_ = kMpRegistrySelfIndexUnset;
    }

    // ── Factories ────────────────────────────────────────────────────────────

    /// @brief Reserve (or replace) the registry memzone, write a fresh
    /// header from `topo`, and CAS-claim `procs[topo.self_index]`.
    /// Called by `Platform::create_primary`.
    [[nodiscard]] static std::expected<MpRegistryHandle, core::ErrorInfo>
    create_primary(std::string_view file_prefix, MpTopology const& topo) {
        if (!topo.valid()) {
            SPDLOG_ERROR(
                "MpRegistry::create_primary: MpTopology failed valid() "
                "(self_index={} total_procs={} file_prefix='{}')",
                topo.self_index, topo.total_procs, file_prefix);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "MpRegistry::create_primary: topology failed valid()"});
        }

        auto name_r = build_mp_registry_name(file_prefix);
        if (!name_r) return std::unexpected(name_r.error());
        const char* name = name_r->data();

        // If a stale memzone from a previous run is still around (DPDK
        // may keep the segment alive across a non-cleanup exit), free
        // it first so we always start with fresh state — this is the
        // explicit "primary always resets" contract.
        if (auto* old = rte_memzone_lookup(name)) {
            SPDLOG_INFO(
                "MpRegistry: primary found stale memzone '{}' from a "
                "previous run; freeing before re-reserving",
                name);
            (void)rte_memzone_free(old);
        }

        const auto* mz = rte_memzone_reserve_aligned(
            name,
            sizeof(MpRegistryHeader),
            SOCKET_ID_ANY,
            /*flags=*/0,
            /*align=*/64);
        if (mz == nullptr) {
            SPDLOG_ERROR(
                "MpRegistry: rte_memzone_reserve_aligned('{}', size={}) "
                "failed (rte_errno={})",
                name, sizeof(MpRegistryHeader), rte_errno);
            return std::unexpected(core::ErrorInfo{
                core::Error::OutOfMemory,
                "MpRegistry: rte_memzone_reserve_aligned failed "
                "(see rte_errno; usually hugepage exhaustion)"});
        }

        auto* hdr = static_cast<MpRegistryHeader*>(mz->addr);
        init_mp_registry_header(hdr, file_prefix, topo);

        // CAS-claim our own slot. Primary on a fresh memzone always
        // succeeds (we just zeroed it); the only way this fails is
        // a benign race with itself in unit tests, in which case the
        // secondary path's "AlreadyClaimed" diagnostic is the right
        // surface.
        uint8_t expected = 0;
        if (!hdr->procs[topo.self_index].claimed.compare_exchange_strong(
                expected, 1, std::memory_order_acq_rel)) {
            SPDLOG_ERROR(
                "MpRegistry::create_primary: CAS-claim of self_index={} "
                "failed on freshly-initialized header — torn write or "
                "memzone collision (file_prefix='{}' total_procs={})",
                topo.self_index, file_prefix, topo.total_procs);
            (void)rte_memzone_free(mz);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "MpRegistry::create_primary: self_index already claimed "
                "(impossible on a freshly initialized header — "
                "indicates a torn write or memzone collision)"});
        }

        SPDLOG_INFO(
            "MpRegistry: primary reserved memzone '{}' "
            "(magic=0x{:08x} ver={} total_procs={} self_index={})",
            name, kMpRegistryMagic, kMpRegistryVersion,
            hdr->total_procs, topo.self_index);

        MpRegistryHandle h;
        h.hdr_          = hdr;
        h.mz_           = mz;
        h.owns_memzone_ = true;
        h.self_index_   = topo.self_index;
        return h;
    }

    /// @brief Look up the primary's registry memzone, cross-validate
    /// magic / version / file_prefix / per-slot topology, and CAS-claim
    /// `procs[topo.self_index]`. Called by `Platform::create_secondary`.
    ///
    /// @param already_claimed When `true` the CAS-claim step is
    /// skipped — the caller has already preclaimed the slot via
    /// `try_claim_free_slot()` (autojoin / `Platform::join_dynamic`
    /// path). The returned handle still takes ownership of the slot
    /// and will release it on destruction, so the caller MUST drop
    /// the original preclaim handle (or transfer it via move) before
    /// invoking attach_secondary with `already_claimed=true` to avoid
    /// double-release at teardown.
    [[nodiscard]] static std::expected<MpRegistryHandle, core::ErrorInfo>
    attach_secondary(std::string_view file_prefix, MpTopology const& topo,
                     bool already_claimed = false) {
        if (!topo.valid()) {
            SPDLOG_ERROR(
                "MpRegistry::attach_secondary: MpTopology failed valid() "
                "(self_index={} total_procs={} file_prefix='{}' "
                "already_claimed={})",
                topo.self_index, topo.total_procs, file_prefix,
                already_claimed);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "MpRegistry::attach_secondary: topology failed valid()"});
        }

        auto name_r = build_mp_registry_name(file_prefix);
        if (!name_r) return std::unexpected(name_r.error());
        const char* name = name_r->data();

        const auto* mz = rte_memzone_lookup(name);
        if (mz == nullptr) {
            SPDLOG_ERROR(
                "MpRegistry: rte_memzone_lookup('{}') returned NULL — "
                "primary not running, file_prefix mismatch, or EAL not "
                "initialized as secondary",
                name);
            return std::unexpected(core::ErrorInfo{
                core::Error::NotFound,
                "MpRegistry: memzone lookup failed (primary not running "
                "or file_prefix mismatch)"});
        }

        auto* hdr = static_cast<MpRegistryHeader*>(mz->addr);

        if (hdr->magic != kMpRegistryMagic) {
            SPDLOG_ERROR(
                "MpRegistry: header magic mismatch on '{}' "
                "(got=0x{:08x}, expected=0x{:08x}) — memzone collided "
                "with a non-eph layout under the same file_prefix",
                name, hdr->magic, kMpRegistryMagic);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "MpRegistry: header magic mismatch (memzone collided "
                "with a non-eph layout under the same file_prefix)"});
        }
        if (hdr->version != kMpRegistryVersion) {
            SPDLOG_ERROR(
                "MpRegistry: header version mismatch on '{}' "
                "(got={}, expected={}) — primary built with a different "
                "eph-net-dpdk version",
                name, hdr->version, kMpRegistryVersion);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "MpRegistry: header version mismatch (primary built "
                "with a different eph-net-dpdk version)"});
        }
        if (std::strncmp(hdr->file_prefix, file_prefix.data(),
                         std::min(file_prefix.size(),
                                  kMpRegistryFilePrefixMax - 1)) != 0) {
            SPDLOG_ERROR(
                "MpRegistry: file_prefix mismatch on '{}' "
                "(header='{}', supplied='{}')",
                name, hdr->file_prefix, file_prefix);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "MpRegistry: file_prefix in header does not match the "
                "one this secondary supplied"});
        }
        if (hdr->total_procs != topo.total_procs) {
            SPDLOG_ERROR(
                "MpRegistry: total_procs mismatch on '{}' "
                "(header={}, secondary's topo={})",
                name, hdr->total_procs, topo.total_procs);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "MpRegistry: total_procs mismatch — secondary's topology "
                "differs from the primary's view"});
        }
        if (topo.self_index >= hdr->total_procs) {
            SPDLOG_ERROR(
                "MpRegistry: self_index ({}) >= total_procs ({}) on '{}'",
                topo.self_index, hdr->total_procs, name);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "MpRegistry: self_index >= total_procs"});
        }

        // Cross-validate self spec — secondary may not silently disagree
        // with primary's view of queues/ports. lcore_mask is intentionally
        // EXCLUDED here: in the autojoin path, each peer knows only its
        // own lcores (via env / config), so primary writes its own slot's
        // mask but leaves peer slots' masks at 0 — secondaries publish
        // their own masks via the slot at claim time (see below).
        const ProcSlot& declared = hdr->procs[topo.self_index];
        const ProcSpec& wanted   = topo.procs[topo.self_index];
        if (declared.queue_lo   != wanted.queue_lo   ||
            declared.queue_hi   != wanted.queue_hi   ||
            declared.port_lo    != wanted.port_lo    ||
            declared.port_hi    != wanted.port_hi) {
            SPDLOG_ERROR(
                "MpRegistry: secondary's spec for self_index={} does not "
                "match primary's: declared queues=[{},{}) ports=[{},{}) "
                "vs primary queues=[{},{}) ports=[{},{})",
                topo.self_index,
                wanted.queue_lo,   wanted.queue_hi,
                wanted.port_lo,    wanted.port_hi,
                declared.queue_lo, declared.queue_hi,
                declared.port_lo,  declared.port_hi);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "MpRegistry: secondary's self spec disagrees with primary"});
        }

        // Cross-process lcore overlap check — only triggers when this
        // secondary's lcore_mask is non-zero (opt-in tracking) AND any
        // OTHER currently-claimed slot has overlapping bits. Catches the
        // "two processes accidentally pinned to lcore 0,1" mental model
        // gap where the OS scheduler silently steals CPU between them.
        //
        // Order: runs BEFORE the CAS for `!already_claimed`, so a
        // failure on that path leaves the slot un-claimed (no leak).
        // For `already_claimed=true` (autojoin), the caller has already
        // CAS-claimed via try_claim_free_slot before reaching here —
        // we must release the slot before returning error, otherwise
        // the slot leaks (caller's disarmed read-only handle won't
        // clear it).
        if (wanted.lcore_mask != 0) {
            const uint32_t total = hdr->total_procs;
            for (uint32_t i = 0; i < total; ++i) {
                if (i == topo.self_index) continue;
                if (hdr->procs[i].claimed.load(
                        std::memory_order_acquire) == 0) continue;
                const uint64_t other_mask = hdr->procs[i].lcore_mask;
                const uint64_t overlap = wanted.lcore_mask & other_mask;
                if (overlap != 0) {
                    SPDLOG_ERROR(
                        "MpRegistry: lcore conflict — self_index={} "
                        "lcore_mask=0x{:016x} overlaps with active "
                        "slot[{}] (tag='{}') lcore_mask=0x{:016x}; "
                        "overlapping bits=0x{:016x}. Two processes on "
                        "the same lcore cause OS scheduler CPU theft "
                        "and silently corrupt benchmark data.",
                        topo.self_index, wanted.lcore_mask,
                        i, hdr->procs[i].tag, other_mask, overlap);
                    if (already_claimed) {
                        // Caller pre-claimed; release before bailing.
                        hdr->procs[topo.self_index].claimed.store(
                            0, std::memory_order_release);
                    }
                    return std::unexpected(core::ErrorInfo{
                        core::Error::InvalidConfig,
                        "MpRegistry: lcore_mask overlaps with another "
                        "active process — silent CPU contention would "
                        "result"});
                }
            }
        }

        if (!already_claimed) {
            // CAS-claim. The whole point: if another secondary booted
            // with the same self_index, exactly one wins.
            uint8_t expected = 0;
            if (!hdr->procs[topo.self_index].claimed.compare_exchange_strong(
                    expected, 1, std::memory_order_acq_rel)) {
                SPDLOG_ERROR(
                    "MpRegistry: self_index={} already claimed (another "
                    "peer is running with the same self_index — config "
                    "bug)",
                    topo.self_index);
                return std::unexpected(core::ErrorInfo{
                    core::Error::InvalidConfig,
                    "MpRegistry: self_index is already claimed by another "
                    "process — duplicate self_index in MP topology"});
            }
        } else {
            // Caller has preclaimed via try_claim_free_slot. Verify the
            // slot is in fact in claimed=1 state — if not, the caller's
            // contract is broken and we refuse to take ownership of an
            // unclaimed slot (would race with a concurrent claimer).
            if (hdr->procs[topo.self_index].claimed.load(
                    std::memory_order_acquire) != 1) {
                SPDLOG_ERROR(
                    "MpRegistry: attach_secondary(already_claimed=true) "
                    "but procs[{}].claimed != 1 — caller contract violated",
                    topo.self_index);
                return std::unexpected(core::ErrorInfo{
                    core::Error::InvalidConfig,
                    "MpRegistry: attach_secondary(already_claimed=true) "
                    "but slot is not actually claimed"});
            }
        }

        // Publish this peer's lcore_mask into its own slot. Each peer
        // owns its slot's lcore_mask write — primary leaves peer slots'
        // masks at 0 because it doesn't know peers' lcore plans. The
        // pre-CAS conflict scan (above) already verified no overlap
        // with currently-claimed peers, so this write is safe.
        // Use release ordering so subsequent peers' acquire-loads in
        // their own conflict scan see this mask.
        if (wanted.lcore_mask != 0) {
            // (lcore_mask field is plain uint64_t, not atomic — but
            // single-writer per slot semantics make this safe; the
            // claimed.store(release) at slot release time provides
            // happens-before for any reader that ACQUIRE-loads claimed
            // first, then reads lcore_mask.)
            hdr->procs[topo.self_index].lcore_mask = wanted.lcore_mask;
            std::atomic_thread_fence(std::memory_order_release);
        }

        SPDLOG_INFO(
            "MpRegistry: secondary attached to '{}' (self_index={} "
            "queues=[{},{}) ports=[{},{}) lcore_mask=0x{:016x})",
            name, topo.self_index,
            wanted.queue_lo, wanted.queue_hi,
            wanted.port_lo,  wanted.port_hi, wanted.lcore_mask);

        MpRegistryHandle h;
        h.hdr_          = hdr;
        h.mz_           = mz;
        h.owns_memzone_ = false;
        h.self_index_   = topo.self_index;
        return h;
    }

    /// @brief Look up the primary's registry memzone and validate
    /// header-level invariants (magic / version / file_prefix) but
    /// **do not** CAS-claim any slot. Returns a read-only handle
    /// (`owns_slot()` is `false`) suitable for inspecting
    /// `header()->total_procs` and per-slot specs before the caller
    /// decides which free slot to claim via `try_claim_free_slot()`.
    ///
    /// This is the entry point for `Platform::join_dynamic` (the
    /// autojoin path) where the secondary doesn't know its own
    /// `self_index` until it scans the registry. The declarative
    /// path (`Platform::create_secondary`) goes through
    /// `attach_secondary` instead because it already knows its
    /// index from the topology.
    [[nodiscard]] static std::expected<MpRegistryHandle, core::ErrorInfo>
    attach_secondary_readonly(std::string_view file_prefix) {
        auto name_r = build_mp_registry_name(file_prefix);
        if (!name_r) return std::unexpected(name_r.error());
        const char* name = name_r->data();

        const auto* mz = rte_memzone_lookup(name);
        if (mz == nullptr) {
            SPDLOG_ERROR(
                "MpRegistry: attach_secondary_readonly: "
                "rte_memzone_lookup('{}') returned NULL — primary not "
                "running, prefix mismatch, or EAL not initialized as "
                "secondary",
                name);
            return std::unexpected(core::ErrorInfo{
                core::Error::NotFound,
                "MpRegistry: memzone lookup failed (primary not running "
                "or file_prefix mismatch)"});
        }

        auto* hdr = static_cast<MpRegistryHeader*>(mz->addr);

        if (hdr->magic != kMpRegistryMagic) {
            SPDLOG_ERROR(
                "MpRegistry: header magic mismatch on '{}' "
                "(got=0x{:08x}, expected=0x{:08x}) — memzone collided "
                "with a non-eph layout under the same file_prefix "
                "(read-only attach)",
                name, hdr->magic, kMpRegistryMagic);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "MpRegistry: header magic mismatch (memzone collided "
                "with a non-eph layout under the same file_prefix)"});
        }
        if (hdr->version != kMpRegistryVersion) {
            SPDLOG_ERROR(
                "MpRegistry: header version mismatch on '{}' "
                "(got={}, expected={}) — primary built with a different "
                "eph-net-dpdk version (read-only attach)",
                name, hdr->version, kMpRegistryVersion);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "MpRegistry: header version mismatch (primary built "
                "with a different eph-net-dpdk version)"});
        }
        if (std::strncmp(hdr->file_prefix, file_prefix.data(),
                         std::min(file_prefix.size(),
                                  kMpRegistryFilePrefixMax - 1)) != 0) {
            SPDLOG_ERROR(
                "MpRegistry: file_prefix mismatch on '{}' "
                "(header='{}', supplied='{}') — read-only attach",
                name, hdr->file_prefix, file_prefix);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "MpRegistry: file_prefix in header does not match the "
                "one this attach_secondary_readonly was called with"});
        }

        SPDLOG_INFO(
            "MpRegistry: attached read-only to '{}' "
            "(magic=0x{:08x} ver={} total_procs={})",
            name, hdr->magic, hdr->version, hdr->total_procs);

        MpRegistryHandle h;
        h.hdr_          = hdr;
        h.mz_           = mz;
        h.owns_memzone_ = false;
        h.self_index_   = kMpRegistrySelfIndexUnset;
        return h;
    }

    // ── Methods ──────────────────────────────────────────────────────────────

    /// @brief Scan `procs[0..total_procs)` and CAS-claim the lowest
    /// free slot. On success, sets this handle's `self_index_` so
    /// the destructor will release the slot, and returns the claimed
    /// index. On failure (all slots claimed) returns
    /// `Error::OutOfMemory`. Lock-free under contention.
    ///
    /// Precondition: this handle must be a fresh read-only handle
    /// from `attach_secondary_readonly` (i.e. `!owns_slot()`).
    /// Calling on an already-owning handle returns
    /// `Error::InvalidConfig` rather than silently double-claiming.
    [[nodiscard]] std::expected<uint8_t, core::ErrorInfo>
    try_claim_free_slot() noexcept {
        if (hdr_ == nullptr) {
            SPDLOG_ERROR(
                "MpRegistry::try_claim_free_slot: handle is inert "
                "(moved-from / never attached)");
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "MpRegistry::try_claim_free_slot: handle is inert"});
        }
        if (self_index_ != kMpRegistrySelfIndexUnset) {
            SPDLOG_ERROR(
                "MpRegistry::try_claim_free_slot: handle already owns "
                "self_index={} — caller must use a fresh read-only handle",
                self_index_);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "MpRegistry::try_claim_free_slot: handle already owns "
                "a slot"});
        }

        const uint32_t total = hdr_->total_procs;
        for (uint32_t i = 0; i < total; ++i) {
            uint8_t expected = 0;
            if (hdr_->procs[i].claimed.compare_exchange_strong(
                    expected, 1, std::memory_order_acq_rel)) {
                self_index_ = static_cast<uint8_t>(i);
                SPDLOG_INFO(
                    "MpRegistry: try_claim_free_slot claimed self_index={} "
                    "(of total_procs={})",
                    i, total);
                return self_index_;
            }
        }
        SPDLOG_WARN(
            "MpRegistry: try_claim_free_slot found all {} slots claimed "
            "(resource exhausted)",
            total);
        return std::unexpected(core::ErrorInfo{
            core::Error::OutOfMemory,
            "MpRegistry: all process slots are claimed "
            "(max_procs reached — start fewer peers or raise max_procs)"});
    }

private:
    void reset_() noexcept {
        hdr_          = nullptr;
        mz_           = nullptr;
        owns_memzone_ = false;
        self_index_   = kMpRegistrySelfIndexUnset;
    }

    void release_() noexcept {
        if (hdr_ == nullptr) return;
        // Always release the slot first so a peer that's spinning on
        // CAS can make progress before we tear down hugepage state.
        // Read-only handles (self_index_ == sentinel) own no slot —
        // skip the release to avoid touching out-of-bounds procs[].
        if (self_index_ != kMpRegistrySelfIndexUnset) {
            hdr_->procs[self_index_].claimed.store(0,
                                                    std::memory_order_release);
        }
        if (owns_memzone_ && mz_ != nullptr) {
            // MP teardown gate — see dpdk-mp-teardown-protocol.md for rationale.
            // Self slot was cleared above, so count_alive_procs() returns
            // peer count; non-zero means freeing would dangle peers' hdr_.
            if (count_alive_procs() != 0) {
                SPDLOG_INFO(
                    "MpRegistry: primary release_ — peers still attached, "
                    "deferring rte_memzone_free. memzone leaks until next "
                    "session (recycled by dpdk-teardown.sh).");
            } else {
                const int rc = rte_memzone_free(mz_);
                if (rc != 0) {
                    SPDLOG_ERROR(
                        "MpRegistry: rte_memzone_free failed (rc={}) — "
                        "hugepage segment will leak until process exit",
                        rc);
                } else {
                    SPDLOG_DEBUG(
                        "MpRegistry: primary freed memzone (self_index={})",
                        self_index_);
                }
            }
        }
        reset_();
    }

    MpRegistryHeader*        hdr_          = nullptr;
    const struct rte_memzone* mz_          = nullptr;
    bool                     owns_memzone_ = false;
    uint8_t                  self_index_   = kMpRegistrySelfIndexUnset;
};

} // namespace eph::dpdk::detail
