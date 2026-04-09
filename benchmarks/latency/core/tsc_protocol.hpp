/// @file core/tsc_protocol.hpp
/// Wire-level TSC stamp protocol used by every scenario.
///
/// Two encodings are supported, both stamping the same three TSC values:
///
///   1. Binary header (24 B, used by raw TCP/UDP scenarios):
///        [0:8]   client_send_tsc   uint64_t little-endian
///        [8:16]  server_recv_tsc   uint64_t little-endian
///        [16:24] server_send_tsc   uint64_t little-endian
///        [24:N]  payload padding
///
///   2. JSON fields (used by WebSocket scenarios):
///        "T"      : server_send_tsc
///        "T_recv" : server_recv_tsc
///        "T_send" : client_send_tsc (echoed by server in responses)
///
/// `parse_T` deliberately searches both `,"T":` and `{"T":` so it does
/// not accidentally match `T_recv` / `T_send`.
#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

#include "eph/utils/time.hpp"

namespace bench::tsc {

// ── Binary protocol ──────────────────────────────────────────────────────

inline constexpr size_t kBinaryHeaderSize = 24;

/// Stamp a 24-byte binary header at the start of `buf`.
/// `buf` must point to at least `kBinaryHeaderSize` writable bytes.
inline void stamp_binary(uint8_t* buf,
                         uint64_t client_send,
                         uint64_t server_recv,
                         uint64_t server_send) noexcept {
    std::memcpy(buf,      &client_send, 8);
    std::memcpy(buf + 8,  &server_recv, 8);
    std::memcpy(buf + 16, &server_send, 8);
}

/// Read three TSC values from a 24-byte binary header.
/// `buf` must point to at least `kBinaryHeaderSize` readable bytes.
inline void parse_binary(const uint8_t* buf,
                         uint64_t& client_send,
                         uint64_t& server_recv,
                         uint64_t& server_send) noexcept {
    std::memcpy(&client_send, buf,      8);
    std::memcpy(&server_recv, buf + 8,  8);
    std::memcpy(&server_send, buf + 16, 8);
}

// ── JSON protocol ────────────────────────────────────────────────────────

namespace detail {

/// Parse the unsigned integer that follows `key` in `json`. Returns 0 on
/// failure. Linear scan, no allocation — designed for hot-path use.
inline uint64_t parse_uint64_after(std::string_view json,
                                   std::string_view key) noexcept {
    auto pos = json.find(key);
    if (pos == std::string_view::npos) return 0;
    pos += key.size();
    while (pos < json.size() && json[pos] == ' ') ++pos;
    uint64_t val = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        val = val * 10 + static_cast<uint64_t>(json[pos] - '0');
        ++pos;
    }
    return val;
}

} // namespace detail

/// Parse the "T" field (server send TSC). Searches both `,"T":` and
/// `{"T":` so it never confuses "T_recv" / "T_send".
inline uint64_t parse_T(const uint8_t* data, size_t len) noexcept {
    std::string_view json(reinterpret_cast<const char*>(data), len);
    if (auto v = detail::parse_uint64_after(json, ",\"T\":"); v > 0) return v;
    return detail::parse_uint64_after(json, "{\"T\":");
}

/// Parse the "T_recv" field (server recv TSC).
inline uint64_t parse_T_recv(const uint8_t* data, size_t len) noexcept {
    std::string_view json(reinterpret_cast<const char*>(data), len);
    return detail::parse_uint64_after(json, "\"T_recv\":");
}

/// Parse the "T_send" field (client send TSC, echoed by server).
inline uint64_t parse_T_send(const uint8_t* data, size_t len) noexcept {
    std::string_view json(reinterpret_cast<const char*>(data), len);
    return detail::parse_uint64_after(json, "\"T_send\":");
}

// ── TSC ↔ ns conversion convenience ──────────────────────────────────────

/// Convert raw TSC cycles to nanoseconds. Returns 0 if TSC was never
/// calibrated (callers MUST check `eph::utils::TSC::is_initialized()`
/// before measurement; this 0 fallback is for non-hot reporting paths).
inline uint64_t cycles_to_ns(uint64_t cycles) noexcept {
    auto ns = eph::utils::TSC::to_ns(cycles);
    return ns ? static_cast<uint64_t>(*ns) : 0;
}

/// Convert nanoseconds to TSC cycles. Returns 0 if not calibrated.
inline uint64_t ns_to_cycles(uint64_t ns) noexcept {
    auto c = eph::utils::TSC::to_cycles(static_cast<double>(ns));
    return c.value_or(0);
}

} // namespace bench::tsc
