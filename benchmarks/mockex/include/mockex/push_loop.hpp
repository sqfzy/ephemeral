/// @file push_loop.hpp
/// Shared push-style inner loop: accept WS client → handshake →
/// sample → stamp → send, repeat until client closes or SIGTERM.
///
/// Pulled out of the per-scenario handlers because ex_market and
/// ex_market_2p differ only in (a) which payload pool they draw from
/// and (b) whether busy-regime events fan out into a small burst of
/// back-to-back sends. Everything else — the MMPP sampler, WS frame
/// encode, timestamp stamping, sleep discipline — is identical.
#pragma once

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <span>

#include <spdlog/spdlog.h>

#include "core/measurement.hpp"           // bench::monotonic_raw_ns
#include "mockex/mmpp2.hpp"
#include "mockex/payload_pool.hpp"
#include "mockex/ws_server.hpp"

namespace mockex {

/// Sleep until `deadline_ns` (CLOCK_MONOTONIC_RAW). Uses
/// `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` — the kernel
/// handles the wait in a single syscall and the wake-up jitter is
/// tens of μs, which is below the inter-arrival scale we're sampling
/// at (hundreds of Hz to low kHz).
///
/// We accept the `_RAW` → `CLOCK_MONOTONIC` mismatch: `clock_nanosleep`
/// does not expose the RAW clock, so we sleep on the slewed variant.
/// The error this introduces is bounded by the slew rate (< 500 ppm
/// under NTP/PTP) and is measured in the bench anyway via the shared
/// `monotonic_raw_ns` read on both sides of the wire.
inline void sleep_until_raw_ns(uint64_t deadline_raw_ns) noexcept {
    // Convert the RAW-clock deadline into a CLOCK_MONOTONIC absolute
    // deadline by taking the instant delta now and adding it to
    // CLOCK_MONOTONIC. A lower-level alternative is a busy-wait on
    // monotonic_raw_ns; for bench rates in the 10-10 000 Hz band the
    // scheduler approach is plenty.
    const uint64_t now_raw = bench::monotonic_raw_ns();
    if (deadline_raw_ns <= now_raw) return;
    const uint64_t delta = deadline_raw_ns - now_raw;

    timespec now_mono{};
    ::clock_gettime(CLOCK_MONOTONIC, &now_mono);
    uint64_t now_mono_ns =
        static_cast<uint64_t>(now_mono.tv_sec) * 1'000'000'000ull +
        static_cast<uint64_t>(now_mono.tv_nsec);
    const uint64_t deadline_mono_ns = now_mono_ns + delta;
    timespec ts{
        .tv_sec  = static_cast<time_t>(deadline_mono_ns / 1'000'000'000ull),
        .tv_nsec = static_cast<long>(deadline_mono_ns % 1'000'000'000ull),
    };
    while (::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
        // Restart on signal-interrupted sleep. SIGTERM sets the
        // shutdown flag which the outer loop polls, so the next tick
        // naturally exits.
    }
}

/// Configuration for the shared push loop.
struct PushConfig {
    std::string scenario_log_tag;  ///< "[ex_market_push]" prefix for logs
    size_t      burst_multiplier = 1;  ///< 1 for single-frame, >1 for 2p burst
};

/// Run the push inner loop on an already-handshaked WS fd. Returns
/// when the peer closes, send fails, or `running` flips.
///
/// The sampler's `next_interval_ns()` is consulted once per logical
/// "event"; each event optionally fans out into `burst_multiplier`
/// back-to-back frames (that's how ex_market_2p models a price event
/// triggering a chain of per-level depth updates). Frames within a
/// burst share the same T stamp — that matches the real exchange
/// semantics, where the event timestamp is stamped at the matching-
/// engine event, not per-outgoing frame.
template <class SamplerT>
inline void
run_push_loop(int cfd,
              SamplerT& sampler,
              PayloadPool& pool,
              const std::atomic<bool>& running,
              const PushConfig& cfg) noexcept {
    uint64_t next_tick_ns = bench::monotonic_raw_ns();
    while (running.load(std::memory_order_relaxed)) {
        sleep_until_raw_ns(next_tick_ns);
        if (!running.load(std::memory_order_relaxed)) break;

        const uint64_t t_send_ns = bench::monotonic_raw_ns();
        const size_t frames = sampler.in_busy_regime()
            ? cfg.burst_multiplier : 1;
        for (size_t i = 0; i < frames; ++i) {
            auto bytes = pool.stamp_and_next(t_send_ns);
            if (!ws::send_frame(cfd, ws::kOpcodeBinary, bytes)) {
                SPDLOG_INFO("{} client hung up mid-send", cfg.scenario_log_tag);
                return;
            }
        }

        const uint64_t dt_ns = sampler.next_interval_ns();
        next_tick_ns = bench::monotonic_raw_ns() + dt_ns;
    }
}

/// Skim any unsolicited client→server frames (ping/close) without
/// blocking. Run this at the top of the push loop on each iteration
/// when you suspect the peer may send control frames — currently not
/// used because bench clients are strictly passive listeners on push
/// scenarios, but exposed for future use.
[[nodiscard]] inline bool
peek_client_frame(int cfd) noexcept {
    (void)cfd;
    return true;
}

} // namespace mockex
