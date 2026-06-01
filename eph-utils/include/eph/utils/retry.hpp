#pragma once

/// @file retry.hpp
/// Blocking retry driver — the eph analogue of Rust `backon`'s
/// `Retryable::retry`. Drives a fallible callable, waiting between attempts
/// according to a `Backoff` strategy (see `backoff.hpp`), until it succeeds,
/// the error is judged non-retriable, or the backoff is exhausted.
///
/// @code
///   using namespace std::chrono_literals;
///
///   // Exponential backoff, up to 5 retries, retry on any error:
///   auto r = eph::utils::retry(
///       [&]{ return connect_once(); },                     // -> std::expected<Conn, ErrorInfo>
///       eph::utils::ExponentialBackoff{{.initial_backoff = 100ms,
///                                       .max_backoff     = 2s,
///                                       .max_attempts    = 5}});
///
///   // Custom retriable predicate (don't retry programmer-error configs):
///   auto r2 = eph::utils::retry(
///       fn, eph::utils::ConstantBackoff{{.delay = 200ms, .max_attempts = 3}},
///       [](const eph::core::ErrorInfo& e) {
///           return e.code != eph::core::Error::InvalidConfig;
///       });
/// @endcode
///
/// Design points:
///   - **Blocking.** The driver sleeps the calling thread between attempts.
///     For non-blocking / tick-driven reconnection on a poll loop (DPDK
///     lcore burst, etc.) do NOT use this — drive a `Backoff` yourself or use
///     `eph::net::ReconnectOrchestrator`, which never sleeps.
///   - **`std::expected`-shaped callables.** `fn()` must return a type that is
///     contextually convertible to `bool` (truthy = success) and exposes
///     `.error()` on failure — i.e. `std::expected<T, E>`. No exceptions are
///     caught; the eph stack does not throw across boundaries.
///   - **Zero `std::function`.** The `when` predicate and the sleeper are
///     template parameters with defaults, so the common call compiles to a
///     tight loop with no type erasure and no heap allocation.
///   - **`when` defaults to "retry every error".** Override to stop early on
///     non-transient failures.
///   - **Injectable sleeper.** Defaults to `ThreadSleeper`
///     (`std::this_thread::sleep_for`). The injection point exists primarily
///     so unit tests can run without actually sleeping — it is not a
///     general-purpose extension surface.
///   - **Observable.** Each retry logs at DEBUG (attempt number, delay, and —
///     when the error type is formattable — the error). Callers needing
///     richer hooks should use `ReconnectOrchestrator`'s event interface.

#include <chrono>
#include <format>
#include <thread>
#include <type_traits>
#include <utility>

#include <spdlog/spdlog.h>

#include "eph/utils/backoff.hpp"

namespace eph::utils {

// ---------------------------------------------------------------------------
// Default policy objects
// ---------------------------------------------------------------------------

/// @brief Default sleeper: blocks the calling thread via
///        `std::this_thread::sleep_for`. A zero/negative delay is a no-op
///        (immediate retry), matching `ConstantBackoff{.delay = 0ms}`.
struct ThreadSleeper {
    void operator()(std::chrono::milliseconds d) const noexcept {
        if (d.count() > 0) std::this_thread::sleep_for(d);
    }
};

/// @brief Default retriable-predicate: retry on every error.
struct RetryAlways {
    template <class E>
    [[nodiscard]] bool operator()(const E&) const noexcept {
        return true;
    }
};

// ---------------------------------------------------------------------------
// ExpectedResult concept
// ---------------------------------------------------------------------------

/// @brief A `std::expected`-shaped result: contextually convertible to bool
///        (truthy on success) and exposing `.error()`.
template <class R>
concept ExpectedResult = requires(const R& r) {
    { static_cast<bool>(r) };
    r.error();
};

// ---------------------------------------------------------------------------
// retry
// ---------------------------------------------------------------------------

/// @brief Repeatedly invoke `fn` until it succeeds, the error is non-retriable
///        per `when`, or `backoff` is exhausted.
///
/// @tparam B        A `Backoff` strategy (consumed by value; one retry session).
/// @tparam Fn       Callable returning an `ExpectedResult`.
/// @tparam When     Predicate `bool(const E&)` — return false to stop retrying
///                  and surface the error immediately. Defaults to retry-all.
/// @tparam Sleeper  `void(std::chrono::milliseconds)` — blocks between
///                  attempts. Defaults to `ThreadSleeper`.
///
/// @param fn       The fallible operation. Invoked at least once.
/// @param backoff  Delay strategy. Its attempt budget bounds the number of
///                 *retries*; total `fn` invocations are at most
///                 `attempts_budget + 1` (the initial try plus one per delay).
/// @param when     Retriable predicate (see above).
/// @param sleep    Sleeper (see above).
/// @return The first successful result, or the last failure if `when` rejects
///         an error or the backoff is exhausted.
template <Backoff B, class Fn, class When = RetryAlways, class Sleeper = ThreadSleeper>
[[nodiscard]] auto retry(Fn&& fn, B backoff, When when = {}, Sleeper sleep = {})
    -> std::remove_cvref_t<std::invoke_result_t<Fn&>> {
    using Result = std::remove_cvref_t<std::invoke_result_t<Fn&>>;
    static_assert(ExpectedResult<Result>,
                  "retry: fn must return a std::expected-shaped result "
                  "(contextually bool-convertible with .error())");

    Result result = fn();
    uint32_t attempt = 1;

    while (!static_cast<bool>(result)) {
        // Non-retriable? Surface immediately.
        if (!when(std::as_const(result).error())) {
            SPDLOG_DEBUG("retry: attempt {} failed with non-retriable error; "
                         "giving up",
                         attempt);
            break;
        }

        // Backoff exhausted? Surface the last error.
        const auto delay = backoff.next_delay();
        if (!delay) {
            SPDLOG_DEBUG("retry: backoff exhausted after {} attempt(s); "
                         "giving up",
                         attempt);
            break;
        }

        // Log the impending retry. Include the error only when its type is
        // formattable (the common eph::core::ErrorInfo case is, via its
        // std::formatter specialization); otherwise log attempt + delay only,
        // keeping retry fully generic over the error type.
        if constexpr (std::formattable<
                          std::remove_cvref_t<decltype(std::as_const(result).error())>,
                          char>) {
            SPDLOG_DEBUG("retry: attempt {} failed ({}); backing off {} ms",
                         attempt, std::format("{}", std::as_const(result).error()),
                         delay->count());
        } else {
            SPDLOG_DEBUG("retry: attempt {} failed; backing off {} ms", attempt,
                         delay->count());
        }

        sleep(*delay);
        result = fn();
        ++attempt;
    }

    return result;
}

}  // namespace eph::utils
