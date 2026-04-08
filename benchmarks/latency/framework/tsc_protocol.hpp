/// @file framework/tsc_protocol.hpp
/// Shared helpers for the TSC timestamp protocol used by bench scenarios.
///
/// Two encoding formats are supported:
///
/// 1. Binary (raw UDP/TCP scenarios):
///      [0:8]   client_send_tsc  — uint64_t little-endian
///      [8:16]  server_recv_tsc  — uint64_t little-endian
///      [16:24] server_send_tsc  — uint64_t little-endian
///      [24:N]  payload padding
///
/// 2. JSON (WebSocket scenarios):
///      ..."T":<server_send_tsc>...      — server send timestamp
///      ..."T_recv":<server_recv_tsc>... — server recv timestamp
///      ..."T_send":<client_send_tsc>... — client send timestamp (echoed)
///
/// All TSC values are raw cycles; conversion to nanoseconds happens
/// at report time via eph::utils::TSC::to_ns().

#pragma once

#include <cstdint>
#include <string_view>

#include "eph/utils/time.hpp"

namespace bench::tsc {

// ── Binary protocol ──────────────────────────────────────────────────────

inline constexpr size_t kBinaryHeaderSize = 24;  // 3 × uint64_t

// ── JSON protocol ────────────────────────────────────────────────────────

/// Parse a uint64_t value following `key` in `json`.
/// Returns 0 on parse failure.
inline uint64_t parse_uint64_after(std::string_view json, std::string_view key) noexcept {
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

/// Parse the "T" field (server send TSC) from a JSON message.
/// Searches both `,"T":` (mid-object) and `{"T":` (start-of-object) to
/// avoid matching "T_recv" or "T_send".
inline uint64_t parse_T(const uint8_t* data, size_t len) noexcept {
    std::string_view json(reinterpret_cast<const char*>(data), len);
    uint64_t v = parse_uint64_after(json, ",\"T\":");
    if (v > 0) return v;
    return parse_uint64_after(json, "{\"T\":");
}

/// Parse the "T_recv" field (server receive TSC).
inline uint64_t parse_T_recv(const uint8_t* data, size_t len) noexcept {
    std::string_view json(reinterpret_cast<const char*>(data), len);
    return parse_uint64_after(json, "\"T_recv\":");
}

// ── TSC → nanoseconds ────────────────────────────────────────────────────

/// Convert TSC cycles to nanoseconds. Returns 0 if calibration failed.
inline uint64_t cycles_to_ns(uint64_t cycles) noexcept {
    auto opt = eph::utils::TSC::to_ns(cycles);
    return static_cast<uint64_t>(opt.value_or(0.0));
}

/// Convert nanoseconds to TSC cycles (inverse of cycles_to_ns).
/// Returns 0 if calibration failed.
inline uint64_t ns_to_cycles(uint64_t ns) noexcept {
    auto cycles_per_ns = eph::utils::TSC::to_ns(1).value_or(1.0);
    return static_cast<uint64_t>(static_cast<double>(ns) / cycles_per_ns);
}

} // namespace bench::tsc

namespace bench {
// Re-export for convenience (callers spell `bench::ns_to_cycles(2'000'000'000ULL)`)
using tsc::cycles_to_ns;
using tsc::ns_to_cycles;
} // namespace bench
