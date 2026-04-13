#pragma once

/// @file kill_switch.hpp
/// Irreversible, thread-safe kill switch for HFT risk/compliance controls.
///
/// Typical usage:
///
/// @code
///   eph::utils::KillSwitch ks{[] {
///       SPDLOG_ERROR("kill switch tripped — cancelling open orders");
///       cancel_all();
///   }};
///
///   // Hot loop
///   while (!ks.tripped()) {
///       process_events();
///   }
///
///   // Trip from any thread — idempotent, callback fires exactly once.
///   ks.trip();
/// @endcode
///
/// Design note: there is **no** `reset()` / `untrip()` / `clear()` method.
/// A kill switch is a one-way action by design (compliance / risk).  If
/// you need a reset-able flag, use `std::atomic<bool>` directly — that is
/// simply a different abstraction.
///

#include <atomic>
#include <functional>
#include <utility>

#include <spdlog/spdlog.h>

namespace eph::utils {

/// Irreversible kill switch.  Once tripped, stays tripped forever.
///
/// - Single-fire: the optional `on_trip` callback runs at most once even
///   if `trip()` is called from many threads.
/// - Thread-safe: `trip()` uses an acquire-release CAS; `tripped()` uses
///   an acquire load, so observers synchronize-with the tripping thread.
/// - `noexcept` throughout.  The callback itself must not throw; any
///   exception escaping the callback terminates (consistent with
///   `noexcept` semantics and the codebase-wide `SPDLOG_NO_EXCEPTIONS`).
///
/// Not copyable, not movable (holds an atomic and a bound callback that
/// would need re-binding on move; the extra complexity buys nothing).
class KillSwitch {
public:
    /// Construct a kill switch in the untripped state.
    /// @param on_trip Optional callback invoked exactly once on the thread
    ///                that wins the CAS in the first `trip()` call.  Pass
    ///                `nullptr` (or omit) for poll-only usage.
    explicit KillSwitch(std::function<void()> on_trip = nullptr) noexcept
        : tripped_{false}
        , on_trip_{std::move(on_trip)}
    {
        SPDLOG_DEBUG("KillSwitch constructed, has_callback={}",
                     static_cast<bool>(on_trip_));
    }

    // No copy, no move — the atomic and the bound std::function don't
    // gain anything from moving, and disabling avoids subtle aliasing.
    KillSwitch(const KillSwitch&)            = delete;
    KillSwitch& operator=(const KillSwitch&) = delete;
    KillSwitch(KillSwitch&&)                 = delete;
    KillSwitch& operator=(KillSwitch&&)      = delete;

    ~KillSwitch() = default;

    /// Poll: returns true iff `trip()` has been called at least once.
    /// Uses acquire ordering so any writes performed before `trip()`
    /// synchronize-with the observer that sees `true`.
    [[nodiscard]] bool tripped() const noexcept {
        return tripped_.load(std::memory_order_acquire);
    }

    /// Trip the switch.  Idempotent and thread-safe.
    ///
    /// - First call: transitions to tripped and, if set, invokes `on_trip`.
    /// - Subsequent calls: no-op (callback is NOT re-invoked).
    /// - Safe under concurrent calls: exactly one thread wins the CAS and
    ///   runs the callback; the others return without side effects.
    void trip() noexcept {
        // Fast path: already tripped → bail early without touching
        // the exchange (cheaper on contention).
        if (tripped_.load(std::memory_order_acquire)) {
            return;
        }

        // Compare-and-swap: only the thread that observes `false` and
        // successfully flips to `true` is entitled to fire the callback.
        bool expected = false;
        if (!tripped_.compare_exchange_strong(
                expected, true,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            // Another thread won the race — nothing else to do.
            return;
        }

        SPDLOG_WARN("KillSwitch tripped");

        if (on_trip_) {
            // Callback is invoked exactly once, on the winning thread.
            // Caller contract: must be noexcept; an escaping exception
            // will call std::terminate via this function's noexcept.
            on_trip_();
        }
    }

private:
    std::atomic<bool>     tripped_;
    std::function<void()> on_trip_;
};

// Compile-time guarantees enforced by the D-3 design decision: the
// kill switch is one-way.  Uses a detection idiom via an unevaluated
// generic lambda so that the (intentional) absence of these members is
// NOT a hard error — GCC's `requires` inside static_assert would
// otherwise treat a missing member access as a normal name-lookup
// failure rather than SFINAE.
namespace detail {
template <class T> concept has_reset  = requires(T& t) { t.reset(); };
template <class T> concept has_untrip = requires(T& t) { t.untrip(); };
template <class T> concept has_clear  = requires(T& t) { t.clear(); };
} // namespace detail

static_assert(!detail::has_reset<KillSwitch>,
              "KillSwitch must NOT expose reset() — D-3");
static_assert(!detail::has_untrip<KillSwitch>,
              "KillSwitch must NOT expose untrip() — D-3");
static_assert(!detail::has_clear<KillSwitch>,
              "KillSwitch must NOT expose clear() — D-3");

} // namespace eph::utils
