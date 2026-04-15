#pragma once

/// @file detail/logger.hpp
/// Compile-time named logger factory for the dpdk subsystem.
///
/// Extracted from net_header.hpp so that lightweight headers (eal.hpp,
/// platform.hpp) can create loggers without pulling in DPDK packet headers.
///
/// The NTTP `get_logger<LoggerName{"..."}>()` surface is preserved for the
/// legacy `eph::dpdk::*` headers. Underneath it now delegates to the shared
/// `eph::core::detail::make_logger` factory (commit 600a51b) so race
/// conditions around `stdout_color_mt` creation are handled uniformly across
/// every module in the project.

#include <algorithm>
#include <cstddef>
#include <string_view>

#include <spdlog/spdlog.h>

#include "eph/core/detail/logger.hpp"

namespace eph::dpdk::detail {

/// Compile-time string literal wrapper for NTTP logger names.
/// Enables `get_logger<LoggerName{"dpdk.tcp"}>()` syntax with C++20 class NTTP.
template <size_t N>
struct LoggerName {
    char data[N]{};
    constexpr LoggerName(const char (&s)[N]) { std::copy_n(s, N, data); }
    [[nodiscard]] constexpr std::string_view sv() const { return {data, N - 1}; }
};

/// Get-or-create a named spdlog logger with colored stdout sink.
///
/// Each unique `Name` produces a separate static local, so loggers are
/// created once per process (thread-safe via the C++11 static-local
/// guarantee). The creation path delegates to
/// `eph::core::detail::make_logger`, which catches the
/// `spdlog_ex("logger already exists")` race that would otherwise occur if
/// the same logger name is claimed concurrently from another TU.
///
/// Usage: auto* log = get_logger<LoggerName{"dpdk.tcp"}>();
template <auto Name>
    requires requires { { Name.sv() } -> std::same_as<std::string_view>; }
[[nodiscard]] inline spdlog::logger* get_logger() {
    static spdlog::logger* const l = [] {
        // The NTTP string is null-terminated in-place, so Name.data is
        // already a valid C string — no std::string allocation needed.
        return ::eph::core::detail::make_logger(Name.data);
    }();
    return l;
}

} // namespace eph::dpdk::detail
