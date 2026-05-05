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
/// Hot path: NONE. Every operation here is on the cold bring-up path
/// (`Platform::primary_bringup_` / `secondary_bringup_`, invoked by
/// `Platform::create` (tenant) / `Platform::serve_nic` (daemon)) or
/// the `~Platform` teardown path, called at most once per process
/// lifecycle.

#include <array>
#include <atomic>
#include <cerrno>       // errno / ESRCH for is_pid_alive
#include <csignal>      // kill(pid, 0) for stale-slot liveness probe
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include <unistd.h>     // getpid

#include <spdlog/spdlog.h>

#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_lcore.h>
#include <rte_memzone.h>

#include "eph/core/error.hpp"
#include "eph/dpdk/detail/registry_hmac_key.hpp"  // T2.3: kRegistryHmacKeyBytes
#include "eph/dpdk/mp_topology.hpp"
#include "eph/net/hmac.hpp"                     // T2.3: HmacSha256Key + sign
#include "eph/utils/scope_guard.hpp"

#include <openssl/mem.h>  // CRYPTO_memcmp for constant-time tag verify

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
///       + pid (pid_t) — owner PID; attach-time liveness probe
///       via kill(pid, 0) detects stale slots from kill-9'd peers
///       and CAS-preempts them. Same v2 schema, no further bump.
///   v3: API reshape — cooperative-MP entry points removed; multi-
///       process is reached only via the daemon-led model
///       (`Platform::serve_nic` for the NIC primary running in
///       `eph-nicd`, `Platform::create` for tenant secondaries; the
///       previous autojoin entry `Platform::create_or_join` was
///       removed in the 2026-05-02 reshape). Wire layout unchanged;
///       bump signals API generation. v3 processes hard-reject v2
///       hugepages and vice-versa: a primary running v2 + secondary
///       launched at v3 is an environment mismatch that should fail
///       loudly, not silently misbehave. Recovery: stop all
///       secondaries → upgrade primary (it recreates the registry)
///       → upgrade secondaries.
///   v4: + `header.hmac_enabled` flag (1 byte) + 7 bytes padding +
///       per-slot 32-byte `tag` field (HMAC-SHA256 over the slot's
///       authenticated data). T2.3 wiring (2026-05-05). When
///       `hmac_enabled=0` the tag bytes are zero-initialized and
///       readers skip verification — single-tenant deployments see
///       no behavioural change. When `hmac_enabled=1` the daemon
///       signs every slot write with the key from
///       `/run/eph/<sanitize_bdf(pci)>.key` and tenants verify on
///       read; tampered slots surface as `Error::InvalidConfig`.
inline constexpr uint32_t kMpRegistryVersion = 4;

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
    /// Owner PID written at slot-claim time. v2 schema.
    /// Used by `attach_secondary` / `try_claim_free_slot` to detect
    /// stale slots: kill(pid, 0) returns ESRCH iff the owner died
    /// without releasing (kill -9, OOM, segfault). Stale slot is
    /// then CAS-preempted with WARN log. Stored as int32_t for ABI
    /// stability (pid_t is typically int but ABI-portable size).
    int32_t pid;

    /// HMAC-SHA256 tag over the *authenticated* slot data. v4 schema.
    /// Authenticated bytes: `tag[]`, `queue_lo`, `queue_hi`, `port_lo`,
    /// `port_hi`, `lcore_mask`, `pid` (everything *except* `claimed`,
    /// which is the cross-process atomic claim flag, and the tag itself).
    /// Zero-initialized when `header.hmac_enabled == 0` — readers
    /// skip verification in that case.
    /// Signed by primary at slot-write time; verified by secondaries on
    /// every read of a claimed slot. T2.3 wiring (2026-05-05).
    uint8_t hmac_tag[32];
};

struct alignas(64) MpRegistryHeader {
    uint32_t magic;
    uint32_t version;
    /// Number of populated `procs[]` entries declared by the primary.
    /// Caps at `MpTopology::kMaxProcs`. Secondaries verify their own
    /// `topo.procs.size() == header.total_procs`.
    uint32_t total_procs;
    /// 1 = HMAC-SHA256 entry tags are present and must be verified by
    /// secondaries on every slot read; 0 = single-tenant mode, slot
    /// `hmac_tag[]` bytes are zero and readers skip verification.
    /// v4 schema (T2.3 wiring). Single byte + 3 bytes pad to keep
    /// `_pad0` at a 4-byte boundary unchanged from v3.
    uint8_t  hmac_enabled;
    uint8_t  _pad_hmac[3];
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

/// Check if a PID corresponds to a live process via kill(pid, 0).
/// Returns true if the process exists (signal 0 = no-op delivery,
/// just permission/existence check). Returns false on ESRCH (process
/// dead) or EPERM with !ESRCH-via-cred (process exists but we can't
/// signal — treat as alive). pid <= 0 is invalid → false (clears
/// stale slot).
[[nodiscard]] inline bool is_pid_alive(int32_t pid) noexcept {
    if (pid <= 0) return false;
    if (::kill(pid, 0) == 0) return true;       // process exists, signalable
    if (errno == ESRCH)      return false;      // process truly dead
    if (errno == EPERM)      return true;       // exists but we lack perms
    return false;                                // any other errno = treat dead
}

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

// ─────────────────────────────────────────────────────────────────────
// T2.3 HMAC sign / verify helpers
// ─────────────────────────────────────────────────────────────────────
//
// Authenticate every byte of a `ProcSlot` *except* `claimed` (cross-
// process atomic claim flag — must remain mutable independently of
// the rest) and `hmac_tag` (the tag itself). The unauthenticated
// fields are the ones that change after a slot has been signed; if
// we authenticated `claimed` the verifier would always fail after a
// claim/release cycle.

inline constexpr size_t kSlotAuthBytes =
    sizeof(ProcSlot::tag)        +    // 32
    sizeof(uint16_t)             +    // queue_lo
    sizeof(uint16_t)             +    // queue_hi
    sizeof(uint32_t)             +    // port_lo
    sizeof(uint32_t)             +    // port_hi
    sizeof(uint64_t)             +    // lcore_mask
    sizeof(int32_t);                  // pid
// = 32 + 2 + 2 + 4 + 4 + 8 + 4 = 56 bytes

/// @brief Pack the authenticated bytes of `slot` into `out`. Output is
/// little-endian byte order on every host (we emit byte-by-byte from
/// the source uint*_t fields explicitly), so a HMAC tag computed on
/// host A is verifiable on host B regardless of native endian — though
/// in practice every secondary attached to the same primary is on the
/// same machine.
inline void
pack_slot_for_hmac(const ProcSlot& slot,
                   std::array<uint8_t, kSlotAuthBytes>& out) noexcept {
    size_t off = 0;
    std::memcpy(out.data() + off, slot.tag, sizeof(slot.tag));
    off += sizeof(slot.tag);
    auto put_u16 = [&](uint16_t v) {
        out[off++] = static_cast<uint8_t>(v & 0xFFu);
        out[off++] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    };
    auto put_u32 = [&](uint32_t v) {
        out[off++] = static_cast<uint8_t>(v & 0xFFu);
        out[off++] = static_cast<uint8_t>((v >> 8) & 0xFFu);
        out[off++] = static_cast<uint8_t>((v >> 16) & 0xFFu);
        out[off++] = static_cast<uint8_t>((v >> 24) & 0xFFu);
    };
    auto put_u64 = [&](uint64_t v) {
        for (int i = 0; i < 8; ++i)
            out[off++] = static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
    };
    put_u16(slot.queue_lo);
    put_u16(slot.queue_hi);
    put_u32(slot.port_lo);
    put_u32(slot.port_hi);
    put_u64(slot.lcore_mask);
    put_u32(static_cast<uint32_t>(slot.pid));
}

/// @brief Sign `slot` in place: compute HMAC-SHA256 over its
/// authenticated bytes under `key` and store the 32-byte result into
/// `slot.hmac_tag`. Called by the daemon (primary) at slot-write time.
inline void
sign_slot_in_place(ProcSlot& slot,
                   const ::eph::net::HmacSha256Key& key) noexcept {
    std::array<uint8_t, kSlotAuthBytes> packed{};
    pack_slot_for_hmac(slot, packed);
    const auto sig = ::eph::net::hmac_sha256_sign(
        key, std::span<const uint8_t>{packed.data(), packed.size()});
    static_assert(sizeof(slot.hmac_tag) == sig.bytes.size(),
                  "ProcSlot::hmac_tag size mismatch with HmacSha256Tag");
    std::memcpy(slot.hmac_tag, sig.bytes.data(), sig.bytes.size());
}

/// @brief Verify `slot.hmac_tag` against `key` in constant time.
/// Returns true on match. Caller (the secondary) discards the slot
/// data on mismatch and surfaces `Error::InvalidConfig` to the
/// application.
[[nodiscard]] inline bool
verify_slot(const ProcSlot& slot,
            const ::eph::net::HmacSha256Key& key) noexcept {
    std::array<uint8_t, kSlotAuthBytes> packed{};
    pack_slot_for_hmac(slot, packed);
    const auto sig = ::eph::net::hmac_sha256_sign(
        key, std::span<const uint8_t>{packed.data(), packed.size()});
    return CRYPTO_memcmp(slot.hmac_tag, sig.bytes.data(),
                         sig.bytes.size()) == 0;
}

/// @brief Initialize a fresh registry header in `dst` from `topo` +
/// `file_prefix`. Caller has already established `dst` points at a
/// freshly reserved memzone of >= sizeof(MpRegistryHeader) bytes.
///
/// Precondition: `topo.valid() == true`. The body iterates
/// `[0, topo.total_procs)` over the source `topo.procs` array (which
/// is fixed-size `kMaxProcs`), so a malformed `total_procs > kMaxProcs`
/// would read OOB. `MpRegistryHandle::create_primary` (the only
/// caller) gates on `topo.valid()` before calling — do not bypass.
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
    // T2.3 wiring: the unkeyed overload zeroes hmac_enabled +
    // hmac_tag bytes — single-tenant deployments see no behavioural
    // change. The keyed overload below sets hmac_enabled=1 and signs.
    dst->hmac_enabled = 0;
    std::memset(dst->_pad_hmac, 0, sizeof(dst->_pad_hmac));
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
        s.pid        = 0;
        std::memset(s.hmac_tag, 0, sizeof(s.hmac_tag));
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

/// @brief T2.3 keyed overload — same as `init_mp_registry_header` but
/// also flips `header.hmac_enabled = 1` and signs every populated
/// `topo.procs[i]` slot with `key`. Free / unpopulated slots stay
/// zeroed (a slot's tag is only valid once it has been claimed; the
/// secondary attach path checks `hmac_enabled && claimed` before
/// verifying).
inline void
init_mp_registry_header_with_hmac(MpRegistryHeader* dst,
                                  std::string_view file_prefix,
                                  MpTopology const& topo,
                                  const ::eph::net::HmacSha256Key& key) noexcept {
    init_mp_registry_header(dst, file_prefix, topo);
    dst->hmac_enabled = 1;
    // Sign the populated slots. Free slots remain zero-tagged; their
    // `claimed == 0` already gates verifier from running on them.
    for (uint8_t i = 0; i < topo.total_procs; ++i) {
        sign_slot_in_place(dst->procs[i], key);
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

    /// @brief T2.3 wiring (cold path). Flip the registry to keyed
    /// mode and stash the key for future sign/verify. Daemon
    /// (`Platform::serve_nic`) calls this immediately after
    /// `create_primary` returns when `enable_registry_hmac=true`,
    /// supplying the same key written to `/run/eph/<bdf>.key`.
    ///
    /// Sets `header.hmac_enabled = 1` and signs every populated
    /// slot so the initial state is verifiable.
    /// Idempotent. Trailing underscore reflects "internal-eph-glue"
    /// status — applications never call this; serve_nic does.
    void enable_hmac_(::eph::net::HmacSha256Key key) noexcept {
        if (hdr_ == nullptr) {
            SPDLOG_DEBUG(
                "MpRegistry::enable_hmac_: handle is moved-from "
                "(noop)");
            return;
        }
        const bool was_enabled = (hdr_->hmac_enabled == 1);
        hmac_key_.emplace(std::move(key));
        hdr_->hmac_enabled = 1;
        for (uint8_t i = 0; i < hdr_->total_procs; ++i) {
            sign_slot_in_place(hdr_->procs[i], *hmac_key_);
        }
        SPDLOG_INFO(
            "MpRegistry::enable_hmac_: HMAC tamper protection {} "
            "(signed {} populated slot(s); was_enabled={})",
            was_enabled ? "rekeyed" : "enabled",
            hdr_->total_procs, was_enabled);
    }

    /// @brief T2.3 audit-on-suspicion. Verify every populated slot
    /// against the stashed key. Returns the number of mismatches
    /// (0 in the healthy case). Each mismatch logged at WARN.
    /// Returns 0 when hmac_enabled==0 (unkeyed mode → no-op).
    /// Cold-path; called from a daemon control thread on operator
    /// `eph-nicctl audit` requests, NOT from any hot dispatch path.
    [[nodiscard]] size_t audit_all() const noexcept {
        if (hdr_ == nullptr || !hdr_->hmac_enabled ||
            !hmac_key_.has_value()) return 0;
        size_t mismatches = 0;
        for (uint8_t i = 0; i < hdr_->total_procs; ++i) {
            const auto& s = hdr_->procs[i];
            if (s.claimed.load(std::memory_order_acquire) == 0) continue;
            if (!verify_slot(s, *hmac_key_)) {
                ++mismatches;
                SPDLOG_WARN(
                    "MpRegistry: tamper detected at slot {} "
                    "(audit) — pid={}, queue=[{},{}), port=[{},{})",
                    i, s.pid, s.queue_lo, s.queue_hi,
                    s.port_lo, s.port_hi);
            }
        }
        return mismatches;
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
    /// Called by `Platform::primary_bringup_` (the internal helper
    /// invoked by `Platform::serve_nic` — the daemon entry,
    /// post-2026-05-02 reshape — when bringing up the NIC primary).
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
        // Publish own pid for liveness probe by future peers.
        hdr->procs[topo.self_index].pid = static_cast<int32_t>(::getpid());

        SPDLOG_INFO(
            "MpRegistry: primary reserved memzone '{}' "
            "(magic=0x{:08x} ver={} total_procs={} self_index={} pid={})",
            name, kMpRegistryMagic, kMpRegistryVersion,
            hdr->total_procs, topo.self_index,
            hdr->procs[topo.self_index].pid);

        MpRegistryHandle h;
        h.hdr_          = hdr;
        h.mz_           = mz;
        h.owns_memzone_ = true;
        h.self_index_   = topo.self_index;
        return h;
    }

    /// @brief Look up the primary's registry memzone, cross-validate
    /// magic / version / file_prefix / per-slot topology, and CAS-claim
    /// `procs[topo.self_index]`. Called by `Platform::secondary_bringup_`
    /// (the internal helper invoked by `Platform::create` — the tenant
    /// entry, post-2026-05-02 reshape — to attach this process as a
    /// DPDK secondary to the daemon's NIC primary).
    ///
    /// @param already_claimed When `true` the CAS-claim step is
    /// skipped — the caller has already preclaimed the slot via
    /// `try_claim_free_slot()` (the daemon-led tenant attach in
    /// `Platform::create`). The returned handle still takes ownership
    /// of the slot and will release it on destruction, so the caller
    /// MUST drop the original preclaim handle (or transfer it via
    /// move) before invoking attach_secondary with
    /// `already_claimed=true` to avoid double-release at teardown.
    [[nodiscard]] static std::expected<MpRegistryHandle, core::ErrorInfo>
    attach_secondary(std::string_view file_prefix, MpTopology const& topo,
                     bool already_claimed = false) {
        // When the autojoin path preclaimed via try_claim_free_slot, ANY
        // early-return below would leak the preclaimed slot — claimed=1
        // with this process's pid, which `is_pid_alive` correctly sees as
        // alive, so subsequent attempts in the same process can never
        // pass-2-reclaim it. The spec-disagree / lcore-overlap branches
        // already release manually; this guard does the same for every
        // earlier validation failure (memzone lookup, magic/version,
        // file_prefix, total_procs, self_index range). We can only honour
        // the release once `hdr` has been resolved AND we know the slot
        // index is in range; failures before that point leave
        // `hdr_for_release` null so the guard's lambda is a no-op.
        MpRegistryHeader* hdr_for_release = nullptr;
        uint8_t           idx_for_release = 0;
        auto slot_guard = utils::ScopeGuard{[&] {
            if (hdr_for_release != nullptr) {
                hdr_for_release->procs[idx_for_release].claimed.store(
                    0, std::memory_order_release);
            }
        }};

        if (!topo.valid()) {
            SPDLOG_ERROR(
                "MpRegistry::attach_secondary: MpTopology failed valid() "
                "(self_index={} total_procs={} file_prefix='{}' "
                "already_claimed={})",
                topo.self_index, topo.total_procs, file_prefix,
                already_claimed);
            // No hdr yet → cannot release; topology coming from a
            // builder-validated source (autojoin uses MpTopology::uniform)
            // makes this branch effectively unreachable on the preclaim
            // path. Pre-v3 cooperative callers controlled the topology and
            // hadn't preclaimed.
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

        // Once `hdr` and `topo.self_index` are both available we can arm
        // the preclaim release guard. Validation failures from this point
        // on will release the slot during stack unwind; the explicit
        // releases at the spec-disagree / lcore-overlap branches still
        // disarm the guard before returning so we never double-release.
        if (already_claimed && topo.self_index < MpTopology::kMaxProcs) {
            hdr_for_release = hdr;
            idx_for_release = topo.self_index;
        }

        if (hdr->magic != kMpRegistryMagic) {
            SPDLOG_ERROR(
                "MpRegistry: header magic mismatch on '{}' "
                "(got=0x{:08x}, expected=0x{:08x}) — memzone collided "
                "with a non-eph layout under the same file_prefix",
                name, hdr->magic, kMpRegistryMagic);
            // slot_guard releases on unwind if already_claimed
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "MpRegistry: header magic mismatch (memzone collided "
                "with a non-eph layout under the same file_prefix)"});
        }
        if (hdr->version != kMpRegistryVersion) {
            SPDLOG_ERROR(
                "MpRegistry: header version mismatch on '{}' "
                "(got={}, expected={}) — primary built with a different "
                "eph-net-dpdk version. Recovery: stop all secondaries, "
                "restart primary (it recreates the registry at the "
                "current schema), then start secondaries.",
                name, hdr->version, kMpRegistryVersion);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "MpRegistry: header version mismatch — recovery: stop "
                "all secondaries, restart primary, then start secondaries"});
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
            // self_index out of range — guard's idx_to_release was
            // bounded by kMaxProcs, but the slot we'd release may not
            // be the slot the caller actually preclaimed. Disarm to
            // avoid clearing an unrelated slot that pass-2 reclaim will
            // self-heal anyway.
            slot_guard.disarm();
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
            // Caller pre-claimed via try_claim_free_slot before reaching
            // here (the daemon-led `Platform::create` tenant attach).
            // This early-return otherwise leaks the slot — claimed=1
            // with the failing process's PID. If the process exits,
            // pass-2 reclaim self-heals it; but if the caller retries
            // `Platform::create` from the same process,
            // `is_pid_alive` returns true on the stale slot and every
            // subsequent attach fails with "no free slots". Mirror the
            // lcore_mask-overlap release pattern below.
            if (already_claimed) {
                hdr->procs[topo.self_index].claimed.store(
                    0, std::memory_order_release);
                slot_guard.disarm();  // explicit release done; avoid double
            }
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
                        slot_guard.disarm();  // explicit release done
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

        // Publish this peer's lcore_mask + pid into its own slot.
        // Each peer owns its slot's metadata writes — primary leaves
        // peer slots' lcore_masks at 0 because it doesn't know peers'
        // lcore plans. The pre-CAS conflict scan (above) already
        // verified no overlap with currently-claimed peers.
        //
        // Memory-ordering contract (round-2 fix, 2026-05-01):
        // The CAS above (acq_rel) makes claimed=1 visible to readers,
        // but its release-side only orders writes BEFORE it. The plain
        // stores below happen AFTER the CAS, so a peer's acquire-load
        // of claimed=1 does NOT synchronize-with these stores — on
        // weakly-ordered cores (AArch64 / POWER) the reader could see
        // claimed=1 paired with stale lcore_mask=0, silently bypassing
        // the v2 cross-process conflict gate. The redundant
        // `claimed.store(1, release)` AFTER the writes is a no-op
        // value-wise (slot is already 1) but provides the release-store
        // partner for future readers' acquire-loads on `claimed`.
        hdr->procs[topo.self_index].pid = static_cast<int32_t>(::getpid());
        if (wanted.lcore_mask != 0) {
            hdr->procs[topo.self_index].lcore_mask = wanted.lcore_mask;
        }
        // Re-publish claimed=1 with release semantics so readers'
        // acquire-load(claimed)==1 happens-after the metadata writes.
        hdr->procs[topo.self_index].claimed.store(
            1, std::memory_order_release);

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
        // Slot ownership transfers from preclaim guard to `h`; disarm so
        // the guard's destructor does not race with the new handle's
        // future release_().
        slot_guard.disarm();
        return h;
    }

    /// @brief Look up the primary's registry memzone and validate
    /// header-level invariants (magic / version / file_prefix) but
    /// **do not** CAS-claim any slot. Returns a read-only handle
    /// (`owns_slot()` is `false`) suitable for inspecting
    /// `header()->total_procs` and per-slot specs before the caller
    /// decides which free slot to claim via `try_claim_free_slot()`.
    ///
    /// This is the entry point for `Platform::create` (the
    /// daemon-led tenant attach, post-2026-05-02 reshape; previously
    /// `Platform::create_or_join`'s autojoin path) where the
    /// secondary doesn't know its own `self_index` until it scans
    /// the registry. `attach_secondary` (the slot-claim variant on
    /// the same handle) was the entry point for the pre-v3
    /// cooperative `Platform::create_secondary` path; that public API
    /// was removed in 2026-05-01's reshape and the helper is now used
    /// only by `secondary_bringup_` callers that have already
    /// preclaimed via `try_claim_free_slot`.
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
                "eph-net-dpdk version (read-only attach). Recovery: stop "
                "all secondaries, restart primary, then start secondaries.",
                name, hdr->version, kMpRegistryVersion);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "MpRegistry: header version mismatch — recovery: stop "
                "all secondaries, restart primary, then start secondaries"});
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

        // total_procs sanity. Storage is uint32_t but the design contract
        // is `[1, MpTopology::kMaxProcs]` (the fixed-size `procs[]` array
        // bound). Past magic+version checks the header is "ours" — but a
        // future-schema primary that bumped layout fields BEFORE bumping
        // version, or a corrupted hugepage segment, could let an extreme
        // value slip through. Without this guard the value silently
        // truncates to uint8_t in the secondary bring-up
        // (`Platform::create` → `secondary_bringup_`) and surfaces as
        // an opaque "MpTopology::valid() failed" three layers up.
        if (hdr->total_procs == 0
                || hdr->total_procs > MpTopology::kMaxProcs) {
            SPDLOG_ERROR(
                "MpRegistry: header total_procs={} is out of contract "
                "range [1, kMaxProcs={}] on '{}' — registry corruption "
                "or undeclared schema skew (magic+version matched but "
                "the fixed-size procs[] array cannot represent this "
                "value); read-only attach refused",
                hdr->total_procs, MpTopology::kMaxProcs, name);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "MpRegistry: header total_procs out of contract range "
                "[1, kMaxProcs] (registry corruption or undeclared "
                "schema skew despite magic+version match)"});
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
        // Pass 1: try to claim a truly-free slot.
        for (uint32_t i = 0; i < total; ++i) {
            uint8_t expected = 0;
            if (hdr_->procs[i].claimed.compare_exchange_strong(
                    expected, 1, std::memory_order_acq_rel)) {
                self_index_ = static_cast<uint8_t>(i);
                // Clear residual metadata (pid + lcore_mask) from any
                // previous graceful-release owner before publishing our
                // own pid. release_() on the prior owner is supposed to
                // wipe these fields, but we double-clear here as a
                // defense-in-depth measure: if any code path ever
                // forgets the clear, this site catches it. Mirrors the
                // pass-2 clear at the kill-9 reclaim branch below.
                clear_slot_metadata_(hdr_->procs[i]);
                hdr_->procs[i].pid = static_cast<int32_t>(::getpid());
                // Round-2 fix: re-publish claimed=1 with release
                // semantics so a peer's acquire-load on `claimed`
                // happens-after the plain stores above. Without this
                // re-store, the CAS's release-side only orders writes
                // BEFORE itself; the metadata writes happen AFTER the
                // CAS and would not synchronize with future readers
                // on weakly-ordered cores.
                hdr_->procs[i].claimed.store(
                    1, std::memory_order_release);
                SPDLOG_INFO(
                    "MpRegistry: try_claim_free_slot claimed self_index={} "
                    "(of total_procs={}, pid={})",
                    i, total, hdr_->procs[i].pid);
                return self_index_;
            }
        }
        // Pass 2: reclaim slots whose owner is dead (kill -9 / OOM).
        // Vuln 2 from /pax --review: stale slots from killed peers
        // permanently block new attaches and prevent primary teardown
        // from firing (count_alive_procs sees ghost process).
        // Probe via kill(pid, 0) and CAS-preempt on ESRCH.
        for (uint32_t i = 0; i < total; ++i) {
            const int32_t stored_pid = hdr_->procs[i].pid;
            if (is_pid_alive(stored_pid)) continue;
            // Slot's owner is dead. Force claim (claimed was 1 → CAS to 1
            // again is a no-op; we just overwrite pid + lcore_mask).
            // Race: another peer may probe and steal at the same instant;
            // CAS to 0 first (release stale), then to 1 (own claim) using
            // strict expected sequence.
            uint8_t exp_claimed = 1;
            if (!hdr_->procs[i].claimed.compare_exchange_strong(
                    exp_claimed, 0, std::memory_order_acq_rel)) {
                // Slot state changed between probe and CAS — another
                // peer either released or also reclaimed it. Skip and
                // let the next loop iteration / next call retry.
                continue;
            }
            // We hold "claimed=0"; immediately try to re-claim it.
            uint8_t exp_free = 0;
            if (hdr_->procs[i].claimed.compare_exchange_strong(
                    exp_free, 1, std::memory_order_acq_rel)) {
                // Reset stale data (pid + lcore_mask) before publishing
                // own pid. Same helper as release_ / pass-1 so future
                // schema additions can't leave one site behind.
                clear_slot_metadata_(hdr_->procs[i]);
                hdr_->procs[i].pid = static_cast<int32_t>(::getpid());
                self_index_ = static_cast<uint8_t>(i);
                // Round-2 fix: re-publish claimed=1 with release
                // semantics — same rationale as pass-1 above (the
                // CAS's release-side only orders prior writes; the
                // metadata stores above need their own release to
                // synchronize with future readers' acquire-loads).
                hdr_->procs[i].claimed.store(
                    1, std::memory_order_release);
                SPDLOG_WARN(
                    "MpRegistry: try_claim_free_slot reclaimed stale "
                    "self_index={} (previous owner pid={} dead — kill -9 "
                    "/ OOM / segfault); now owned by pid={}",
                    i, stored_pid, hdr_->procs[i].pid);
                return self_index_;
            }
            // Another peer raced us back to 1 — try next slot.
        }
        SPDLOG_WARN(
            "MpRegistry: try_claim_free_slot found all {} slots claimed "
            "(and all owners alive — resource exhausted)",
            total);
        return std::unexpected(core::ErrorInfo{
            core::Error::OutOfMemory,
            "MpRegistry: all process slots are claimed "
            "(MpTopology::kMaxProcs reached — start fewer peers; "
            "the cap is a compile-time constant since the 2026-05-02 "
            "daemon reshape removed the configurable max_procs field)"});
    }

private:
    void reset_() noexcept {
        hdr_          = nullptr;
        mz_           = nullptr;
        owns_memzone_ = false;
        self_index_   = kMpRegistrySelfIndexUnset;
    }

    /// @brief Zero out the per-claimer ephemeral metadata fields on a
    /// `ProcSlot` (`pid`, `lcore_mask`). The other ProcSlot fields
    /// (`tag`, `queue_lo/hi`, `port_lo/hi`) are intentionally preserved
    /// because they are the primary-owned topology contract written
    /// once at `init_mp_registry_header` time and shared by every
    /// claimant of the same `self_index` — clearing them would force
    /// each new claimant to re-publish identical values for no benefit.
    ///
    /// **Happens-before contract** (two distinct sides):
    ///
    /// *Release side* (slot becomes free): callers MUST invoke this
    /// BEFORE the `claimed.store(0, release)` that signals "slot is
    /// free". A reader that subsequently sees `claimed=0` via
    /// acquire-load is guaranteed to see the cleared metadata —
    /// pristine slot semantics.
    ///
    /// *Claim side* (slot becomes occupied): the `claimed.compare_
    /// exchange_*(_, 1, acq_rel)` makes `claimed=1` visible, but its
    /// release-side only orders writes BEFORE itself. Plain stores of
    /// `pid`/`lcore_mask` that happen AFTER the CAS need their own
    /// release-store partner to synchronize with future readers —
    /// otherwise a peer doing `claimed.load(acquire)==1` followed by
    /// a plain read of `lcore_mask` could see stale data on weakly-
    /// ordered cores (AArch64 / POWER). Each claim site therefore
    /// follows: CAS(0→1, acq_rel) → clear_slot_metadata_(...) →
    /// pid/lcore_mask plain stores → claimed.store(1, release). The
    /// final store is a no-op value-wise (slot is already 1) but
    /// provides the release-store partner for future acquire-loads.
    ///
    /// Discovered via /pax --review LENS "production / MP mental model
    /// / silent misuse closure" on 2026-05-01:
    ///   * Round 1 fix (release side): the previous code missed the
    ///     metadata clear in two of three release/claim sites
    ///     (graceful `release_` + `try_claim_free_slot` pass-1), so a
    ///     benign release+reclaim cycle would let the previous owner's
    ///     lcore_mask survive and trip attach_secondary's conflict
    ///     scan against a non-existent peer.
    ///   * Round 2 fix (claim side): plain stores of pid/lcore_mask
    ///     after the CAS-claim were not happens-before any reader's
    ///     acquire-load on `claimed`; the redundant
    ///     claimed.store(1, release) closes the synchronization gap.
    ///     The previous orphan `std::atomic_thread_fence(release)` in
    ///     attach_secondary was a no-op (no subsequent atomic store
    ///     to pair with) and has been removed.
    static void clear_slot_metadata_(ProcSlot& s) noexcept {
        s.pid        = 0;
        s.lcore_mask = 0;
    }

    void release_() noexcept {
        if (hdr_ == nullptr) return;
        // Always release the slot first so a peer that's spinning on
        // CAS can make progress before we tear down hugepage state.
        // Read-only handles (self_index_ == sentinel) own no slot —
        // skip the release to avoid touching out-of-bounds procs[].
        if (self_index_ != kMpRegistrySelfIndexUnset) {
            // Clear ephemeral metadata BEFORE the release-store on
            // `claimed`, so any peer that subsequently sees `claimed=0`
            // via acquire-load also sees a pristine slot — no ghost
            // pid/lcore_mask leaking into the next claim cycle.
            clear_slot_metadata_(hdr_->procs[self_index_]);
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
    /// T2.3 wiring: when populated, sign all slot writes + verify
    /// on audit. Daemon (Platform::serve_nic) installs via
    /// `enable_hmac_()`. Wrapped in optional so unkeyed default
    /// (single tenant) pays zero key construction cost.
    std::optional<::eph::net::HmacSha256Key> hmac_key_{};
};

/// @brief T2.3 N series — process-level pointer to the daemon's
/// MpRegistry handle. Set by `Platform::serve_nic` (primary side)
/// alongside `g_active_queue_allocator` / `g_active_icmp_directory`;
/// nullptr on tenants and on hosts running unkeyed mode. The
/// `on_nicctl_audit_thunk` loads via this global to call
/// `audit_all()` for the audit reply's mp_registry counters.
inline std::atomic<MpRegistryHandle*> g_active_mp_registry{nullptr};

} // namespace eph::dpdk::detail
