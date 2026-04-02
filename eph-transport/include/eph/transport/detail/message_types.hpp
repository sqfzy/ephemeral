#pragma once

#include <cstdint>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "eph/utils/alignment.hpp"
#include "eph/transport/websocket.hpp"
#include "eph/transport/tls_record.hpp"

namespace eph::net {

// ---------------------------------------------------------------------------
// Internal message types for SPSC queue
// ---------------------------------------------------------------------------

namespace detail {

/// Message passed from application thread to TX thread via SPSC queue.
/// Fixed-size to satisfy TrivialData constraint.
/// tsc field is always present (fits in cache-line padding) but only
/// written/read when Transport::kEnableTimestamps is true.
template <size_t MaxPayload>
struct alignas(eph::utils::CACHE_LINE_SIZE) TxMessage {
    uint8_t  data[MaxPayload]{};
    uint16_t len = 0;
    uint8_t  opcode = ws::opcode::kBinary;
    uint64_t tsc = 0;  // enqueue-time TSC (unused when kEnableTimestamps=false)

    static_assert(MaxPayload > 0, "MaxPayload must be > 0");
    static_assert(MaxPayload <= tls_const::kMaxRecordPayload,
                  "MaxPayload exceeds TLS max record size");
};

/// Message passed from RX processing to application via SPSC queue.
/// tsc carries the arrival-time TSC (unused when kEnableTimestamps=false).
template <size_t MaxPayload>
struct alignas(eph::utils::CACHE_LINE_SIZE) RxMessage {
    uint8_t  data[MaxPayload]{};
    uint16_t len = 0;
    uint8_t  opcode = ws::opcode::kBinary;
    uint64_t tsc = 0;  // arrival-time TSC (unused when kEnableTimestamps=false)
};

inline spdlog::logger* transport_logger() {
    static auto l = [] {
        auto lg = spdlog::get("net.transport");
        if (!lg) lg = spdlog::stdout_color_mt("net.transport");
        // Inherit level from spdlog global default
        return lg;
    }();
    return l.get();
}

} // namespace detail

} // namespace eph::net
