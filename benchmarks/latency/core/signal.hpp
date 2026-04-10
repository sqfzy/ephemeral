#pragma once

/// @file core/signal.hpp
/// Thin compat shim — the real implementation lives in
/// eph-utils/include/eph/utils/shutdown_signal.hpp.
///
/// Bench code uses the unqualified `g_running` and `install_signal_handlers`
/// names (via `using namespace bench`) so this shim aliases the canonical
/// eph::utils symbols into the bench namespace to keep callers unchanged.
/// New consumers should #include "eph/utils/shutdown_signal.hpp" directly.

#include "eph/utils/shutdown_signal.hpp"

namespace bench {

inline auto& g_running = ::eph::utils::g_shutdown_flag;

inline void install_signal_handlers() noexcept {
    ::eph::utils::install_shutdown_handlers();
}

} // namespace bench
