#pragma once

/// @file logger.hpp
/// Shared logger factory for the transport subsystem.
///
/// Provides get_or_create_logger() — a thread-safe, lazy-init helper
/// that eliminates the repeated pattern of spdlog::get() + fallback
/// spdlog::stdout_color_mt() found across transport detail headers.

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace eph::net::detail {

/// Get or create a named spdlog logger with colored stdout sink.
///
/// Thread-safe (spdlog's internal registry is mutex-protected).
/// Callers should cache the result in a function-local static to
/// avoid repeated registry lookups on the hot path.
///
/// @param name  Logger name (e.g., "transport.core", "transport.tls_enc")
/// @return Shared pointer to the logger (never null)
inline std::shared_ptr<spdlog::logger> get_or_create_logger(const char* name) {
    auto lg = spdlog::get(name);
    if (!lg) lg = spdlog::stdout_color_mt(name);
    return lg;
}

} // namespace eph::net::detail
