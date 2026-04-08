/// @file framework/signal.hpp
/// Signal handling and CPU pinning helpers shared by all bench binaries.

#pragma once

#include <atomic>
#include <csignal>
#include <cstdlib>

#include <spdlog/spdlog.h>

#include "eph/utils/cpu.hpp"

namespace bench {

inline std::atomic<bool> g_running{true};

namespace detail {
inline void sig_handler(int) {
    g_running.store(false, std::memory_order_relaxed);
}
} // namespace detail

inline void install_signal_handlers() {
    std::signal(SIGINT, detail::sig_handler);
    std::signal(SIGTERM, detail::sig_handler);
}

/// Pin current thread to `cpu`. Exits the process on failure (bench
/// should never silently run unpinned — measurement would be invalid).
inline void pin_or_die(int cpu, const char* name) {
    auto r = eph::utils::set_thread_affinity(cpu, name);
    if (!r) {
        spdlog::error("Failed to pin {} to core {}: {}", name, cpu, r.error());
        std::exit(1);
    }
    spdlog::info("Pinned {} to core {}", name, cpu);
}

} // namespace bench
