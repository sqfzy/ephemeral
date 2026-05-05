#pragma once

/// @file detail/hmac_keyed_entry.hpp
/// HMAC-SHA256 entry wrapper for hugepage-backed cross-process registries
/// (T2.3 from the 2026-05-05 action list, **skeleton only**).
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
/// The wrapper provides the *primitive* the eventual full deployment
/// will need: `HmacKeyedEntry<T>` carries `T` plus a 32-byte HMAC-SHA256
/// tag computed over `T`'s bytes under a daemon-distributed key.
/// `verify()` recomputes the tag in constant time and reports tamper.
/// The actual wiring into MpRegistry / IcmpDirectory / QueueAllocator is
/// left to a follow-up `pax --feat --deep` because it requires:
///
///   1. Designing the daemon→secondaries key distribution (read-only
///      `/run/eph/<bdf>.key` mode 0440 root-only?). Out of scope here.
///   2. Defining failure semantics: tamper detected → log + tear down
///      that secondary? Reset the slot? Quarantine the registry? Each
///      has cross-process consequences.
///   3. Performance check: every cross-process read becomes verify().
///      For control plane (cold path) this is fine; for ICMP dispatch
///      we'd want to verify only on suspicion (e.g. unusual fields).
///
/// What this header gives you today:
///   * `HmacKeyedEntry<T>` POD layout: `T data; uint8_t tag[32];`
///   * `compute_tag(span<uint8_t> bytes, key) -> Tag` — bytes-only,
///     no T involved (caller decides which bytes are authenticated).
///   * `verify(span<uint8_t> bytes, key, expected_tag) -> bool` —
///     constant-time comparison via `CRYPTO_memcmp`.
///   * `EPH_DPDK_ENABLE_HMAC` gating macro: when undefined (the
///     current default), the wrapper compiles but is unused; future
///     wiring will be `#if EPH_DPDK_ENABLE_HMAC` blocks in registry
///     code.
///
/// Test coverage: `tests/test_hmac_keyed_entry.cpp` (T2.3 skeleton).
/// Future test: `tests/test_mp_registry_hmac_tamper.cpp` once the
/// registries are actually wired (T3.3 — also skeleton-only today).

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
