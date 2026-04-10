#pragma once

/// @file detail/logger.hpp
/// Compile-time named logger factory for the dpdk subsystem.
///
/// Extracted from net_header.hpp so that lightweight headers (eal.hpp,
/// platform.hpp) can create loggers without pulling in DPDK packet headers.

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

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
/// Each unique Name produces a separate static local, so loggers are
/// created once per process (thread-safe via C++11 static-local guarantee).
/// Using a C++20 NTTP (non-type template parameter) string literal avoids
/// the runtime map lookup that a std::string_view parameter would need.
///
/// Usage: auto* log = get_logger<LoggerName{"dpdk.tcp"}>();
template <auto Name>
    requires requires { { Name.sv() } -> std::same_as<std::string_view>; }
[[nodiscard]] inline spdlog::logger* get_logger() {
    static auto l = [] {
        constexpr auto name = Name.sv();
        auto lg = spdlog::get(std::string{name});
        if (!lg) lg = spdlog::stdout_color_mt(std::string{name});
        return lg;
    }();
    return l.get();
}

} // namespace eph::dpdk::detail
