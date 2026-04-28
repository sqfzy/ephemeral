#pragma once

/// @file reconnect_logger.hpp
/// Per-module spdlog logger for `ReconnectOrchestrator`. Lives next to
/// the other transport-internal loggers (`tls_record_logger` etc.) and
/// follows the same lazy-initialized singleton pattern.

#include <spdlog/spdlog.h>

#include "eph/core/detail/logger.hpp"

namespace eph::net::detail {

/// @brief Lazily-initialized logger for `ReconnectOrchestrator`.
/// @return Pointer to the `"net.reconnect"` spdlog logger (process-singleton).
///
/// Same lifetime contract as the other detail loggers in `eph-net`: the
/// returned pointer is valid for the rest of the process; users must not
/// delete it. Compile-time level filtering is via `SPDLOG_ACTIVE_LEVEL`,
/// so DEBUG/TRACE calls compile out in release builds.
inline spdlog::logger* reconnect_logger() noexcept {
    static auto* l = ::eph::core::detail::make_logger("net.reconnect");
    return l;
}

} // namespace eph::net::detail
