#pragma once

/// @file packet_view.hpp
/// Formal `PacketView` concept — the duck-typed contract between
/// Stream / Datagram backends and the codec layer.
///
/// Part of Phase 5 of the v3.3 refactor (see
/// .artifacts/design-eph-v3.3-architecture-20260410.md). Phases 1–4
/// implemented the codecs and backends against the same informal surface
/// (documented in `codec.hpp`'s file-level comment); Phase 5 formalizes it
/// here so that every PacketView implementation can be compile-time
/// verified via `static_assert(PacketView<T>)`.
///
/// ## Contract
///
/// A PacketView is a lightweight, non-owning cursor over a contiguous byte
/// window of a received packet. It is handed from the Stream / Datagram
/// backend to the codec inside the RX hot path and is the codec's sole
/// view of the payload bytes — codecs MUST NOT retain pointers derived
/// from a PacketView across the decode call that received it, because the
/// underlying storage (kernel reassembly buffer or DPDK mbuf) may be
/// recycled immediately after decode returns.
///
/// ## Required operations
///
/// ```cpp
///   uint8_t*       writable_data() noexcept;  // mutating cursor (in-place TLS)
///   const uint8_t* data() const noexcept;     // read-only cursor
///   size_t         length() const noexcept;   // bytes remaining in window
///   void           trim_front(size_t n) noexcept;  // skb_pull equivalent
///   void           trim_back(size_t n)  noexcept;  // skb_trim equivalent
///   uint64_t       arrival_tsc() const noexcept;   // NIC-arrival TSC (0 if n/a)
/// ```
///
/// ## Implementations
///
/// - `eph::codec::SpanPacketView`             — generic span-backed (codec tests)
/// - `eph::net::kernel::detail::SpanView`     — kernel backend (reassembly buffer)
/// - `eph::net::dpdk::detail::MbufView`       — DPDK backend (raw mbuf payload)
///
/// The DPDK `MbufView` is the critical one: since it points directly into a
/// `rte_mbuf` payload, the codec's `writable_data()` returns the same pointer
/// the NIC DMA'd into, which lets the TLS layer decrypt **in place** via
/// `EVP_AEAD_CTX_open` (aws-lc supports in == out for AES-GCM). That is the
/// headline zero-copy property behind the whole v3.3 design.

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace eph::core {

/// @brief Compile-time check that `T` satisfies the PacketView contract.
///
/// Note: `trim_front` / `trim_back` are required to be `noexcept`, but the
/// concept only checks existence + signature; a static_assert on
/// `noexcept(declval<T&>().trim_front(0))` is performed at implementation
/// sites so the concept stays permissive for mock types in unit tests that
/// may forward through a logging wrapper.
template <class T>
concept PacketView = requires(T& v, const T& cv, std::size_t n) {
    // Read/write data cursors
    { v.writable_data() }  -> std::convertible_to<std::uint8_t*>;
    { cv.data() }          -> std::convertible_to<const std::uint8_t*>;

    // Window length
    { cv.length() }        -> std::convertible_to<std::size_t>;

    // Cursor manipulation (skb_pull / skb_trim equivalents).
    // Return type is not constrained (void), which `requires` expresses
    // with a simple statement-expression.
    v.trim_front(n);
    v.trim_back(n);

    // NIC-arrival TSC (0 if untracked).
    { cv.arrival_tsc() }   -> std::convertible_to<std::uint64_t>;
};

} // namespace eph::core
