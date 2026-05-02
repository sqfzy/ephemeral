#pragma once

/// @file scope_guard.hpp
/// RAII scope guard: run a cleanup callable on scope exit unless dismissed.
///
/// Use this in any function with **two or more early-return paths that
/// must release the same resource**. Across four consecutive `/pax --loop
/// review eph-net-dpdk` rounds (r1 keepalive_misses_, r2 effective_mss_,
/// r3 mp_registry spec-disagree, r4 mp_registry pre-CAS validation), the
/// dominant bug class was "function has N early-return paths, only some
/// of them release the resource". This guard mechanically eliminates the
/// class: the destructor runs the cleanup unless `disarm()` was called.
///
/// Typical usage:
///
/// @code
///   auto* slot = registry.try_claim();
///   if (slot == nullptr) return std::unexpected{...};
///
///   auto guard = eph::utils::ScopeGuard{[&] {
///       registry.release(slot);
///   }};
///
///   if (!validate_a()) return std::unexpected{...};   // guard fires
///   if (!validate_b()) return std::unexpected{...};   // guard fires
///   if (!validate_c()) return std::unexpected{...};   // guard fires
///
///   guard.disarm();                                   // success path
///   return slot;
/// @endcode
///
/// Design notes:
/// - **Non-copyable, non-movable.** A movable guard would need to nullify
///   the source's armed flag on move, doubling the surface area for
///   subtle accident; KillSwitch follows the same rule for the same
///   reason. CTAD lets you write `ScopeGuard{[]{...}}` directly.
/// - **`[[nodiscard]]`** because a temporary `ScopeGuard{...};` (no
///   binding) destructs immediately at the end of the statement, which
///   is almost certainly a bug — caller meant to keep the guard alive
///   for the whole scope.
/// - **Destructor is `noexcept`.** The cleanup callable must not throw;
///   an escaping exception terminates (consistent with the codebase-wide
///   `SPDLOG_NO_EXCEPTIONS`).
/// - **No `std::function`** — F is held by-value, so empty-state checks
///   and heap-alloc are avoided. Stateless lambdas compile to zero
///   storage; captures are stored by-value in F.
///
/// This is **not** the C++26 `std::scope_exit` — that has different
/// move semantics and lives in `<scope>` which is not yet ubiquitous.
/// The minimal API here matches the project's RAII style (KillSwitch,
/// TokenBucket, etc.).

#include <concepts>
#include <type_traits>
#include <utility>

namespace eph::utils {

/// RAII guard that runs `fn` on destruction unless `disarm()` was called.
///
/// @tparam F  cleanup callable; must be `noexcept`-invocable and not
///            throw at runtime (escaping exception → `std::terminate`).
template <std::invocable F>
class [[nodiscard]] ScopeGuard {
public:
    /// Construct an armed guard owning the cleanup callable.
    ///
    /// @param fn  the cleanup callable (typically a lambda capturing the
    ///            resource handle by value or by reference; reference
    ///            captures are valid as long as the captured object
    ///            outlives the guard, which it always does for the
    ///            "release-on-early-return" pattern).
    explicit ScopeGuard(F fn) noexcept(
        std::is_nothrow_move_constructible_v<F>)
        : fn_{std::move(fn)}
    {}

    // Non-copyable, non-movable.
    ScopeGuard(const ScopeGuard&)            = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
    ScopeGuard(ScopeGuard&&)                 = delete;
    ScopeGuard& operator=(ScopeGuard&&)      = delete;

    /// Disarm the guard so the cleanup will NOT run on destruction.
    ///
    /// Call this on the success path right before the function returns
    /// the resource to the caller (i.e. the caller has now taken
    /// ownership and the cleanup is the caller's responsibility).
    /// Idempotent — calling twice is harmless.
    void disarm() noexcept { armed_ = false; }

    /// Query whether the guard is still armed.
    /// Mainly useful for tests; production code rarely needs this.
    [[nodiscard]] bool armed() const noexcept { return armed_; }

    ~ScopeGuard() noexcept {
        if (armed_) {
            // Caller contract: fn_ must be noexcept-callable. The
            // class-level noexcept on this destructor means an escaping
            // exception calls std::terminate — consistent with the
            // project's SPDLOG_NO_EXCEPTIONS posture.
            fn_();
        }
    }

private:
    F    fn_;
    bool armed_ = true;
};

}  // namespace eph::utils
