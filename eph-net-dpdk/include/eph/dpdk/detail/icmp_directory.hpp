#pragma once

/// @file detail/icmp_directory.hpp
/// Cross-process ICMP target directory — a hugepage-backed POD lookup
/// table mapping `(4-tuple, proto) → owner_proc_index`.
///
/// Purpose: when an ICMP Frag Needed message lands on a secondary's
/// RX queue but the target stream lives in primary (or another
/// secondary), the local `IcmpRegistry::dispatch` finds nothing and
/// would otherwise silently drop the message. This directory lets the
/// receiving process look up the owner and forward the parsed message
/// via `rte_mp_*` IPC.
///
/// Why it's separate from `IcmpRegistry`:
///   - `IcmpRegistry` lives in heap memory, holds raw `void* stream`
///     pointers (only valid in the owning process), uses `std::mutex`
///     (undefined behaviour in cross-process shared memory)
///   - This directory only needs `(tuple, proto, owner_proc_index)` —
///     no per-target callback, no stream pointer. It's POD-only,
///     atomic-only, hugepage-friendly.
///   - Result: `IcmpRegistry` stays untouched (the existing
///     test_icmp_dispatch's 10 cases keep passing byte-for-byte).
///     This directory is a strict addition: each `register_icmp_
///     target` call now does **dual register** — local registry +
///     this directory.
///
/// Generation counter (stale-handle protection):
///   Each entry has a `std::atomic<uint32_t> generation` that is
///   incremented on every `unregister`. IPC dispatch messages carry
///   the generation observed at lookup time; the receiving process
///   re-reads the entry's gen on dispatch and drops if mismatched —
///   preventing UAF if a stream is unregistered between the
///   secondary's lookup and the primary's dispatch.
///
/// Mutual layout: this header parallels `mp_registry.hpp`'s POD
/// layout style — same `magic / version / file_prefix / [array of
/// atomic-fielded entries]` shape, same memzone-backed RAII handle.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include <spdlog/spdlog.h>

#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_lcore.h>
#include <rte_memzone.h>

#include <openssl/mem.h>  // T2.3: CRYPTO_memcmp for constant-time tag verify

#include "eph/core/error.hpp"
#include "eph/net/hmac.hpp"  // T2.3: HmacSha256Key + sign helpers
#include "eph/dpdk/detail/icmp_registry.hpp"  // IcmpRegistry (for thunk)
#include "eph/dpdk/detail/mp_ipc.hpp"         // pack/parse_payload<T>
#include "eph/dpdk/packet_core.hpp"           // ConnectionTuple
#include "eph/dpdk/packet_parse.hpp"          // ParsedIcmp

namespace eph::dpdk::detail {

// ─────────────────────────────────────────────────────────────────────────────
// Compile-time constants
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Memzone name prefix. Composed name is `eph_mp_icmp/<file_prefix>`.
inline constexpr std::string_view kIcmpDirectoryNamePrefix = "eph_mp_icmp/";

/// @brief Mirrors `RTE_MEMZONE_NAMESIZE` (32 incl. NUL).
inline constexpr size_t kIcmpDirectoryNameCap = 32;

/// @brief Cap on the user-supplied `file_prefix` portion of the name.
/// 32 - len("eph_mp_icmp/") - 1 (trailing NUL) = 19.
inline constexpr size_t kIcmpDirectoryFilePrefixMax =
    kIcmpDirectoryNameCap - kIcmpDirectoryNamePrefix.size() - 1;  // 19

/// @brief Magic bytes 'EICD' (eph icmp directory) in memory order. A
/// hex dump shows `45 49 43 44 ..` and is greppable.
inline constexpr uint32_t kIcmpDirectoryMagic =
    (static_cast<uint32_t>('E') << 0)  |
    (static_cast<uint32_t>('I') << 8)  |
    (static_cast<uint32_t>('C') << 16) |
    (static_cast<uint32_t>('D') << 24);

/// v2 (T2.3 wiring, 2026-05-05): adds `header.hmac_enabled` flag +
/// per-entry `hmac_tag[32]`. Hot-path lookup() does NOT verify (would
/// add ~150-300ns/aarch64 per ICMP, far above the 50-200ns/mbuf
/// budget). Verification is **on-suspicion**:
///   - audit-on-error: callbacks that reject an entry call
///     `verify_entry_for_audit()` to surface tampering vs ABA;
///   - periodic sweep: `audit_sweep_one_round(key)` verifies a
///     batch of entries each call (caller invokes ~1 Hz from a
///     control thread).
/// Hybrid coverage: passive tampering detected within ≤1 second; an
/// active error-trigger triggers immediate audit. Single-tenant
/// deployments leave `hmac_enabled=0` and pay zero overhead.
inline constexpr uint32_t kIcmpDirectoryVersion = 2;

/// @brief Cap on populated entries. 1024 ≈ 4 procs × 256 streams/proc;
/// generous for HFT but not extravagant. Header sizeof ≈ 32 KiB ≪
/// hugepage (2 MiB). Increasing requires a version bump.
inline constexpr size_t kIcmpDirectoryMaxEntries = 1024;

/// @brief Sentinel used to indicate "no owner" / "free slot" in
/// `owner_proc` field. Real proc indices are 0-based ≤ 63
/// (MpTopology::kMaxProcs cap).
inline constexpr uint8_t kIcmpDirectoryNoOwner = 0xFF;

/// @brief `IcmpDirectoryEntry::claimed` state values. See the field's
/// docstring for the lifecycle.
inline constexpr uint8_t kIcmpSlotFree       = 0;
inline constexpr uint8_t kIcmpSlotPublished  = 1;
inline constexpr uint8_t kIcmpSlotInProgress = 2;

// ─────────────────────────────────────────────────────────────────────────────
// POD layout — must remain trivially copyable + standard layout
// ─────────────────────────────────────────────────────────────────────────────

/// @brief One directory slot. Memory layout MUST stay stable across
/// the build tree (all eph processes link the same header). Fields
/// are ordered so all atomics align naturally; pads are explicit.
struct IcmpDirectoryEntry {
    /// State machine for the slot's claim lifecycle:
    ///   0 = free (no owner, fields are zeroed),
    ///   2 = in-progress (CAS-acquired but tuple/owner fields not yet
    ///       fully written — readers MUST treat as miss),
    ///   1 = published (tuple/owner fields fully visible; readers can
    ///       safely compare).
    /// CAS 0→2 on register; release-store 2→1 once fields are written;
    /// release-store →0 on unregister. The 2-step publish closes the
    /// race where a reader load-acquired `claimed==1` but the producer's
    /// non-atomic field writes (proto/owner_proc/ips/ports) had not yet
    /// happened-before that load.
    std::atomic<uint8_t> claimed;
    /// IP protocol (e.g. 6 = TCP, 17 = UDP). 0 = unset.
    uint8_t  proto;
    /// Owning process index. `kIcmpDirectoryNoOwner` (0xFF) when free.
    uint8_t  owner_proc;
    uint8_t  _pad0;

    uint32_t src_ip;   ///< Host byte order, matches ConnectionTuple
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;

    uint32_t _pad1;    ///< Align `generation` to 8-byte boundary

    /// Bumped on every unregister; IPC dispatch messages carry the
    /// gen observed at lookup, owner re-reads on dispatch, drops if
    /// mismatch (stale handle protection).
    std::atomic<uint32_t> generation;

    /// HMAC-SHA256 tag over the *authenticated* entry payload (proto +
    /// owner_proc + src_ip + dst_ip + src_port + dst_port — i.e. the
    /// 5-tuple plus owner). Signed by primary at two-phase publish
    /// time; verified on suspicion, NOT on every lookup. v2 schema
    /// (T2.3 wiring, 2026-05-05). Zero when `header.hmac_enabled == 0`.
    /// `claimed` and `generation` are intentionally NOT in the
    /// authenticated payload — `claimed` mutates per claim/unclaim
    /// cycle (signing it would force re-sign on every state
    /// transition); `generation` mutates per unregister (same).
    uint8_t hmac_tag[32];
};

/// @brief Header block at the start of the memzone. Cacheline-aligned
/// so the hot stats counters don't share a line with anything else
/// while still keeping the entries[] array tightly packed behind it.
struct alignas(64) IcmpDirectoryHeader {
    uint32_t magic;          ///< = kIcmpDirectoryMagic
    uint32_t version;        ///< = kIcmpDirectoryVersion
    uint32_t max_entries;    ///< Effective `entries` array bound (== kIcmpDirectoryMaxEntries)
    /// 1 = HMAC-SHA256 entry tags present and audited via
    /// verify-on-suspicion + periodic sweep; 0 = single-tenant mode
    /// (entry `hmac_tag[]` zero, audit calls return "no-op success").
    /// v2 schema (T2.3 wiring).
    uint8_t  hmac_enabled;
    uint8_t  _pad_hmac[3];   ///< Align _pad0 to 4
    uint32_t _pad0;          ///< Align file_prefix to 8

    /// Null-terminated copy of the user `file_prefix`. Sanity-check
    /// only — primary/secondary mismatch on file_prefix would have
    /// already failed at `rte_memzone_lookup` (different memzone
    /// names). Carried here for human-readable dump output.
    char file_prefix[kIcmpDirectoryFilePrefixMax + 1];

    /// Stats — atomic so any process can read live counts without locking.
    std::atomic<uint64_t> ipc_msgs_sent;
    std::atomic<uint64_t> ipc_msgs_received;
    std::atomic<uint64_t> dropped_stale;
    std::atomic<uint64_t> dropped_no_owner;

    std::array<IcmpDirectoryEntry, kIcmpDirectoryMaxEntries> entries;
};

static_assert(std::atomic<uint8_t>::is_always_lock_free,
              "IcmpDirectoryEntry::claimed must be lock-free for cross-process use");
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "IcmpDirectoryEntry::generation must be lock-free");
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "IcmpDirectoryHeader stats counters must be lock-free");
static_assert(std::is_trivially_copyable_v<IcmpDirectoryEntry>,
              "IcmpDirectoryEntry must be trivially copyable for memzone storage");
static_assert(std::is_trivially_copyable_v<IcmpDirectoryHeader>,
              "IcmpDirectoryHeader must be trivially copyable for memzone storage");
static_assert(alignof(IcmpDirectoryHeader) >= 64,
              "IcmpDirectoryHeader requires cacheline alignment");

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Compose memzone name `eph_mp_icmp/<file_prefix>` into a
/// fixed-size buffer. Returns InvalidConfig on empty / oversize.
[[nodiscard]] inline std::expected<std::array<char, kIcmpDirectoryNameCap>,
                                   core::ErrorInfo>
build_icmp_directory_name(std::string_view file_prefix) noexcept {
    if (file_prefix.empty()) {
        SPDLOG_ERROR(
            "IcmpDirectory::build_name: file_prefix is empty — "
            "PlatformConfig.file_prefix must be set for MP-IPC");
        return std::unexpected(core::ErrorInfo{
            core::Error::InvalidConfig,
            "IcmpDirectory: file_prefix must be non-empty"});
    }
    if (file_prefix.size() > kIcmpDirectoryFilePrefixMax) {
        SPDLOG_ERROR(
            "IcmpDirectory::build_name: file_prefix='{}' size={} exceeds "
            "{} bytes (RTE_MEMZONE_NAMESIZE - len(\"eph_mp_icmp/\") - 1)",
            file_prefix, file_prefix.size(), kIcmpDirectoryFilePrefixMax);
        return std::unexpected(core::ErrorInfo{
            core::Error::InvalidConfig,
            "IcmpDirectory: file_prefix exceeds 19 bytes "
            "(RTE_MEMZONE_NAMESIZE - len(\"eph_mp_icmp/\") - 1)"});
    }

    std::array<char, kIcmpDirectoryNameCap> buf{};
    std::memcpy(buf.data(), kIcmpDirectoryNamePrefix.data(),
                kIcmpDirectoryNamePrefix.size());
    std::memcpy(buf.data() + kIcmpDirectoryNamePrefix.size(),
                file_prefix.data(), file_prefix.size());
    return buf;
}

// ─────────────────────────────────────────────────────────────────────
// T2.3 HMAC helpers — verify-on-suspicion mode (hot-path stays clean)
// ─────────────────────────────────────────────────────────────────────
//
// Authenticated payload per entry (16 bytes, explicit little-endian):
//
//   proto        1 byte
//   owner_proc   1 byte
//   src_ip       4 bytes
//   dst_ip       4 bytes
//   src_port     2 bytes
//   dst_port     2 bytes
//   _pad         2 bytes (zero — keep payload size 16 = nice round)
//                ─────
//                16 bytes total
//
// Excluded from payload:
//   - claimed       — atomic state machine (0/1/2 per claim/unclaim)
//   - generation    — bumped per unregister
//   - hmac_tag      — tag itself
//
// The exclusion of `claimed` and `generation` lets primary sign the
// entry once at publish (claimed: 2→1) and never re-sign while the
// entry stays valid. Subsequent unregister-and-reclaim cycles with
// SAME tuple+owner re-sign produces the same tag (deterministic);
// with different tuple+owner the new claim re-signs to a new tag.

inline constexpr size_t kIcmpEntryAuthBytes = 16;

inline void
pack_icmp_entry_for_hmac(const IcmpDirectoryEntry& e,
                         std::array<uint8_t, kIcmpEntryAuthBytes>& out) noexcept {
    size_t off = 0;
    out[off++] = e.proto;
    out[off++] = e.owner_proc;
    auto put_u32 = [&](uint32_t v) {
        out[off++] = static_cast<uint8_t>(v & 0xFFu);
        out[off++] = static_cast<uint8_t>((v >> 8) & 0xFFu);
        out[off++] = static_cast<uint8_t>((v >> 16) & 0xFFu);
        out[off++] = static_cast<uint8_t>((v >> 24) & 0xFFu);
    };
    auto put_u16 = [&](uint16_t v) {
        out[off++] = static_cast<uint8_t>(v & 0xFFu);
        out[off++] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    };
    put_u32(e.src_ip);
    put_u32(e.dst_ip);
    put_u16(e.src_port);
    put_u16(e.dst_port);
    out[off++] = 0;  // _pad
    out[off++] = 0;  // _pad
}

/// @brief Sign `entry` in place. Called by primary at two-phase
/// publish time, *between* the field writes and the release-store
/// of `claimed = kIcmpSlotPublished`. Cost: ~150-300 ns aarch64.
/// **NEVER** called on the hot lookup path.
inline void
sign_icmp_entry_in_place(IcmpDirectoryEntry& e,
                         const ::eph::net::HmacSha256Key& key) noexcept {
    std::array<uint8_t, kIcmpEntryAuthBytes> packed{};
    pack_icmp_entry_for_hmac(e, packed);
    const auto sig = ::eph::net::hmac_sha256_sign(
        key, std::span<const uint8_t>{packed.data(), packed.size()});
    static_assert(sizeof(e.hmac_tag) == sig.bytes.size(),
                  "IcmpDirectoryEntry::hmac_tag size mismatch");
    std::memcpy(e.hmac_tag, sig.bytes.data(), sig.bytes.size());
}

/// @brief Verify `entry.hmac_tag` against `key` in constant time.
/// Called from:
///   - audit-on-error: when a callback rejects an entry, before
///     concluding the rejection is logical (tuple mismatch) vs
///     malicious (tampered). Cost amortized to error-handling path.
///   - periodic sweep (audit_sweep_one_round): caller invokes ~1 Hz.
/// **NEVER** called from `lookup()` on the hot RX dispatch path.
[[nodiscard]] inline bool
verify_icmp_entry(const IcmpDirectoryEntry& e,
                  const ::eph::net::HmacSha256Key& key) noexcept {
    std::array<uint8_t, kIcmpEntryAuthBytes> packed{};
    pack_icmp_entry_for_hmac(e, packed);
    const auto sig = ::eph::net::hmac_sha256_sign(
        key, std::span<const uint8_t>{packed.data(), packed.size()});
    return CRYPTO_memcmp(e.hmac_tag, sig.bytes.data(),
                         sig.bytes.size()) == 0;
}

/// @brief Initialize a fresh directory in `dst`. Caller has ensured
/// `dst` points at a freshly reserved memzone of >=
/// sizeof(IcmpDirectoryHeader) bytes.
inline void
init_icmp_directory_header(IcmpDirectoryHeader* dst,
                           std::string_view file_prefix) noexcept {
    dst->magic       = kIcmpDirectoryMagic;
    dst->version     = kIcmpDirectoryVersion;
    dst->max_entries = static_cast<uint32_t>(kIcmpDirectoryMaxEntries);
    dst->hmac_enabled = 0;  // T2.3: default unkeyed; flipped by
                            // enable_hmac_() helper on the daemon side
    std::memset(dst->_pad_hmac, 0, sizeof(dst->_pad_hmac));
    dst->_pad0       = 0;
    std::memset(dst->file_prefix, 0, sizeof(dst->file_prefix));
    std::memcpy(dst->file_prefix, file_prefix.data(),
                std::min(file_prefix.size(),
                         sizeof(dst->file_prefix) - 1));

    dst->ipc_msgs_sent.store(0, std::memory_order_relaxed);
    dst->ipc_msgs_received.store(0, std::memory_order_relaxed);
    dst->dropped_stale.store(0, std::memory_order_relaxed);
    dst->dropped_no_owner.store(0, std::memory_order_relaxed);

    for (auto& e : dst->entries) {
        e.claimed.store(kIcmpSlotFree, std::memory_order_relaxed);
        e.proto = 0;
        e.owner_proc = kIcmpDirectoryNoOwner;
        e._pad0 = 0;
        e.src_ip = 0;
        e.dst_ip = 0;
        e.src_port = 0;
        e.dst_port = 0;
        e._pad1 = 0;
        e.generation.store(0, std::memory_order_relaxed);
        std::memset(e.hmac_tag, 0, sizeof(e.hmac_tag));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// IcmpDirectoryHandle — RAII owner of a directory attachment
// ─────────────────────────────────────────────────────────────────────────────

class IcmpDirectoryHandle {
public:
    /// @brief Look-up result returned by `lookup()`. Carries the
    /// owner proc index, the generation observed at lookup time,
    /// and the slot index for re-checking.
    struct LookupResult {
        uint8_t  owner_proc;
        uint32_t generation;
        size_t   slot_idx;
    };

    IcmpDirectoryHandle() noexcept = default;

    IcmpDirectoryHandle(const IcmpDirectoryHandle&) = delete;
    IcmpDirectoryHandle& operator=(const IcmpDirectoryHandle&) = delete;

    IcmpDirectoryHandle(IcmpDirectoryHandle&& o) noexcept
        : hdr_(o.hdr_), mz_(o.mz_), owns_memzone_(o.owns_memzone_) {
        o.reset_();
    }

    IcmpDirectoryHandle& operator=(IcmpDirectoryHandle&& o) noexcept {
        if (this != &o) {
            release_();
            hdr_          = o.hdr_;
            mz_           = o.mz_;
            owns_memzone_ = o.owns_memzone_;
            o.reset_();
        }
        return *this;
    }

    ~IcmpDirectoryHandle() { release_(); }

    [[nodiscard]] explicit operator bool() const noexcept {
        return hdr_ != nullptr;
    }

    [[nodiscard]] IcmpDirectoryHeader const* header() const noexcept {
        return hdr_;
    }
    [[nodiscard]] IcmpDirectoryHeader* header() noexcept { return hdr_; }

    // ── Factories ────────────────────────────────────────────────────────────

    /// @brief Reserve (or replace) the directory memzone. Same
    /// reset-on-create contract as `MpRegistryHandle::create_primary`.
    [[nodiscard]] static std::expected<IcmpDirectoryHandle, core::ErrorInfo>
    create_primary(std::string_view file_prefix) {
        auto name_r = build_icmp_directory_name(file_prefix);
        if (!name_r) return std::unexpected(name_r.error());
        const char* name = name_r->data();

        // Free any stale memzone from a previous run — primary always
        // resets (matches mp_registry's contract).
        if (auto* old = rte_memzone_lookup(name)) {
            SPDLOG_INFO(
                "IcmpDirectory: primary found stale memzone '{}' from a "
                "previous run; freeing before re-reserving",
                name);
            (void)rte_memzone_free(old);
        }

        const auto* mz = rte_memzone_reserve_aligned(
            name,
            sizeof(IcmpDirectoryHeader),
            SOCKET_ID_ANY,
            /*flags=*/0,
            /*align=*/64);
        if (mz == nullptr) {
            SPDLOG_ERROR(
                "IcmpDirectory: rte_memzone_reserve_aligned('{}', size={}) "
                "failed (rte_errno={})",
                name, sizeof(IcmpDirectoryHeader), rte_errno);
            return std::unexpected(core::ErrorInfo{
                core::Error::OutOfMemory,
                "IcmpDirectory: rte_memzone_reserve_aligned failed "
                "(see rte_errno; usually hugepage exhaustion)"});
        }

        auto* hdr = static_cast<IcmpDirectoryHeader*>(mz->addr);
        init_icmp_directory_header(hdr, file_prefix);

        SPDLOG_INFO(
            "IcmpDirectory: primary reserved memzone '{}' "
            "(magic=0x{:08x} ver={} max_entries={})",
            name, kIcmpDirectoryMagic, kIcmpDirectoryVersion,
            hdr->max_entries);

        IcmpDirectoryHandle h;
        h.hdr_          = hdr;
        h.mz_           = mz;
        h.owns_memzone_ = true;
        return h;
    }

    /// @brief Look up the primary's directory memzone and validate.
    [[nodiscard]] static std::expected<IcmpDirectoryHandle, core::ErrorInfo>
    attach_secondary(std::string_view file_prefix) {
        auto name_r = build_icmp_directory_name(file_prefix);
        if (!name_r) return std::unexpected(name_r.error());
        const char* name = name_r->data();

        const auto* mz = rte_memzone_lookup(name);
        if (mz == nullptr) {
            SPDLOG_ERROR(
                "IcmpDirectory: rte_memzone_lookup('{}') returned NULL — "
                "primary not running, file_prefix mismatch, or EAL not "
                "initialized as secondary",
                name);
            return std::unexpected(core::ErrorInfo{
                core::Error::NotFound,
                "IcmpDirectory: memzone lookup failed"});
        }

        auto* hdr = static_cast<IcmpDirectoryHeader*>(mz->addr);
        if (hdr->magic != kIcmpDirectoryMagic) {
            SPDLOG_ERROR(
                "IcmpDirectory: header magic mismatch on '{}' "
                "(got=0x{:08x}, expected=0x{:08x}) — memzone collision "
                "or corruption",
                name, hdr->magic, kIcmpDirectoryMagic);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "IcmpDirectory: header magic mismatch (memzone collision "
                "or corruption)"});
        }
        if (hdr->version != kIcmpDirectoryVersion) {
            SPDLOG_ERROR(
                "IcmpDirectory: header version mismatch on '{}' "
                "(got={}, expected={}) — primary built with a different "
                "eph-net-dpdk revision",
                name, hdr->version, kIcmpDirectoryVersion);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "IcmpDirectory: header version mismatch (build-tree drift)"});
        }
        // max_entries sanity. Storage is uint32_t but the contract is the
        // compile-time constant `kIcmpDirectoryMaxEntries` — the same
        // value `init_icmp_directory_header` writes at primary-create time
        // and the static `entries[]` array bound. A corrupt or schema-
        // skewed (but magic+version-passing) primary could otherwise let
        // a runaway value through, and several IcmpDirectory methods use
        // `max_entries` as either a loop bound (register_target Pass 1/2,
        // post-CAS rescan, list_targets) or a slot-index guard
        // (release_slot, lookup_owner). Either path turns a bigger-than-
        // bound value into an out-of-range read on the fixed-size array.
        // Use strict equality — any drift means "this header isn't ours
        // and we don't want to silently truncate".
        if (hdr->max_entries != kIcmpDirectoryMaxEntries) {
            SPDLOG_ERROR(
                "IcmpDirectory: header max_entries={} disagrees with the "
                "compile-time constant kIcmpDirectoryMaxEntries={} on '{}' "
                "— registry corruption or undeclared schema skew (magic+"
                "version matched but the entries[] array bound disagrees); "
                "secondary attach refused before any register_target/"
                "lookup_owner could read past entries.end()",
                hdr->max_entries, kIcmpDirectoryMaxEntries, name);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "IcmpDirectory: header max_entries disagrees with "
                "kIcmpDirectoryMaxEntries (registry corruption or "
                "undeclared schema skew despite magic+version match)"});
        }

        SPDLOG_INFO(
            "IcmpDirectory: secondary attached to '{}' (max_entries={})",
            name, hdr->max_entries);

        IcmpDirectoryHandle h;
        h.hdr_          = hdr;
        h.mz_           = mz;
        h.owns_memzone_ = false;
        return h;
    }

    // ── Mutators ─────────────────────────────────────────────────────────────

    /// @brief Claim a free slot for `(tuple, proto, owner_proc)`.
    /// Returns the slot index on success, ResourceExhausted-class
    /// error on full directory, InvalidConfig on duplicate.
    /// Linear scan; cap=1024 → ~few μs worst case (cold path).
    [[nodiscard]] std::expected<size_t, core::ErrorInfo>
    register_target(net::ConnectionTuple const& tuple,
                    uint8_t                     proto,
                    uint8_t                     owner_proc) noexcept {
        if (hdr_ == nullptr) {
            SPDLOG_ERROR(
                "IcmpDirectory::register_target: handle is moved-from "
                "(proto={} owner_proc={})", proto, owner_proc);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "IcmpDirectory::register_target: handle is moved-from"});
        }
        if (owner_proc == kIcmpDirectoryNoOwner) {
            SPDLOG_ERROR(
                "IcmpDirectory::register_target: owner_proc=0xFF is the "
                "no-owner sentinel (proto={} src=0x{:08x}:{} dst=0x{:08x}:{})",
                proto, tuple.src_ip, tuple.src_port,
                tuple.dst_ip, tuple.dst_port);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "IcmpDirectory::register_target: 0xFF is reserved as "
                "no-owner sentinel"});
        }

        // Pass 1: best-effort early dup-check. This is **not** the
        // authoritative dup detection — a concurrent peer may CAS-claim
        // the same (tuple, proto) between this pass and Pass 2 below.
        // The post-claim re-scan in Pass 2 is the canonical guard. We
        // keep Pass 1 because in the common single-process / no-race
        // case it surfaces the dup error before we burn a slot, giving
        // a clearer diagnostic ("already registered" vs "raced and
        // lost").
        //
        // Only treat state==Published as a candidate for the dup
        // compare. State==InProgress means a concurrent peer holds the
        // slot but its tuple/owner fields have not yet been published —
        // their non-atomic writes are NOT happens-before our acquire
        // load on `claimed`, so reading those fields would observe an
        // arbitrary mix of stale + new bytes. The post-claim Pass 2
        // scan re-discovers any duplicate that was racing with us.
        for (size_t i = 0; i < hdr_->max_entries; ++i) {
            auto& e = hdr_->entries[i];
            if (e.claimed.load(std::memory_order_acquire) != kIcmpSlotPublished)
                continue;
            if (e.proto == proto &&
                e.src_ip == tuple.src_ip && e.dst_ip == tuple.dst_ip &&
                e.src_port == tuple.src_port && e.dst_port == tuple.dst_port) {
                SPDLOG_WARN(
                    "IcmpDirectory::register_target: duplicate (tuple, proto) "
                    "found at slot {} (proto={} src=0x{:08x}:{} "
                    "dst=0x{:08x}:{} existing owner_proc={})",
                    i, proto, tuple.src_ip, tuple.src_port,
                    tuple.dst_ip, tuple.dst_port, e.owner_proc);
                return std::unexpected(core::ErrorInfo{
                    core::Error::InvalidConfig,
                    "IcmpDirectory::register_target: (tuple, proto) "
                    "already registered"});
            }
        }

        // Pass 2: CAS-claim a free slot, then re-scan for duplicates
        // post-claim. This collapses a TOCTOU race where two peers
        // with the same (tuple, proto) both cleared Pass 1, both
        // CAS-claimed disjoint slots, and both wrote — leaving the
        // directory with two entries owning the same tuple and
        // `lookup` returning a non-deterministic owner_proc. The
        // post-claim scan finds the conflict deterministically: each
        // peer that wrote ITS payload checks whether anyone else is
        // already publishing the same payload at a lower-or-different
        // index. The peer with the higher index releases its slot
        // and returns dup-error; the peer with the lower index keeps
        // it. Both peers reach a consistent verdict because the
        // tiebreaker (lower index wins) is total and observable
        // through acquire loads.
        for (size_t i = 0; i < hdr_->max_entries; ++i) {
            auto& e = hdr_->entries[i];
            uint8_t expected = kIcmpSlotFree;
            // Two-step publish — step 1: CAS to InProgress. The slot
            // is now reserved for us but readers MUST treat it as a
            // miss (not yet Published) because tuple/owner fields are
            // about to be written below.
            if (!e.claimed.compare_exchange_strong(
                    expected, kIcmpSlotInProgress,
                    std::memory_order_acq_rel)) {
                continue;  // slot was just claimed by someone else
            }
            // Race won on this slot — write payload while in
            // InProgress state. No reader compares fields against an
            // InProgress slot (Pass 1 / Pass 2 / lookup all gate on
            // ==Published), so the non-atomic field stores are safe.
            //
            // Note: gen is NOT bumped on register; only on unregister
            // (so a fresh slot starts at gen=0 and increments per
            // detach cycle). This means dispatch IPC carrying gen=0
            // is "first generation, never unregistered yet" — fine.
            e.proto      = proto;
            e.owner_proc = owner_proc;
            e._pad0      = 0;
            e.src_ip     = tuple.src_ip;
            e.dst_ip     = tuple.dst_ip;
            e.src_port   = tuple.src_port;
            e.dst_port   = tuple.dst_port;

            // T2.3 wiring: sign the entry BEFORE the Published
            // release-store. This way any reader that observes
            // Published is also guaranteed (via the same release-
            // acquire edge) to see the freshly-written hmac_tag.
            // Hot lookup() does NOT verify; sign only happens at
            // publish + on legitimate re-claim.
            // Skip when hmac_enabled==0 (single-tenant) — leaves
            // tag bytes whatever they were (zero from init or
            // residual from prior publication; either way readers
            // ignore the tag in unkeyed mode).
            if (hdr_->hmac_enabled && hmac_key_.has_value()) {
                sign_icmp_entry_in_place(e, *hmac_key_);
            }

            // Two-step publish — step 2: release-store →Published. Any
            // subsequent acquire-load on `claimed` that observes
            // Published is now guaranteed to also observe every field
            // write above. THIS is the synchronizes-with edge for the
            // tuple comparison in lookup() / Pass 1 / Pass 2.
            e.claimed.store(kIcmpSlotPublished, std::memory_order_release);

            // Post-claim duplicate scan. We scan ALL slots (not just
            // [0, i)) because a concurrent peer may have CAS-claimed
            // a slot at index j > i AFTER our slot but before our
            // Published store became visible. By scanning the full
            // range and yielding to any peer at a strictly lower
            // index, we get a stable lower-index-wins tiebreaker
            // while still detecting symmetric concurrent writers.
            //
            // Only Published slots compare — InProgress means the
            // peer hasn't finished writing fields, and we'd race
            // their writes. If they're racing with us on the same
            // tuple, ONE of us will see the other's Published state
            // on a subsequent register attempt or via Pass 2's own
            // re-scan after their publish completes. The
            // race-yielding tiebreaker (lower index wins) is still
            // total because both peers eventually reach Published
            // state and one will scan the other on their second
            // pass. Worst case: both peers briefly succeed; the
            // next register attempt for the same tuple fails Pass
            // 1 and the actual duplicate is surfaced.
            for (size_t j = 0; j < hdr_->max_entries; ++j) {
                if (j == i) continue;
                auto& other = hdr_->entries[j];
                if (other.claimed.load(std::memory_order_acquire)
                        != kIcmpSlotPublished)
                    continue;
                // Compare tuple+proto via direct field load. proto/
                // ports/ips are non-atomic, but the acquire-load on
                // `claimed`==Published above synchronises-with the
                // producer's release-store transition (InProgress→
                // Published) — by which point ALL field writes have
                // happened-before. Field reads here are race-free.
                if (other.proto == proto &&
                    other.src_ip == tuple.src_ip &&
                    other.dst_ip == tuple.dst_ip &&
                    other.src_port == tuple.src_port &&
                    other.dst_port == tuple.dst_port) {
                    if (j < i) {
                        // We lose the tiebreaker — release our slot
                        // so the dup-winner is the only owner. Do
                        // NOT bump generation on this release: this
                        // slot was Published only momentarily and
                        // is about to be invalidated; bumping gen
                        // would noisily invalidate any future
                        // readers of THIS slot's gen counter.
                        e.proto      = 0;
                        e.owner_proc = kIcmpDirectoryNoOwner;
                        e.src_ip     = 0;
                        e.dst_ip     = 0;
                        e.src_port   = 0;
                        e.dst_port   = 0;
                        e.claimed.store(kIcmpSlotFree,
                                        std::memory_order_release);
                        SPDLOG_DEBUG(
                            "IcmpDirectory::register_target: lost "
                            "TOCTOU race for (proto={} src=0x{:08x}:{} "
                            "dst=0x{:08x}:{}) — peer claimed lower "
                            "slot {} first; releasing slot {}",
                            proto, tuple.src_ip, tuple.src_port,
                            tuple.dst_ip, tuple.dst_port, j, i);
                        return std::unexpected(core::ErrorInfo{
                            core::Error::InvalidConfig,
                            "IcmpDirectory::register_target: (tuple, "
                            "proto) already registered (lost TOCTOU "
                            "race against a concurrent peer)"});
                    }
                    // j > i: we hold the lower index, the other
                    // peer must yield. Continue scanning to surface
                    // any other duplicates.
                }
            }
            return i;
        }
        SPDLOG_ERROR(
            "IcmpDirectory::register_target: directory full ({} slots) "
            "(proto={} src=0x{:08x}:{} dst=0x{:08x}:{} owner_proc={}) — "
            "increase kIcmpDirectoryMaxEntries or shorten stream lifetime",
            hdr_->max_entries, proto, tuple.src_ip, tuple.src_port,
            tuple.dst_ip, tuple.dst_port, owner_proc);
        return std::unexpected(core::ErrorInfo{
            core::Error::OutOfMemory,
            "IcmpDirectory::register_target: directory full (1024 slots)"});
    }

    /// @brief Release a slot. Bumps generation so any in-flight IPC
    /// msg referencing this slot's old gen will be dropped at owner-
    /// side dispatch.
    ///
    /// Idempotent: silently no-op on out-of-range `slot_idx` and on
    /// a slot that is already free (`claimed==0`). The free-slot
    /// guard matters because:
    ///
    ///   * `IcmpDirectorySlotGuard::release_()` already gates on its
    ///     own `engaged_` flag, but a misbehaving caller invoking
    ///     `unregister(slot_idx)` directly (test paths, future
    ///     introspection tools) on a stale index would otherwise
    ///     corrupt a valid neighbouring registration that has since
    ///     reused that slot, AND silently bump its generation so the
    ///     legitimate owner's in-flight IPC dispatches start getting
    ///     dropped as "stale gen". Both failures present as silent
    ///     ICMP misses with no diagnostic — bad.
    ///   * Idempotent unregister also lets retry / cleanup paths
    ///     call `unregister` defensively without bookkeeping.
    void unregister(size_t slot_idx) noexcept {
        if (hdr_ == nullptr || slot_idx >= hdr_->max_entries) return;
        auto& e = hdr_->entries[slot_idx];
        // Free-slot guard — see @note above. Acquire-load on `claimed`
        // synchronises-with the producing release-store→Published in
        // register_target so we observe a consistent view of the
        // slot's state. Treat InProgress as occupied: another peer
        // is mid-publish on this slot; releasing it would be racy.
        // (In practice slot_idx originates from a SlotGuard tied to
        // a successful register_target return, so we won't see
        // InProgress for our own slot — this branch handles the
        // misbehaving-direct-caller path.)
        const uint8_t state = e.claimed.load(std::memory_order_acquire);
        if (state == kIcmpSlotFree) return;
        if (state == kIcmpSlotInProgress) {
            SPDLOG_WARN(
                "IcmpDirectory::unregister: slot {} is InProgress "
                "(another peer mid-publish) — refusing to release; "
                "caller likely passed a stale slot_idx",
                slot_idx);
            return;
        }
        // ++gen first (acquire-release with claimed clear so dispatch
        // path observing Published + new gen is consistent).
        e.generation.fetch_add(1, std::memory_order_acq_rel);
        e.proto      = 0;
        e.owner_proc = kIcmpDirectoryNoOwner;
        e.src_ip     = 0;
        e.dst_ip     = 0;
        e.src_port   = 0;
        e.dst_port   = 0;
        e.claimed.store(kIcmpSlotFree, std::memory_order_release);
    }

    // ── Lookups (read-only) ─────────────────────────────────────────────────

    /// @brief Find the slot owning `(tuple, proto)`. Returns
    /// `std::nullopt` if no such target.
    /// Linear scan; cap=1024 → ~few μs worst case (cold ICMP miss path).
    [[nodiscard]] std::optional<LookupResult>
    lookup(net::ConnectionTuple const& tuple,
           uint8_t                     proto) const noexcept {
        if (hdr_ == nullptr) return std::nullopt;
        for (size_t i = 0; i < hdr_->max_entries; ++i) {
            const auto& e = hdr_->entries[i];
            // Only Published slots have field writes that
            // happen-before our acquire-load; InProgress slots are a
            // miss. The producer's transient InProgress→Published
            // window is sub-microsecond; an ICMP miss in that window
            // means the receiver retransmits via TCP and the next
            // PMTU update lands on a Published slot.
            if (e.claimed.load(std::memory_order_acquire)
                    != kIcmpSlotPublished)
                continue;
            if (e.proto == proto &&
                e.src_ip == tuple.src_ip && e.dst_ip == tuple.dst_ip &&
                e.src_port == tuple.src_port && e.dst_port == tuple.dst_port) {
                return LookupResult{
                    .owner_proc = e.owner_proc,
                    .generation = e.generation.load(std::memory_order_acquire),
                    .slot_idx   = i,
                };
            }
        }
        return std::nullopt;
    }

    /// @brief Stale-check used by the owner-side IPC handler before
    /// dispatching to the local `IcmpRegistry`. Returns true iff the
    /// slot is still claimed AND its generation matches `expected_gen`.
    [[nodiscard]] bool
    is_slot_alive(size_t slot_idx, uint32_t expected_gen) const noexcept {
        if (hdr_ == nullptr || slot_idx >= hdr_->max_entries) return false;
        const auto& e = hdr_->entries[slot_idx];
        // Only Published slots count as alive — InProgress is a slot
        // mid-publish and must not deliver to a stale handler.
        return e.claimed.load(std::memory_order_acquire) == kIcmpSlotPublished
            && e.generation.load(std::memory_order_acquire) == expected_gen;
    }

    /// @brief Caller asserts another peer still holds a reference to
    /// this directory's shared memzone (via `attach_secondary_lookup`),
    /// so the destructor must NOT call `rte_memzone_free` (which would
    /// dangle the peer's `hdr_` pointer). Set during MP teardown by
    /// `Platform::Impl::~Impl()` when `mp_registry->is_last_alive_proc()`
    /// is false. Idempotent.
    ///
    /// **Owns_memzone semantics**: this clears the `owns_memzone_` flag
    /// in-place. The handle still carries `hdr_` and `mz_` pointers (so
    /// other accessors keep working), but the destructor's
    /// `rte_memzone_free` branch is skipped. The OS releases the
    /// per-process hugepage mapping on exit; the shared backing stays
    /// for peers, recycled by `dpdk-teardown.sh` on next session.
    void disable_memzone_free() noexcept {
        owns_memzone_ = false;
    }

private:
    void reset_() noexcept {
        hdr_          = nullptr;
        mz_           = nullptr;
        owns_memzone_ = false;
    }

    void release_() noexcept {
        if (hdr_ == nullptr) return;
        if (owns_memzone_ && mz_ != nullptr) {
            const int rc = rte_memzone_free(mz_);
            if (rc != 0) {
                SPDLOG_ERROR(
                    "IcmpDirectory: rte_memzone_free failed (rc={}) — "
                    "hugepage segment will leak until process exit",
                    rc);
            }
        }
        reset_();
    }

    IcmpDirectoryHeader*      hdr_          = nullptr;
    const struct rte_memzone* mz_           = nullptr;
    bool                      owns_memzone_ = false;
    /// T2.3: when populated, sign every published entry. Daemon owns
    /// the master key; secondaries that want to audit hold their own
    /// (read from `/run/eph/<bdf>.key` per `registry_hmac_key.hpp`).
    /// Wrapped in optional so unkeyed default pays zero key
    /// construction cost.
    std::optional<::eph::net::HmacSha256Key> hmac_key_{};
    /// T2.3 periodic sweep cursor — advances one batch per
    /// `audit_sweep_one_round()` call so callers can drip the
    /// authenticate-everything pass over many ticks instead of
    /// stalling on a 1024-entry HMAC sweep.
    size_t                    sweep_cursor_ = 0;

public:
    /// @brief T2.3 N series — read-only header accessor for the
    /// `on_nicctl_audit_thunk` (which counts Published entries via
    /// the wire-format flags). Returns nullptr on inert handles.
    [[nodiscard]] IcmpDirectoryHeader const* header_() const noexcept {
        return hdr_;
    }

    /// @brief T2.3 wiring (cold path). Flip the directory to keyed
    /// mode and stash the key for future sign/verify. Idempotent.
    /// Daemon calls this immediately after publishing a registry,
    /// supplying the same key written to `/run/eph/<bdf>.key`.
    /// `nullopt` disables (test-only path).
    void enable_hmac_(::eph::net::HmacSha256Key key) noexcept {
        if (hdr_ == nullptr) return;
        hmac_key_.emplace(std::move(key));
        hdr_->hmac_enabled = 1;
    }

    /// @brief T2.3 audit-on-suspicion: verify a single Published
    /// entry against the supplied key. Returns:
    ///   - `true` when entry is well-signed (or hmac_enabled==0
    ///     unkeyed mode → no-op true)
    ///   - `false` when tag mismatches → call site logs + drops
    ///     the dispatch as untrusted
    /// Constant-time comparison via `verify_icmp_entry`. Cost
    /// ~150-300 ns aarch64; **only** invoked from the
    /// rejection / error path (NEVER from the hot lookup loop).
    [[nodiscard]] bool
    audit_entry(size_t slot_idx) const noexcept {
        if (hdr_ == nullptr) return false;
        if (!hdr_->hmac_enabled) return true;  // unkeyed: no audit
        if (!hmac_key_.has_value()) return true;  // attached but no key
        if (slot_idx >= hdr_->entries.size()) return false;
        const auto& e = hdr_->entries[slot_idx];
        if (e.claimed.load(std::memory_order_acquire) !=
            kIcmpSlotPublished) {
            return true;  // free / in-progress: nothing to verify
        }
        return verify_icmp_entry(e, *hmac_key_);
    }

    /// @brief T2.3 periodic sweep — verify a batch of `batch_size`
    /// entries starting at the current sweep cursor. Caller invokes
    /// from a control thread at ~1 Hz; with 1024 entries / 64-batch
    /// sweep covers the full directory in 16 ticks (~16 seconds
    /// worst-case missed-detection window when paired with
    /// audit-on-error which closes the typical case in <1 ms).
    ///
    /// Returns the number of mismatches detected this round (0 in
    /// the healthy case). Each mismatch is also logged to the
    /// directory logger at WARN level so journalctl picks them up.
    ///
    /// Default `batch_size` chosen so a 1 Hz invoker covers all 1024
    /// entries in 16 seconds — operators tune by changing the call
    /// cadence or the batch size.
    size_t
    audit_sweep_one_round(size_t batch_size = 64) noexcept {
        if (hdr_ == nullptr) return 0;
        if (!hdr_->hmac_enabled) return 0;
        if (!hmac_key_.has_value()) return 0;
        const size_t total = hdr_->entries.size();
        size_t mismatches = 0;
        const size_t end = std::min(sweep_cursor_ + batch_size, total);
        for (size_t i = sweep_cursor_; i < end; ++i) {
            const auto& e = hdr_->entries[i];
            if (e.claimed.load(std::memory_order_acquire) !=
                kIcmpSlotPublished) continue;
            if (!verify_icmp_entry(e, *hmac_key_)) {
                ++mismatches;
                SPDLOG_WARN(
                    "IcmpDirectory: tamper detected at slot {} "
                    "(audit sweep) — proto={}, owner={}, "
                    "src={:08x}:{}, dst={:08x}:{}",
                    i, e.proto, e.owner_proc,
                    e.src_ip, e.src_port,
                    e.dst_ip, e.dst_port);
            }
        }
        sweep_cursor_ = (end >= total) ? 0 : end;
        return mismatches;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// IcmpDirectorySlotGuard — RAII slot release for handle composition
// ─────────────────────────────────────────────────────────────────────────────
//
// Owned by `Platform::IcmpTargetHandle` to make the directory
// slot's unregister run automatically alongside the local
// `IcmpRegistry::Handle` unregister. Holds a non-owning pointer to
// the directory; the directory itself is owned by `Platform::Impl`
// and outlives every Stream.

class IcmpDirectorySlotGuard {
public:
    IcmpDirectorySlotGuard() noexcept = default;

    IcmpDirectorySlotGuard(IcmpDirectoryHandle* dir, size_t slot_idx) noexcept
        : dir_(dir), slot_idx_(slot_idx), engaged_(true) {}

    ~IcmpDirectorySlotGuard() noexcept { release_(); }

    IcmpDirectorySlotGuard(const IcmpDirectorySlotGuard&) = delete;
    IcmpDirectorySlotGuard& operator=(const IcmpDirectorySlotGuard&) = delete;

    IcmpDirectorySlotGuard(IcmpDirectorySlotGuard&& o) noexcept
        : dir_(o.dir_), slot_idx_(o.slot_idx_), engaged_(o.engaged_) {
        o.engaged_ = false;
    }

    IcmpDirectorySlotGuard& operator=(IcmpDirectorySlotGuard&& o) noexcept {
        if (this != &o) {
            release_();
            dir_      = o.dir_;
            slot_idx_ = o.slot_idx_;
            engaged_  = o.engaged_;
            o.engaged_ = false;
        }
        return *this;
    }

    [[nodiscard]] bool engaged() const noexcept { return engaged_; }

private:
    void release_() noexcept {
        if (engaged_ && dir_) {
            dir_->unregister(slot_idx_);
        }
        engaged_ = false;
    }

    IcmpDirectoryHandle* dir_      = nullptr;
    size_t               slot_idx_ = 0;
    bool                 engaged_  = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// IPC payload + dispatch thunk
// ─────────────────────────────────────────────────────────────────────────────
//
// `IcmpDispatchMsg` is the wire format for cross-process ICMP Frag
// Needed forwarding. POD, trivially copyable, version-tagged.
//
// `g_active_icmp_directory` / `g_active_icmp_registry` are
// process-level pointers set by `Platform::create_*` when
// `mp_topology` is set. The DPDK `rte_mp_t` handler signature gives
// no context parameter, so these globals are how the static thunk
// finds its way back to the owning Platform's directory + registry.
// One Platform per process is the eph contract; multi-Platform
// processes (some unit tests) leave only the most-recently-created
// active.

struct alignas(8) IcmpDispatchMsg {
    uint8_t  version;         ///< schema version (= 1 currently)
    uint8_t  proto;           ///< IP protocol (TCP=6 / UDP=17)
    uint16_t reserved0;
    uint32_t src_ip;          ///< embedded packet's src IP, host order
    uint32_t dst_ip;          ///< embedded packet's dst IP, host order
    uint16_t src_port;        ///< embedded packet's src port, host order
    uint16_t dst_port;        ///< embedded packet's dst port, host order
    uint16_t next_hop_mtu;    ///< from ICMP Frag Needed
    uint16_t reserved1;
    uint32_t slot_idx;        ///< IcmpDirectory slot the secondary looked up
    uint32_t generation;      ///< gen at lookup; owner re-checks for stale drop
};
static_assert(sizeof(IcmpDispatchMsg) == 32);  // 28 bytes payload + alignas(8) pad
static_assert(std::is_trivially_copyable_v<IcmpDispatchMsg>);

inline constexpr std::string_view kIcmpDispatchActionName = "eph_icmp_dispatch";

/// @brief Process-level pointer to the active IcmpDirectory. Set by
/// `Platform::create` / `Platform::serve_nic`'s primary / secondary
/// bring-up paths when mp_topology is in effect; cleared on Platform
/// destruction. Loaded by the static dispatch thunk and by
/// `tcp_stream`'s closure.
inline std::atomic<IcmpDirectoryHandle*> g_active_icmp_directory{nullptr};

/// @brief Process-level pointer to the active IcmpRegistry. Same
/// lifecycle as `g_active_icmp_directory`.
inline std::atomic<IcmpRegistry*> g_active_icmp_registry{nullptr};

/// @brief This process's index in MpTopology. Used by the closure to
/// short-circuit "I am the owner" lookups (race window where the
/// directory says I own but local registry already moved on).
/// 0xFF = uninitialized / mp_topology not set.
inline std::atomic<uint8_t> g_active_self_proc_index{0xFF};

/// @brief DPDK rte_mp_t handler for incoming `eph_icmp_dispatch`
/// messages. Validates payload version + slot generation, rebuilds
/// the essentials of `ParsedIcmp`, and delivers to the local
/// `IcmpRegistry::dispatch`.
///
/// Best-effort: on any validation failure (size mismatch, version
/// mismatch, stale gen, no active registry) increments the
/// appropriate stats counter and returns 0 (DPDK ignores nonzero
/// returns from msg actions). Never throws. Never blocks beyond the
/// IcmpRegistry mutex.
inline int
on_icmp_dispatch_thunk(const struct rte_mp_msg* msg,
                       const void* /*peer*/) {
    auto* dir = g_active_icmp_directory.load(std::memory_order_acquire);
    auto* reg = g_active_icmp_registry.load(std::memory_order_acquire);
    if (dir == nullptr || reg == nullptr) {
        // Platform not yet up, or already torn down. Silent drop —
        // the IPC msg arrived in a transient window.
        return 0;
    }

    auto parsed_msg = parse_payload<IcmpDispatchMsg>(msg);
    if (!parsed_msg) {
        // size / null mismatch — not our msg version
        if (auto* hdr = dir->header()) {
            hdr->dropped_stale.fetch_add(1, std::memory_order_relaxed);
        }
        return 0;
    }
    const IcmpDispatchMsg& m = *parsed_msg;

    if (m.version != 1) {
        if (auto* hdr = dir->header()) {
            hdr->dropped_stale.fetch_add(1, std::memory_order_relaxed);
        }
        return 0;
    }

    if (auto* hdr = dir->header()) {
        hdr->ipc_msgs_received.fetch_add(1, std::memory_order_relaxed);
    }

    // Stale-handle gate: if the slot was unregistered between the
    // sender's lookup and our dispatch, drop silently.
    if (!dir->is_slot_alive(m.slot_idx, m.generation)) {
        if (auto* hdr = dir->header()) {
            hdr->dropped_stale.fetch_add(1, std::memory_order_relaxed);
        }
        return 0;
    }

    // Reconstruct just enough of ParsedIcmp for IcmpRegistry::dispatch.
    // The matcher only reads embedded_* + next_hop_mtu fields.
    ::eph::dpdk::net::ParsedIcmp parsed{};
    parsed.embedded_valid    = true;
    parsed.embedded_proto    = m.proto;
    parsed.embedded_src_ip   = m.src_ip;
    parsed.embedded_dst_ip   = m.dst_ip;
    parsed.embedded_src_port = m.src_port;
    parsed.embedded_dst_port = m.dst_port;
    parsed.next_hop_mtu      = m.next_hop_mtu;
    parsed.type              = 3;   // Destination Unreachable
    parsed.code              = 4;   // Frag Needed

    reg->dispatch(parsed);   // void; uses mu_ + UAF-safe callback path
    return 0;
}

/// @brief Build an IPC dispatch msg from a ParsedIcmp + directory
/// lookup result. Used by the secondary's Poller closure when local
/// dispatch misses but the directory finds an owner_proc != self.
[[nodiscard]] inline IcmpDispatchMsg
make_icmp_dispatch_msg(const ::eph::dpdk::net::ParsedIcmp& parsed,
                       size_t slot_idx,
                       uint32_t generation) noexcept {
    IcmpDispatchMsg m{};
    m.version      = 1;
    m.proto        = parsed.embedded_proto;
    m.src_ip       = parsed.embedded_src_ip;
    m.dst_ip       = parsed.embedded_dst_ip;
    m.src_port     = parsed.embedded_src_port;
    m.dst_port     = parsed.embedded_dst_port;
    m.next_hop_mtu = parsed.next_hop_mtu;
    m.slot_idx     = static_cast<uint32_t>(slot_idx);
    m.generation   = generation;
    return m;
}

} // namespace eph::dpdk::detail
