#pragma once

/// @file logger.hpp
/// Shared lazy-init logger factory — eliminates repeated boilerplate across
/// eph-net, eph-net-kernel, and eph-net-dpdk modules.

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace eph::core::detail {

/// @brief Get or create a named spdlog logger (thread-safe, idempotent).
///
/// On first call for a given `name`, creates a color stdout logger.
/// Subsequent calls return the cached instance. Safe against concurrent
/// creation (spdlog::stdout_color_mt throws spdlog_ex if the name is
/// already registered — we catch that and fall back to spdlog::get).
///
/// Typical usage at each call site:
/// @code
///   inline spdlog::logger* my_logger() {
///       static auto* l = eph::core::detail::make_logger("my.subsystem");
///       return l;
///   }
/// @endcode
[[nodiscard]] inline spdlog::logger* make_logger(const char* name) {
    auto lg = spdlog::get(name);
    if (!lg) {
        try {
            lg = spdlog::stdout_color_mt(name);
        } catch (const spdlog::spdlog_ex&) {
            lg = spdlog::get(name);
        }
    }
    // Defense-in-depth: every call site dereferences this raw pointer
    // via SPDLOG_LOGGER_*(l, ...) without a null guard, so a nullptr
    // here would crash any first log call. The only path that can
    // surface nullptr is a race where another thread `spdlog::drop`s
    // the name between the throwing `stdout_color_mt` and the
    // recovery `spdlog::get` — vanishingly unlikely but possible.
    // Fall back to spdlog::default_logger() (always non-null) so the
    // worst-case outcome is "log to default sink" rather than crash.
    if (!lg) {
        lg = spdlog::default_logger();
    }
    return lg.get();
}

} // namespace eph::core::detail
