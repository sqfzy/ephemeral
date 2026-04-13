#pragma once

/// @file socket_addr.hpp
/// IPv4 + SocketAddr value types for the `eph::net` concept layer.
///
/// Provides the minimal
/// address value types used by the new `Stream` / `Datagram` / `Poller`
/// concepts. Deliberately minimal: IPv4 only, no resolver, no syscalls — the
/// kernel / dpdk backends layer their own address-family plumbing on top.
///
/// Design rationale:
///   - Header-only, trivially copyable, constexpr-friendly where possible so
///     addresses can live in constant-initialized tables (e.g. for market-data
///     multicast groups).
///   - `parse` returns `std::expected<Ipv4Addr, core::ErrorInfo>` rather than
///     throwing, per the project-wide fallible-API convention.
///   - No network-byte-order helpers here beyond `from_be32` — the underlying
///     wire format is normalized away: octets are stored in host-native order
///     (big-endian numerically: `octets[0]` is the high octet) so that textual
///     round-trip and comparisons are obvious.

#include <array>
#include <bit>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <string>
#include <string_view>

#include "eph/core/error.hpp"

namespace eph::net {

// ---------------------------------------------------------------------------
// Ipv4Addr
// ---------------------------------------------------------------------------

/// @brief IPv4 address as 4 octets in the textual order `a.b.c.d`.
///
/// Storage: `octets[0]` is the most-significant (leftmost) octet. This is the
/// opposite of `struct in_addr`'s `s_addr` layout on little-endian hosts but
/// matches the order a human reads in `127.0.0.1`. Backends that need a
/// `uint32_t` in network byte order should call `to_be32()`.
struct Ipv4Addr {
    /// Octets in textual order: `octets[0] = a` for "a.b.c.d".
    std::array<uint8_t, 4> octets{0, 0, 0, 0};

    constexpr Ipv4Addr() noexcept = default;

    /// @brief Construct from 4 octets in textual order.
    constexpr Ipv4Addr(uint8_t a, uint8_t b, uint8_t c, uint8_t d) noexcept
        : octets{a, b, c, d} {}

    /// @brief Construct from a big-endian 32-bit address (network byte order).
    /// @param be  the address as it would appear in `struct in_addr::s_addr`
    ///            on a big-endian host (i.e. `(a<<24)|(b<<16)|(c<<8)|d`).
    [[nodiscard]] static constexpr Ipv4Addr from_be32(uint32_t be) noexcept {
        return Ipv4Addr{
            static_cast<uint8_t>((be >> 24) & 0xFF),
            static_cast<uint8_t>((be >> 16) & 0xFF),
            static_cast<uint8_t>((be >>  8) & 0xFF),
            static_cast<uint8_t>( be        & 0xFF),
        };
    }

    /// @brief Convenience factory for constexpr contexts.
    [[nodiscard]] static constexpr Ipv4Addr from_octets(uint8_t a, uint8_t b,
                                                        uint8_t c, uint8_t d) noexcept {
        return Ipv4Addr{a, b, c, d};
    }

    /// @brief Encode back to a big-endian 32-bit value.
    [[nodiscard]] constexpr uint32_t to_be32() const noexcept {
        return (static_cast<uint32_t>(octets[0]) << 24)
             | (static_cast<uint32_t>(octets[1]) << 16)
             | (static_cast<uint32_t>(octets[2]) <<  8)
             |  static_cast<uint32_t>(octets[3]);
    }

    /// @brief Parse a dotted-quad string "a.b.c.d".
    ///
    /// Strictly rejects:
    ///   - empty input
    ///   - leading/trailing whitespace
    ///   - components > 255
    ///   - components with non-digit characters
    ///   - fewer or more than 4 components
    ///   - empty components (e.g. "1..2.3", "1.2.3.", ".1.2.3")
    ///
    /// Note: we deliberately do NOT accept legacy short forms like "127.1"
    /// that `inet_aton` accepts — those are footguns on modern software.
    [[nodiscard]] static std::expected<Ipv4Addr, core::ErrorInfo>
    parse(std::string_view s) noexcept {
        if (s.empty()) {
            return std::unexpected(core::ErrorInfo{core::Error::InvalidConfig,
                                                    "ipv4 parse: empty input"});
        }
        std::array<uint8_t, 4> out{};
        size_t component = 0;
        size_t i = 0;
        while (component < 4) {
            // Parse one decimal number in [0, 255] with no leading '+' or spaces.
            if (i >= s.size() || s[i] < '0' || s[i] > '9') {
                return std::unexpected(core::ErrorInfo{
                    core::Error::InvalidConfig,
                    "ipv4 parse: expected digit"});
            }
            uint32_t val = 0;
            size_t digits = 0;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
                val = val * 10 + static_cast<uint32_t>(s[i] - '0');
                if (val > 255) {
                    return std::unexpected(core::ErrorInfo{
                        core::Error::InvalidConfig,
                        "ipv4 parse: octet out of range"});
                }
                ++i;
                ++digits;
                if (digits > 3) {
                    return std::unexpected(core::ErrorInfo{
                        core::Error::InvalidConfig,
                        "ipv4 parse: too many digits in octet"});
                }
            }
            out[component++] = static_cast<uint8_t>(val);
            if (component < 4) {
                // Expect a dot separator before the next component.
                if (i >= s.size() || s[i] != '.') {
                    return std::unexpected(core::ErrorInfo{
                        core::Error::InvalidConfig,
                        "ipv4 parse: missing dot separator"});
                }
                ++i;
            }
        }
        if (i != s.size()) {
            // Trailing garbage after 4th component.
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "ipv4 parse: trailing garbage"});
        }
        return Ipv4Addr{out[0], out[1], out[2], out[3]};
    }

    /// @brief Render as "a.b.c.d".
    [[nodiscard]] std::string to_string() const {
        // 15 chars max ("255.255.255.255") + NUL.
        char buf[16];
        int n = std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                              static_cast<unsigned>(octets[0]),
                              static_cast<unsigned>(octets[1]),
                              static_cast<unsigned>(octets[2]),
                              static_cast<unsigned>(octets[3]));
        return (n > 0) ? std::string{buf, static_cast<size_t>(n)} : std::string{};
    }

    [[nodiscard]] friend constexpr bool operator==(const Ipv4Addr&,
                                                   const Ipv4Addr&) noexcept = default;
};

// ---------------------------------------------------------------------------
// SocketAddr
// ---------------------------------------------------------------------------

/// @brief IPv4 endpoint: address + port (host byte order).
struct SocketAddr {
    Ipv4Addr ip{};
    uint16_t port{0};

    constexpr SocketAddr() noexcept = default;
    constexpr SocketAddr(Ipv4Addr a, uint16_t p) noexcept : ip(a), port(p) {}

    /// @brief Render as "a.b.c.d:port".
    [[nodiscard]] std::string to_string() const {
        auto s = ip.to_string();
        s.push_back(':');
        s.append(std::to_string(port));
        return s;
    }

    [[nodiscard]] friend constexpr bool operator==(const SocketAddr&,
                                                   const SocketAddr&) noexcept = default;
};

} // namespace eph::net
