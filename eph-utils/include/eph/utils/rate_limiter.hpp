#pragma once

/// @file rate_limiter.hpp
/// Thread-safe token-bucket rate limiter for per-venue / global throughput
/// governance.
///
/// Typical usage:
///
/// @code
///   using eph::utils::TokenBucket;
///   TokenBucket binance{{.capacity = 1200, .refill_per_second = 20.0}};
///   // weight=1 per normal request, weight=10 for expensive endpoints
///   if (!binance.try_acquire(1)) {
///       SPDLOG_WARN("binance rate limit exhausted");
///       return;
///   }
/// @endcode
///
/// Design:
///
/// - **Thread-safe by default.**  Shared rate limiters across threads are
///   the common HFT case (one per venue, many sender threads).  Mutex-
///   protected; uncontended overhead is ~20 ns — acceptable given that the
///   alternative (lock-free float accumulation) is a lot of code for a
///   tiny win.  See D-4 in plan.md for the full decision record.
/// - **Fixed configuration.**  `capacity` and `refill_per_second` are
///   captured at construction; there is deliberately no dynamic reconfig
///   API.  Venues with step-changing limits should recreate the bucket.
/// - **`steady_clock`, not TSC.**  Rate limits operate on sub-second
///   budgets — the relative coarseness of `steady_clock` is fine and we
///   avoid TSC calibration / hot-plug issues.
/// - **Weighted acquire.**  Matches Binance / OKX weight-based budgets.
/// - **`noexcept`.**  `std::mutex::lock` can theoretically throw on
///   resource exhaustion, but in practice we accept std::terminate on a
///   broken mutex (same policy as the rest of eph-utils).

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>

#include <spdlog/spdlog.h>

namespace eph::utils {

/// Token bucket rate limiter.
///
/// Invariants:
///   - `tokens_` is always in `[0, capacity_]` after every public call.
///   - `last_refill_` monotonically advances (never moves backward).
///
/// See D-4 for rationale behind thread-safe-by-default.
class TokenBucket {
public:
    struct Config {
        /// Maximum burst size, in tokens.  Also the initial fill.
        std::uint32_t capacity;
        /// Sustained refill rate, tokens per second.  May be fractional
        /// for slow limiters (e.g. 0.1 = one token every 10 s).  Must be
        /// strictly positive; non-positive values are clamped to 0 which
        /// makes the bucket a pure burst-then-empty budget.
        double refill_per_second;
    };

    /// Construct a full bucket.  Post-condition: `available_tokens() == capacity`.
    explicit TokenBucket(Config cfg) noexcept
        : capacity_{cfg.capacity}
        , refill_per_sec_{std::max(0.0, cfg.refill_per_second)}
        , tokens_{static_cast<double>(cfg.capacity)}
        , last_refill_{std::chrono::steady_clock::now()}
    {
        SPDLOG_DEBUG("TokenBucket ctor capacity={} refill_per_second={}",
                     capacity_, refill_per_sec_);
    }

    // Not copyable, not movable: contains a mutex.
    TokenBucket(const TokenBucket&)            = delete;
    TokenBucket& operator=(const TokenBucket&) = delete;
    TokenBucket(TokenBucket&&)                 = delete;
    TokenBucket& operator=(TokenBucket&&)      = delete;

    /// Non-blocking acquire.  Returns `true` iff `weight` tokens were
    /// deducted from the bucket, `false` otherwise.  Never sleeps.
    ///
    /// Edge cases:
    ///   - `weight == 0`: always succeeds as a free no-op (consistent with
    ///     "zero cost costs zero tokens").
    ///   - `weight > capacity`: always fails — the bucket can never hold
    ///     enough tokens to satisfy the request, so we reject immediately
    ///     rather than spin-wait forever.
    [[nodiscard]] bool try_acquire(std::uint32_t weight = 1) noexcept {
        if (weight == 0) {
            // Free no-op: no refill, no deduction.  Keeps the fast path
            // tight for probe-style callers.
            return true;
        }
        if (weight > capacity_) {
            // Impossible to ever satisfy — fail fast instead of spinning.
            SPDLOG_DEBUG("TokenBucket::try_acquire weight={} > capacity={} — reject",
                         weight, capacity_);
            return false;
        }

        std::lock_guard<std::mutex> lk(mu_);
        refill_locked_();

        if (tokens_ >= static_cast<double>(weight)) {
            tokens_ -= static_cast<double>(weight);
            return true;
        }
        SPDLOG_DEBUG("TokenBucket::try_acquire weight={} tokens={} — reject",
                     weight, tokens_);
        return false;
    }

    /// Best-effort snapshot of currently available tokens.  May be stale
    /// the moment this function returns — for diagnostics / metrics only.
    [[nodiscard]] double available_tokens() const noexcept {
        std::lock_guard<std::mutex> lk(mu_);
        // const_cast-free: refill_locked_ is logically const in the
        // observable-state sense but mutates `tokens_` / `last_refill_`,
        // so we duplicate the refill math inline here to keep this
        // method clean and const-correct.
        const auto  now     = std::chrono::steady_clock::now();
        const auto  elapsed = std::chrono::duration<double>(now - last_refill_).count();
        const double refilled = tokens_ + elapsed * refill_per_sec_;
        return std::min(refilled, static_cast<double>(capacity_));
    }

    [[nodiscard]] std::uint32_t capacity()    const noexcept { return capacity_; }
    [[nodiscard]] double        refill_rate() const noexcept { return refill_per_sec_; }

private:
    /// Advance `tokens_` by the amount refilled since `last_refill_`.
    /// Caller must hold `mu_`.  Clamps to `capacity_` so long-idle
    /// buckets don't overflow.
    void refill_locked_() noexcept {
        const auto now     = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration<double>(now - last_refill_).count();
        if (elapsed <= 0.0) {
            // Clock ran backward (shouldn't for steady_clock) or zero
            // granularity — skip, do not rewind last_refill_.
            return;
        }
        tokens_      = std::min(static_cast<double>(capacity_),
                                tokens_ + elapsed * refill_per_sec_);
        last_refill_ = now;
    }

    const std::uint32_t capacity_;
    const double        refill_per_sec_;

    mutable std::mutex                              mu_;
    double                                          tokens_;       // guarded by mu_
    std::chrono::steady_clock::time_point           last_refill_;  // guarded by mu_
};

// Sanity: TokenBucket must not inherit virtual dispatch and must remain
// a concrete, non-polymorphic type ("no virtual in hot path" rule).
static_assert(!std::is_polymorphic_v<TokenBucket>,
              "TokenBucket must not be polymorphic");

} // namespace eph::utils
