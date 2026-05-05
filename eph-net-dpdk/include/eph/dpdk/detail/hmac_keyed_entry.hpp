#pragma once

/// @file detail/hmac_keyed_entry.hpp
/// HMAC-SHA256 entry wrapper for hugepage-backed cross-process registries
/// (originally T2.3 skeleton from the 2026-05-05 action list — see
/// **Status update below** for the wiring outcome).
///
/// Threat model the wrapper addresses:
///   The hugepage POD layouts that `MpRegistry` / `IcmpDirectory` /
///   `QueueAllocator` use are mapped read-write into every secondary
///   process attached to a NIC. A compromised secondary (or one with a
///   wild-pointer write that lands in the shared segment) can tamper
///   with another secondary's registered ICMP target, queue claim, or
///   process slot — and there is no integrity check that would surface
///   the tamper. The 2026-05-05 review classified this as a Tier 2
///   risk for multi-LP / multi-strategy deployments (single-LP single-
///   strategy assumes all secondaries in the same trust domain).
///
/// **Status update (2026-05-05 K/L/M/N/O/P series, post-skeleton)**:
/// The production wiring shipped, but the registries did not adopt
/// `HmacKeyedEntry<T>` directly — each registry instead embeds a
/// `hmac_tag[32]` field in its existing POD layout and defines its
/// own `pack_*_for_hmac` / `sign_*_in_place` / `verify_*` helpers
/// over an explicit little-endian payload. This avoided a
/// hugepage-wire-format break (the v1 entry layouts predate
/// `HmacKeyedEntry`) and let each registry pick which bytes are
/// authenticated (e.g. `IcmpDirectoryEntry` excludes `claimed` and
/// `generation` because they mutate per claim/release).
///
/// Resolved design questions (originally out-of-scope here):
///   1. Daemon→secondaries key distribution: shipped via
///      `eph/dpdk/detail/registry_hmac_key.hpp` —
///      `/run/eph/<sanitize_bdf(pci)>.key` mode 0440 root-owned.
///   2. Failure semantics: `verify_*` returns false → audit caller
///      logs `WARN`, increments mismatch counter, drops the
///      individual dispatch. Periodic 1 Hz `audit_sweep_one_round`
///      catches passive tampering. Daemon does NOT tear down
///      secondaries — operator decides via Prometheus alerts.
///   3. Performance: hot lookup paths NEVER verify; verify is
///      audit-on-suspicion + periodic sweep on a control thread.
///      `bench_registry_hmac.cpp` measures the actual cost.
///
/// What this header still provides:
///   * `HmacKeyedEntry<T>` POD layout (generic primitive — usable
///     for future registries that haven't yet picked their own
///     authenticated-payload schema).
///   * `compute_tag(span<uint8_t> bytes, key) -> Tag` — bytes-only,
///     no T involved (caller decides which bytes are authenticated).
///   * `verify(span<uint8_t> bytes, key, expected_tag) -> bool` —
///     constant-time comparison via `CRYPTO_memcmp`.
///
/// Production callers: none today. The 3 shipped registries
/// (MpRegistry / QueueAllocator / IcmpDirectory) carry their own
/// per-type pack/sign/verify helpers. This header survives as a
/// reference primitive + as the test target for
/// `tests/test_hmac_keyed_entry.cpp` (sign/verify round-trip
/// invariants) and `tests/test_hmac_tamper_simulation.cpp` (T3.3
/// closed — 5 fuzz cases × 3500 trials).
///
/// The previous `EPH_DPDK_ENABLE_HMAC` compile-time gating macro
/// was never adopted; the registries use a runtime
/// `header.hmac_enabled` flag so single-tenant deployments (no
/// HMAC) and multi-tenant (HMAC on) coexist on the same build.

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

#include <openssl/mem.h>  // CRYPTO_memcmp (aws-lc)

#include "eph/net/hmac.hpp"

namespace eph::dpdk::detail {

inline constexpr size_t kHmacKeyedEntryTagBytes = 32;

/// @brief POD wrapper carrying `T` plus a 32-byte HMAC-SHA256 tag over
///        `T`'s on-wire byte representation.
///
/// `T` MUST be trivially copyable so its bytes can be authenticated
/// directly. The wrapper does not interpret `T` — the caller computes
/// the tag over a `std::span<const uint8_t>` view of `T`'s storage,
/// which means the same wrapper is reusable across every registry
/// type that needs entry integrity.
///
/// Layout: `T` first (so existing readers can treat the prefix as the
/// old POD when running with `EPH_DPDK_ENABLE_HMAC` off — but **not**
/// when on, because the registry would need a wire-format bump in any
/// case). Tag last for natural cache-line packing.
template <class T>
struct HmacKeyedEntry {
    static_assert(std::is_trivially_copyable_v<T>,
                  "HmacKeyedEntry requires T to be trivially copyable so "
                  "its bytes can be authenticated directly");

    T                                       data{};
    std::array<uint8_t, kHmacKeyedEntryTagBytes> tag{};
};

/// @brief Compute the HMAC tag over `bytes` under `key`.
///
/// Single-shot: forwards to `eph::net::hmac_sha256_sign`. Returns the
/// 32-byte tag as an array (caller copies into the entry's `tag` field).
/// Allocation-free, noexcept.
[[nodiscard]] inline std::array<uint8_t, kHmacKeyedEntryTagBytes>
compute_tag(std::span<const uint8_t>         bytes,
            const eph::net::HmacSha256Key&   key) noexcept {
    const auto sig = eph::net::hmac_sha256_sign(key, bytes);
    return sig.bytes;
}

/// @brief Verify `expected_tag` matches `HMAC(key, bytes)` in constant
///        time.
///
/// Constant-time comparison via aws-lc's `CRYPTO_memcmp` (the same
/// primitive aws-lc itself uses for AEAD tag comparison). Returns
/// `true` iff the tag matches.
[[nodiscard]] inline bool
verify_tag(std::span<const uint8_t>                                bytes,
           const eph::net::HmacSha256Key&                          key,
           const std::array<uint8_t, kHmacKeyedEntryTagBytes>&     expected_tag)
    noexcept {
    const auto actual = compute_tag(bytes, key);
    return CRYPTO_memcmp(actual.data(), expected_tag.data(),
                         actual.size()) == 0;
}

/// @brief Helper: span-view of `entry.data`'s bytes for tag input.
///
/// Reinterpret-cast through `uint8_t const*` is well-defined for any
/// trivially-copyable type per [basic.types.general] — the alternative
/// of `std::bit_cast` to a `std::array<uint8_t, sizeof(T)>` would force
/// a full copy on every verify, which is wasteful in a hot-path
/// validation loop.
template <class T>
[[nodiscard]] inline std::span<const uint8_t>
data_bytes_view(const HmacKeyedEntry<T>& entry) noexcept {
    return std::span<const uint8_t>{
        reinterpret_cast<const uint8_t*>(&entry.data),
        sizeof(T)
    };
}

/// @brief Sign-in-place: compute `entry.tag = HMAC(key, entry.data bytes)`.
///
/// Convenience for primary-side write paths. Caller fills `entry.data`
/// then calls this; subsequent secondary readers verify by recomputing.
template <class T>
inline void
sign_entry(HmacKeyedEntry<T>&            entry,
           const eph::net::HmacSha256Key& key) noexcept {
    entry.tag = compute_tag(data_bytes_view(entry), key);
}

/// @brief Verify-in-place: returns `true` iff `entry.tag` is the valid
///        HMAC of `entry.data` under `key`.
template <class T>
[[nodiscard]] inline bool
verify_entry(const HmacKeyedEntry<T>&       entry,
             const eph::net::HmacSha256Key& key) noexcept {
    return verify_tag(data_bytes_view(entry), key, entry.tag);
}

}  // namespace eph::dpdk::detail
