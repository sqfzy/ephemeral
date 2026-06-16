#pragma once

/// @file logger.hpp
/// @deprecated Transitional shim. The canonical logging surface is now
/// `eph/core/log.hpp` (`EPH_LOG_*` macros + `eph::log::get`). This header
/// remains only so not-yet-migrated call sites of `make_logger` keep compiling;
/// it is removed once every module is migrated. New code must use
/// `eph::log::get(...)` directly.

#include "eph/core/log.hpp"

namespace eph::core::detail {

/// @deprecated Use `eph::log::get` instead. Delegates to the canonical factory;
/// note the registered name is now prefixed with `eph.`.
[[nodiscard]] inline spdlog::logger* make_logger(const char* name) {
    return ::eph::log::get(name);
}

} // namespace eph::core::detail
