#pragma once

/// @file reconnect_orchestrator.hpp
///
/// `ReconnectOrchestrator<S>` — backend-agnostic connection lifecycle
/// orchestrator. Composes `eph::net::ReconnectPolicy` with user-provided
/// callbacks (factory / on_disconnect / on_reconnect / attach / detach),
/// and exposes 4 metrics.
///
/// The motivation is multi-venue HFT (Binance, OKX, Bybit, ...) where
/// every adapter ends up rewriting the same loop:
///
///     while (running) {
///         disconnect_detected → backoff → DNS resolve → factory → attach
///                              → resubscribe → run until disconnect
///     }
///
/// `ReconnectOrchestrator` lifts the *generic* parts (state machine, backoff
/// timing, metrics) into the library; the *venue-specific* parts (DNS,
/// subscribe payload, cancel-on-disconnect, TLS resumption) remain in
/// callbacks the caller supplies. Result: per-venue glue shrinks from a
/// hand-rolled loop to a `ReconnectOrchestrator` constructor + 1-2 lambdas.
///
/// State machine:
///
///                ┌─────────┐
///       new ──→  │  Idle   │ (constructed, start() not called)
///                └────┬────┘
///                     │  start(now_tsc)
///                     ↓
///                ┌──────────┐
///        ┌─────→ │Connecting│
///        │       └────┬─────┘
///        │            │ factory_fn() OK
///        │            ↓
///        │       ┌─────────┐  ←── user calls mark_disconnected(reason)
///        │       │Connected│      or tick() detects state == Closed/Reset
///        │       └────┬────┘
///        │            │ on_disconnect_(reason)
///        │            │ detach_fn_(stream.get())
///        │            │ stream_.reset()
///        │            ↓
///        │       ┌─────────┐
///        │       │ Backoff │ ─── tick(now): wait until next_attempt_tsc
///        │       └────┬────┘
///        │            │ now >= next_attempt_tsc
///        │            │ policy.should_reconnect()?
///        │       yes ─┘                no
///        └─────────────────────────┐   ↓
///                                  ↓ ┌────────┐
///                              Connecting│ Failed │ (terminal)
///                                       └────────┘
///
/// Ownership model — owning + tick-based:
///   - The orchestrator owns the current `std::unique_ptr<S>` Stream.
///   - `current()` lets the caller add the inner Stream to its Poller.
///   - On disconnect, `detach_fn_` is invoked so the caller can remove
///     the dying Stream from the Poller before it is destroyed; the
///     subsequent `factory_fn_` produces a fresh Stream and `attach_fn_`
///     re-registers it. This keeps the orchestrator backend-agnostic
///     (no `Poller&` member).
///   - The orchestrator never sleeps. The caller's main loop drives
///     `tick(now_tsc)` once per poll cycle, which advances the state
///     machine when the next backoff deadline elapses. Compatible with
///     DPDK lcore burst loops where blocking is forbidden.
///
/// Thread safety: not internally synchronized. One driver thread per
/// orchestrator instance, mirroring `Stream` and `Poller`. The metric
/// atomics are present so a separate sink-reader thread can observe
/// counters without racing the writer.
///
/// Example (Binance over DPDK):
///
///     namespace en = eph::net;
///     en::ReconnectConfig cfg{
///         .policy = {.initial_backoff = 250ms, .max_backoff = 10s}};
///     auto factory = [&]() { return DpdkTcpStream::create_and_attach(scfg, platform); };
///     auto on_recon = [&](uint32_t a, uint64_t d_ns) noexcept {
///         orch.current()->send(subscribe_msg);
///     };
///     en::ReconnectOrchestrator<DpdkTcpStream<WsCodec, true>> orch{
///         cfg, factory, /*on_disc*/{}, on_recon};
///     orch.start(eph::utils::TSC::now());
///     while (running) {
///         poller.poll();
///         orch.tick(eph::utils::TSC::now());
///     }

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include <spdlog/spdlog.h>

#include "eph/core/error.hpp"
#include "eph/core/metrics_concept.hpp"

#include "eph/net/concepts.hpp"
#include "eph/net/detail/reconnect_logger.hpp"
#include "eph/net/reconnect_policy.hpp"

namespace eph::net {

// ---------------------------------------------------------------------------
// ReconnectState
// ---------------------------------------------------------------------------

/// @brief Lifecycle state of a `ReconnectOrchestrator`.
///
/// Linear progression with two terminal forks:
///   `Idle` → `Connecting` → `Connected` ↔ `Backoff` ↔ `Connecting` → `Failed`
///
/// `Failed` is reached only when `ReconnectPolicy::should_reconnect()`
/// returns `false` after a factory failure. A user-initiated `stop()` also
/// halts reconnect attempts but preserves the current Stream (state stays
/// at `Connected` or whatever it was; further `tick()` calls are no-ops).
enum class ReconnectState : uint8_t {
    Idle = 0,    ///< Constructed, `start()` not yet called.
    Connecting,  ///< Factory invocation in progress (or about to retry).
    Connected,   ///< Stream is alive and `current()` is non-null.
    Backoff,     ///< Waiting for next_attempt_tsc to elapse.
    Failed,      ///< Policy exhausted; further `tick()` is a no-op.
};

/// @brief Stable string for a `ReconnectState`. Useful for logging.
[[nodiscard]] constexpr const char* to_string(ReconnectState s) noexcept {
    switch (s) {
        case ReconnectState::Idle:       return "Idle";
        case ReconnectState::Connecting: return "Connecting";
        case ReconnectState::Connected:  return "Connected";
        case ReconnectState::Backoff:    return "Backoff";
        case ReconnectState::Failed:     return "Failed";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// ReconnectMetric
// ---------------------------------------------------------------------------

/// @brief Counters exposed by `ReconnectOrchestrator`. Indexed into a
///        per-instance atomic array; readable via `metric()` or pushed to
///        a `MetricsSink` via `publish_reconnect_metrics()`.
///
/// `kReconnectDurationNs` accumulates the per-cycle (disconnect → reconnect)
/// duration sum across all successful reconnects. `kLastReconnectDurationNs`
/// holds only the most recent cycle (gauge-style). Together with `kCount`,
/// dashboards can derive `avg = total / count` and `max ≈ last` (last is
/// not a true maximum, but in HFT reconnect noise it's a reasonable proxy).
enum class ReconnectMetric : std::size_t {
    kReconnectCount          = 0,  ///< Successful reconnects (counter).
    kReconnectFailures       = 1,  ///< Factory invocation failures (counter).
    kReconnectDurationNs     = 2,  ///< Sum of per-cycle ns durations (counter).
    kLastReconnectDurationNs = 3,  ///< Most recent cycle ns (gauge-style).
    kCount                   = 4,  ///< Sentinel — always last.
};

/// @brief OTel-style hierarchical names. Index MUST match `ReconnectMetric`.
inline constexpr std::array<std::string_view,
                            static_cast<std::size_t>(ReconnectMetric::kCount)>
kReconnectMetricNames = {
    "net.reconnect.count",
    "net.reconnect.failures",
    "net.reconnect.duration_ns",
    "net.reconnect.duration_ns.last",
};

static_assert(kReconnectMetricNames.size() ==
              static_cast<std::size_t>(ReconnectMetric::kCount),
              "kReconnectMetricNames out of sync with ReconnectMetric enum");

// ---------------------------------------------------------------------------
// ReconnectConfig
// ---------------------------------------------------------------------------

/// @brief Construction-time settings for `ReconnectOrchestrator`.
struct ReconnectConfig {
    /// @brief Backoff policy parameters. Forwarded into the orchestrator's
    ///        owned `ReconnectPolicy`.
    ReconnectPolicyConfig policy{};

    /// @brief When `true` (default), `tick()` reads `current()->state()` and
    ///        treats `Closed` (and any non-`Established` non-transitional
    ///        state) as a peer-initiated disconnect, transitioning to
    ///        Backoff. When `false`, only explicit `mark_disconnected()`
    ///        triggers the transition. UDP-style streams that have no
    ///        meaningful "closed" state should set this to `false`.
    bool auto_detect_via_state = true;
};

// ---------------------------------------------------------------------------
// ReconnectOrchestrator<S>
// ---------------------------------------------------------------------------

/// @brief Backend-agnostic connection lifecycle orchestrator.
///
/// See file header for the complete state machine and motivation. Templated
/// on a Stream concept conformer (`KernelTcpStream`, `DpdkTcpStream`,
/// `FakeStream`).
///
/// All callbacks are `std::function`-typed: reconnect is *not* a hot path
/// (a 24h cycle on Binance, less than 1s typical on OKX), so the one-time
/// constructor heap allocation for capture state is negligible compared to
/// API simplicity.
///
/// @tparam S  Stream concept conformer (must satisfy `eph::net::Stream<S>`).
template <Stream S>
class ReconnectOrchestrator {
public:
    using StreamPtr    = std::unique_ptr<S>;
    using Factory      = std::function<std::expected<StreamPtr, eph::core::ErrorInfo>()>;
    using OnDisconnect = std::function<void(const eph::core::ErrorInfo&) noexcept>;
    using OnReconnect  = std::function<void(uint32_t attempt, uint64_t duration_ns) noexcept>;
    using AttachFn     = std::function<std::expected<void, eph::core::ErrorInfo>(S*) noexcept>;
    using DetachFn     = std::function<void(S*) noexcept>;

    // ── Construction ────────────────────────────────────────────────────────

    /// @brief Construct an idle orchestrator. No I/O, no factory invocation.
    ///
    /// @param cfg            Backoff policy + auto-detect flag.
    /// @param factory        Required. Produces a fresh Stream on each attempt.
    /// @param on_disconnect  Optional. Invoked on every detected disconnect
    ///                       (auto or user-driven). Default: no-op.
    /// @param on_reconnect   Optional. Invoked once per successful reconnect
    ///                       with `(attempt_number, duration_ns)`. The user
    ///                       typically (re)subscribes inside this callback
    ///                       via `current()->send(...)`. Default: no-op.
    /// @param attach         Optional. Invoked AFTER each successful factory
    ///                       call so the user can register the Stream on
    ///                       their Poller. Default: no-op (caller's factory
    ///                       must register itself, e.g. DPDK
    ///                       `create_and_attach`).
    /// @param detach         Optional. Invoked BEFORE the Stream is reset
    ///                       so the user can deregister it. Default: no-op.
    ReconnectOrchestrator(ReconnectConfig cfg,
                          Factory factory,
                          OnDisconnect on_disconnect = {},
                          OnReconnect on_reconnect = {},
                          AttachFn attach = {},
                          DetachFn detach = {}) noexcept
        : cfg_(cfg),
          policy_(cfg.policy),
          factory_(std::move(factory)),
          on_disconnect_(std::move(on_disconnect)),
          on_reconnect_(std::move(on_reconnect)),
          attach_(std::move(attach)),
          detach_(std::move(detach)),
          state_(ReconnectState::Idle) {
        SPDLOG_LOGGER_TRACE(detail::reconnect_logger(),
            "ReconnectOrchestrator ctor: initial_backoff={}ms max_backoff={}ms "
            "max_attempts={} auto_detect={}",
            cfg_.policy.initial_backoff.count(),
            cfg_.policy.max_backoff.count(),
            cfg_.policy.max_attempts,
            cfg_.auto_detect_via_state);
    }

    ReconnectOrchestrator(const ReconnectOrchestrator&)            = delete;
    ReconnectOrchestrator& operator=(const ReconnectOrchestrator&) = delete;
    ReconnectOrchestrator(ReconnectOrchestrator&&)                 = delete;
    ReconnectOrchestrator& operator=(ReconnectOrchestrator&&)      = delete;

    // ── Lifecycle ───────────────────────────────────────────────────────────

    /// @brief Trigger the first connection attempt.
    ///
    /// Idle → Connecting; on factory success → Connected (and `attach_` runs);
    /// on factory failure → either Backoff (policy still has budget) or
    /// Failed (policy exhausted). Calling outside the Idle state returns
    /// `Error::InvalidConfig` and changes nothing.
    ///
    /// @param now_tsc  Current TSC value (`eph::utils::TSC::now()`).
    /// @return Empty on success or backoff scheduled; `ErrorInfo` only on
    ///         programmer-error (called twice / wrong state).
    [[nodiscard]] std::expected<void, eph::core::ErrorInfo>
    start(uint64_t now_tsc) noexcept;

    /// @brief Advance the state machine. Call once per poll cycle.
    ///
    /// In `Connected`: reads `stream->state()` and may transition to
    /// Backoff if `auto_detect_via_state` and the state is not Established.
    /// In `Backoff`: if `now_tsc >= next_attempt_tsc_`, attempt factory
    /// again. In `Idle` / `Failed`: no-op.
    ///
    /// @param now_tsc  Current TSC value.
    void tick(uint64_t now_tsc) noexcept;

    /// @brief Force a disconnect transition (e.g. heartbeat timeout, explicit
    ///        Close-1011 receipt). No-op outside Connected.
    ///
    /// @param reason  Carried verbatim into `on_disconnect_` and the log.
    void mark_disconnected(eph::core::ErrorInfo reason) noexcept;

    /// @brief Halt further reconnect attempts. The current Stream (if any)
    ///        is preserved; `tick()` becomes a no-op.
    ///
    /// Intended for graceful shutdown. Distinct from `Failed`: `stop()`
    /// is user-initiated and explicit; `Failed` is policy exhaustion.
    void stop() noexcept;

    // ── Accessors ───────────────────────────────────────────────────────────

    /// @brief Current Stream pointer. Non-null only in `Connected` state.
    [[nodiscard]] S* current() noexcept { return stream_.get(); }
    [[nodiscard]] const S* current() const noexcept { return stream_.get(); }

    /// @brief Current state machine state.
    [[nodiscard]] ReconnectState state() const noexcept { return state_; }

    /// @brief Total attempts since construction (or last `start()`).
    /// Mirrors `policy_.attempts()`.
    [[nodiscard]] uint32_t attempts() const noexcept { return policy_.attempts(); }

    /// @brief Read one of the four orchestrator metrics.
    /// Out-of-range `m` returns 0 (mirrors `Stream::metric` contract).
    [[nodiscard]] uint64_t metric(ReconnectMetric m) const noexcept {
        const auto i = static_cast<std::size_t>(m);
        if (i >= static_cast<std::size_t>(ReconnectMetric::kCount)) return 0;
        return metrics_[i].load(std::memory_order_relaxed);
    }

    /// @brief Convenience accessor for `kReconnectCount`.
    [[nodiscard]] uint64_t reconnect_count() const noexcept {
        return metric(ReconnectMetric::kReconnectCount);
    }
    /// @brief Convenience accessor for `kReconnectFailures`.
    [[nodiscard]] uint64_t reconnect_failures() const noexcept {
        return metric(ReconnectMetric::kReconnectFailures);
    }
    /// @brief Convenience accessor for `kReconnectDurationNs` (sum).
    [[nodiscard]] uint64_t total_reconnect_duration_ns() const noexcept {
        return metric(ReconnectMetric::kReconnectDurationNs);
    }
    /// @brief Convenience accessor for `kLastReconnectDurationNs` (gauge).
    [[nodiscard]] uint64_t last_reconnect_duration_ns() const noexcept {
        return metric(ReconnectMetric::kLastReconnectDurationNs);
    }

private:
    ReconnectConfig    cfg_;
    ReconnectPolicy    policy_;
    Factory            factory_;
    OnDisconnect       on_disconnect_;
    OnReconnect        on_reconnect_;
    AttachFn           attach_;
    DetachFn           detach_;

    StreamPtr          stream_;
    ReconnectState     state_;
    uint64_t           next_attempt_tsc_{0};  ///< Earliest TSC at which to attempt next factory call.
    uint64_t           disconnect_tsc_{0};    ///< TSC of the latest disconnect (for duration_ns).
    bool               stopped_{false};       ///< Set by `stop()`; gates `tick()`.

    // Cache-line-isolated atomics so a sink reader thread can scan without
    // false-sharing the writer's cache line. Same alignment trick as the
    // Stream metric arrays elsewhere in the codebase.
    alignas(64) std::array<std::atomic<uint64_t>,
                           static_cast<std::size_t>(ReconnectMetric::kCount)>
        metrics_{};

    // Stage 2 fills these in.
    void try_attempt_(uint64_t now_tsc) noexcept;
    void enter_backoff_(uint64_t now_tsc) noexcept;
};

// ---------------------------------------------------------------------------
// publish_reconnect_metrics
// ---------------------------------------------------------------------------

/// @brief Push every metric on `orch` to `sink`. Counters via `push_counter`,
///        the "last" duration via `push_gauge`. `tags` is forwarded verbatim.
///
/// Mirrors the existing `eph::net::publish_metrics` pattern for streams.
/// Caller-driven cadence — typically once per second or on a control plane
/// tick. Reading the atomics is `memory_order_relaxed` (we tolerate slightly
/// stale reads in exchange for zero contention with the writer).
template <class Orch, eph::core::MetricsSink Sink>
void publish_reconnect_metrics(const Orch& orch, Sink& sink,
                               std::span<const eph::core::MetricTag> tags = {}) noexcept {
    using M = ReconnectMetric;
    sink.push_counter(kReconnectMetricNames[static_cast<std::size_t>(M::kReconnectCount)],
                      static_cast<int64_t>(orch.metric(M::kReconnectCount)), tags);
    sink.push_counter(kReconnectMetricNames[static_cast<std::size_t>(M::kReconnectFailures)],
                      static_cast<int64_t>(orch.metric(M::kReconnectFailures)), tags);
    sink.push_counter(kReconnectMetricNames[static_cast<std::size_t>(M::kReconnectDurationNs)],
                      static_cast<int64_t>(orch.metric(M::kReconnectDurationNs)), tags);
    sink.push_gauge(kReconnectMetricNames[static_cast<std::size_t>(M::kLastReconnectDurationNs)],
                    static_cast<double>(orch.metric(M::kLastReconnectDurationNs)), tags);
}

// ---------------------------------------------------------------------------
// Stage-1 placeholder definitions — Stage 2 implements the real bodies.
// Kept inline in the header so the class is fully usable today (Idle stays
// Idle on tick(); start() returns success; mark_disconnected logs only).
// ---------------------------------------------------------------------------

template <Stream S>
inline std::expected<void, eph::core::ErrorInfo>
ReconnectOrchestrator<S>::start(uint64_t /*now_tsc*/) noexcept {
    SPDLOG_LOGGER_DEBUG(detail::reconnect_logger(),
        "ReconnectOrchestrator::start: stage-1 placeholder (state={})",
        to_string(state_));
    return {};
}

template <Stream S>
inline void ReconnectOrchestrator<S>::tick(uint64_t /*now_tsc*/) noexcept {
    // Stage-1 placeholder. Stage 2 implements the real state machine.
}

template <Stream S>
inline void ReconnectOrchestrator<S>::mark_disconnected(eph::core::ErrorInfo reason) noexcept {
    SPDLOG_LOGGER_DEBUG(detail::reconnect_logger(),
        "ReconnectOrchestrator::mark_disconnected: stage-1 placeholder ({})",
        reason.detail);
}

template <Stream S>
inline void ReconnectOrchestrator<S>::stop() noexcept {
    stopped_ = true;
    SPDLOG_LOGGER_DEBUG(detail::reconnect_logger(),
        "ReconnectOrchestrator::stop: stage-1 placeholder");
}

template <Stream S>
inline void ReconnectOrchestrator<S>::try_attempt_(uint64_t /*now_tsc*/) noexcept {}

template <Stream S>
inline void ReconnectOrchestrator<S>::enter_backoff_(uint64_t /*now_tsc*/) noexcept {}

} // namespace eph::net
