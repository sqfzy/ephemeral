#pragma once

/// @file log.hpp
/// The single compile-time logging gate + logger factory for the whole eph
/// library. Every eph header that logs includes THIS header (never
/// `<spdlog/spdlog.h>` directly) and emits through the `EPH_LOG_*` macros.
///
/// ## Default: SILENT
/// When `EPH_ENABLE_LOG` is **not** defined, every `EPH_LOG_*` macro expands to
/// `(void)0`. The library emits nothing, registers no logger, opens no sink, and
/// never touches the host application's stdout or its spdlog default logger.
/// Nothing is evaluated, so there is zero hot-path overhead — ideal for HFT
/// production. This guarantee is **independent of include order**: it does not
/// matter whether (or when) the consuming application includes `<spdlog/...>`
/// itself, and we never mutate the consumer's own `SPDLOG_ACTIVE_LEVEL`.
///
/// ## Opt in
/// Define `EPH_ENABLE_LOG` (e.g. `-DEPH_ENABLE_LOG`, or `xmake f --eph_log=y`)
/// to route eph logs to per-subsystem named loggers on stdout. Each log line
/// carries its source via spdlog's `%n` (e.g. `eph.net.dpdk.tcp`). Fine-tune the
/// verbosity by additionally defining `SPDLOG_ACTIVE_LEVEL`
/// (e.g. `-DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG`); when enabled and otherwise
/// unset it defaults to the most verbose level so the gate is the sole filter.

// When logging is enabled, default to the most verbose compile-time level so
// EPH_ENABLE_LOG alone controls output; the consumer can still narrow it by
// predefining SPDLOG_ACTIVE_LEVEL before including any eph header.
#if defined(EPH_ENABLE_LOG) && !defined(SPDLOG_ACTIVE_LEVEL)
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <string>
#include <string_view>

// The single gate. EPH_ENABLE_LOG — not SPDLOG_ACTIVE_LEVEL — decides whether an
// eph log site expands, so the "silent by default" guarantee holds regardless of
// what the consumer includes first.
#if defined(EPH_ENABLE_LOG)
#  define EPH_LOG_TRACE(logger, ...)    SPDLOG_LOGGER_TRACE((logger), __VA_ARGS__)
#  define EPH_LOG_DEBUG(logger, ...)    SPDLOG_LOGGER_DEBUG((logger), __VA_ARGS__)
#  define EPH_LOG_INFO(logger, ...)     SPDLOG_LOGGER_INFO((logger), __VA_ARGS__)
#  define EPH_LOG_WARN(logger, ...)     SPDLOG_LOGGER_WARN((logger), __VA_ARGS__)
#  define EPH_LOG_ERROR(logger, ...)    SPDLOG_LOGGER_ERROR((logger), __VA_ARGS__)
#  define EPH_LOG_CRITICAL(logger, ...) SPDLOG_LOGGER_CRITICAL((logger), __VA_ARGS__)
#else
// Disabled: expand to an unevaluated reference of every argument so that
// variables/parameters used *only* for logging do not trip -Wunused in
// consumer builds (the common case, since logging is off by default). The
// `false ? ... : (void)0` keeps the sink() call in a potentially-evaluated
// branch (marking args odr-used) while guaranteeing it is never actually
// executed — zero runtime cost and no side effects from the arguments. As a
// bonus, disabled log calls are still type-checked.
#  define EPH_LOG_TRACE(logger, ...)    (false ? ::eph::log::detail::sink((logger), __VA_ARGS__) : (void)0)
#  define EPH_LOG_DEBUG(logger, ...)    (false ? ::eph::log::detail::sink((logger), __VA_ARGS__) : (void)0)
#  define EPH_LOG_INFO(logger, ...)     (false ? ::eph::log::detail::sink((logger), __VA_ARGS__) : (void)0)
#  define EPH_LOG_WARN(logger, ...)     (false ? ::eph::log::detail::sink((logger), __VA_ARGS__) : (void)0)
#  define EPH_LOG_ERROR(logger, ...)    (false ? ::eph::log::detail::sink((logger), __VA_ARGS__) : (void)0)
#  define EPH_LOG_CRITICAL(logger, ...) (false ? ::eph::log::detail::sink((logger), __VA_ARGS__) : (void)0)
#endif

namespace eph::log::detail {

/// Sink for the disabled `EPH_LOG_*` branch: accepts the logger plus every log
/// argument by reference and does nothing. Invoked only from the never-taken
/// arm of a `false ? ... : (void)0`, so it marks all arguments odr-used
/// (suppressing -Wunused) without ever executing or evaluating them.
template <typename... Ts>
constexpr void sink(const Ts&...) noexcept {}

} // namespace eph::log::detail

namespace eph::log {

/// @brief Get-or-create the named eph logger for a subsystem (thread-safe,
/// idempotent). This is the ONE canonical factory — it replaces the former
/// `eph::core::detail::make_logger`, `eph::dpdk::detail::get_logger<LoggerName>`,
/// and the per-module hand-rolled `*_logger()` bodies.
///
/// @param name Subsystem name without prefix, e.g. `"net.dpdk.tcp"`. The logger
///   is registered as `"eph." + name` so a consumer can filter the whole library
///   under the `eph.*` namespace.
///
/// On first call for a given name it creates a color stdout logger (level set to
/// `trace` so the compile-time gate is the sole filter). Concurrent first calls
/// race on `stdout_color_mt`, which throws `spdlog_ex` for the loser — caught and
/// recovered via `spdlog::get`. Falls back to `spdlog::default_logger()`
/// (always non-null) so the returned pointer is never null. Intended to be cached
/// in a function-local `static` at each call site; only ever invoked when
/// `EPH_ENABLE_LOG` is defined (otherwise the `EPH_LOG_*` arg is unevaluated).
[[nodiscard]] inline spdlog::logger* get(std::string_view name) {
    std::string full = "eph.";
    full += name;
    auto lg = spdlog::get(full);
    if (!lg) {
        try {
            lg = spdlog::stdout_color_mt(full);
            lg->set_level(spdlog::level::trace);
        } catch (const spdlog::spdlog_ex&) {
            // Lost a creation race against another thread — recover the winner.
            lg = spdlog::get(full);
        }
    }
    if (!lg) {
        lg = spdlog::default_logger();
    }
    return lg.get();
}

} // namespace eph::log
