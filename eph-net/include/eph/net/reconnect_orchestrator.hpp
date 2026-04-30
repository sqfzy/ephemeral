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
#include <chrono>
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
#include "eph/core/tcp_state.hpp"
#include "eph/utils/time.hpp"

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
///
/// `kSubscribeReplayCount` (T2.11) counts how many times the application
/// successfully completed a subscribe-replay after a reconnect. The
/// orchestrator can never observe replay completion itself — payloads /
/// ACKs are venue-specific — so the user calls `note_subscribe_replay()`
/// from inside their `OnReconnect` (or `OnReconnectEvent::Connected`)
/// callback once they have re-sent (and, for ACK-bearing venues,
/// confirmed) the subscription. The metric is therefore an *application-
/// asserted* counter, not a transport-observed one. Pair with
/// `kReconnectCount` to compute the replay success rate
/// (`replay / count`); a sustained drift means the app is reconnecting
/// but failing to re-subscribe.
enum class ReconnectMetric : std::size_t {
    kReconnectCount          = 0,  ///< Successful reconnects (counter).
    kReconnectFailures       = 1,  ///< Factory invocation failures (counter).
    kReconnectDurationNs     = 2,  ///< Sum of per-cycle ns durations (counter).
    kLastReconnectDurationNs = 3,  ///< Most recent cycle ns (gauge-style).
    kSubscribeReplayCount    = 4,  ///< User-asserted subscribe-replay completions (counter).
    kCount                   = 5,  ///< Sentinel — always last.
};

/// @brief OTel-style hierarchical names. Index MUST match `ReconnectMetric`.
///
/// New entries MUST be appended before `kCount` and the matching name
/// added to the same index here — the `static_assert` below catches any
/// drift at compile time.
inline constexpr std::array<std::string_view,
                            static_cast<std::size_t>(ReconnectMetric::kCount)>
kReconnectMetricNames = {
    "net.reconnect.count",
    "net.reconnect.failures",
    "net.reconnect.duration_ns",
    "net.reconnect.duration_ns.last",
    "net.reconnect.subscribe_replay_count",
};

static_assert(kReconnectMetricNames.size() ==
              static_cast<std::size_t>(ReconnectMetric::kCount),
              "kReconnectMetricNames out of sync with ReconnectMetric enum");

// ---------------------------------------------------------------------------
// ReconnectEvent (T2.14 — richer event hook)
// ---------------------------------------------------------------------------

/// @brief Fine-grained kinds of state transitions emitted by
///        `ReconnectOrchestrator`. One enum value per distinguishable point
///        in the reconnect lifecycle, so observers can differentiate between
///        e.g. a factory failure vs an attach failure without a parallel
///        side-channel.
///
/// The legacy `OnDisconnect` / `OnReconnect` callbacks remain in place; this
/// hook is purely additive.
///
/// Coverage map (orchestrator → kind):
///   - Detected disconnect (auto via state, or `mark_disconnected`) →
///     `DisconnectDetected`
///   - About to invoke `factory_fn`                               → `AttemptStarted`
///   - `factory_fn` returned an error                              → `AttemptFailed`
///   - `factory_fn` succeeded; about to invoke `attach_fn`         → (no event;
///     see Connected if the whole pipeline succeeds)
///   - `attach_fn` returned an error                               → `AttachFailed`
///   - Stream is live                                              → `Connected`
///   - Backoff scheduled (after any failure path)                  → `BackoffScheduled`
///   - Policy budget exhausted; terminal state                     → `Failed`
enum class ReconnectEventKind : uint8_t {
    DisconnectDetected = 0,  ///< tick() saw stream.state() != Established (or mark_disconnected).
    AttemptStarted,          ///< factory_fn() about to be called.
    AttemptFailed,           ///< factory_fn() returned error.
    AttachFailed,            ///< attach_fn(stream) returned error.
    Connected,               ///< factory + attach succeeded; stream live.
    BackoffScheduled,        ///< policy.next_backoff returned; will retry at next_attempt_tsc.
    Failed,                  ///< policy exhausted; terminal Failed state.
};

/// @brief Stable string for a `ReconnectEventKind`. Useful for logging.
[[nodiscard]] constexpr const char* to_string(ReconnectEventKind k) noexcept {
    switch (k) {
        case ReconnectEventKind::DisconnectDetected: return "DisconnectDetected";
        case ReconnectEventKind::AttemptStarted:     return "AttemptStarted";
        case ReconnectEventKind::AttemptFailed:      return "AttemptFailed";
        case ReconnectEventKind::AttachFailed:       return "AttachFailed";
        case ReconnectEventKind::Connected:          return "Connected";
        case ReconnectEventKind::BackoffScheduled:   return "BackoffScheduled";
        case ReconnectEventKind::Failed:             return "Failed";
    }
    return "UNKNOWN";
}

/// @brief Structured event payload delivered to `OnReconnectEvent`.
///
/// Fields are populated only when meaningful for the kind:
///   - `attempt`     — 1-based attempt number associated with the event.
///                     For `AttemptStarted` / `AttemptFailed` / `AttachFailed`
///                     / `Connected` this is the in-flight (or just-finished)
///                     attempt. For `BackoffScheduled` / `Failed` it is the
///                     attempt number that *would* be next. Matches the legacy
///                     `OnReconnect` second arg for `Connected`.
///   - `event_tsc`   — TSC at the moment the orchestrator emitted the event.
///   - `duration_ns` — only `Connected` populates this with the cycle delta
///                     from the most recent disconnect (or `start()`) to
///                     the just-completed factory+attach. Other kinds set 0.
///   - `error`       — only `AttemptFailed` and `AttachFailed` carry the
///                     underlying `ErrorInfo`. Others set `{Error::Ok, ""}`.
///
/// Deliberately omitted: `was_resumed`. The orchestrator cannot observe
/// whether the user's `factory_fn` reused a TLS session ticket; correlate
/// `Connected` events with the TLS layer's `tls.resume_count` metric (see
/// `eph::net::StreamMetric`) externally.
struct ReconnectEvent {
    ReconnectEventKind     kind;
    uint32_t               attempt;
    uint64_t               event_tsc;
    uint64_t               duration_ns;
    eph::core::ErrorInfo   error;
};

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
    // Note: std::function does not support `noexcept`-qualified function types
    // under libstdc++ 14 (the qualifier becomes part of the type in C++17 but
    // std::function's primary template doesn't model it). Concrete callbacks
    // are still allowed to be noexcept lambdas; the type alias just can't
    // carry the qualifier.
    using Factory          = std::function<std::expected<StreamPtr, eph::core::ErrorInfo>()>;
    using OnDisconnect     = std::function<void(const eph::core::ErrorInfo&)>;
    using OnReconnect      = std::function<void(uint32_t attempt, uint64_t duration_ns)>;
    using AttachFn         = std::function<std::expected<void, eph::core::ErrorInfo>(S*)>;
    using DetachFn         = std::function<void(S*)>;
    /// @brief Optional fine-grained event hook (see `ReconnectEvent`). The
    ///        callback runs synchronously on the driver thread that called
    ///        `start()` / `tick()` / `mark_disconnected()`. It MUST NOT
    ///        propagate exceptions — the orchestrator's transition path is
    ///        `noexcept` and an escaping exception will terminate via the
    ///        project's noexcept boundary (`std::terminate`).
    using OnReconnectEvent = std::function<void(const ReconnectEvent&)>;

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

    /// @brief Install (or replace) the fine-grained event hook.
    ///
    /// Backwards-compatible addition: pre-existing `OnDisconnect` /
    /// `OnReconnect` callbacks continue to fire as before. The event hook
    /// is supplementary — it is invoked at every state transition with a
    /// `ReconnectEvent` carrying kind / attempt / timing / error context.
    ///
    /// Mutator (rather than ctor arg) because `ReconnectOrchestrator` is
    /// non-movable and we don't want to grow the ctor parameter list every
    /// time a new optional hook lands. Pass `{}` to clear.
    ///
    /// Thread safety: install before `start()` (or while quiescent). The
    /// orchestrator does not internally synchronize callback storage —
    /// installing concurrently with a live `tick()` is undefined.
    void set_on_reconnect_event(OnReconnectEvent fn) noexcept {
        on_event_ = std::move(fn);
    }

    // ── Accessors ───────────────────────────────────────────────────────────

    /// @brief Current Stream pointer. Non-null only in `Connected` state.
    [[nodiscard]] S* current() noexcept { return stream_.get(); }
    [[nodiscard]] const S* current() const noexcept { return stream_.get(); }

    /// @brief Current state machine state.
    [[nodiscard]] ReconnectState state() const noexcept { return state_; }

    /// @brief Total attempts since construction (or last `start()`).
    /// Mirrors `policy_.attempts()`.
    [[nodiscard]] uint32_t attempts() const noexcept { return policy_.attempts(); }

    /// @brief Read one of the five orchestrator metrics (see
    ///        `ReconnectMetric` enum for the full list — count,
    ///        failures, duration_ns sum, last duration_ns,
    ///        subscribe_replay_count).
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
    /// @brief Convenience accessor for `kSubscribeReplayCount` (counter).
    [[nodiscard]] uint64_t subscribe_replay_count() const noexcept {
        return metric(ReconnectMetric::kSubscribeReplayCount);
    }

    /// @brief Acknowledge that a subscribe-replay completed successfully
    ///        for the current session. Bumps `kSubscribeReplayCount` by 1.
    ///
    /// Intended call site: inside the user's `OnReconnect` callback (or
    /// the `OnReconnectEvent::Connected` event handler) once they have
    /// finished re-sending the venue-specific subscribe payload — and,
    /// for ACK-bearing protocols, confirmed the venue accepted it. The
    /// orchestrator deliberately does NOT auto-bump from `try_attempt_`
    /// because "stream connected" and "subscriptions restored" are
    /// distinct events: a venue can close the socket on a malformed
    /// subscribe even though the TCP/TLS/WS stack is happy.
    ///
    /// Callable from any state — the metric is purely additive and we
    /// don't gate on `state_` so a caller that bumps from a custom
    /// follow-up callback (e.g. settle-ack handler) doesn't have to
    /// race the state machine. Use `subscribe_replay_count()` to read.
    void note_subscribe_replay() noexcept {
        // Memory order: relaxed mirrors every other metric increment in
        // this header. Reader thread (publisher) tolerates slightly stale
        // reads in exchange for zero contention with the driver thread.
        const auto idx = static_cast<std::size_t>(
            ReconnectMetric::kSubscribeReplayCount);
        const auto v = metrics_[idx].fetch_add(1, std::memory_order_relaxed) + 1;
        SPDLOG_LOGGER_TRACE(detail::reconnect_logger(),
            "ReconnectOrchestrator::note_subscribe_replay: count={} state={}",
            v, to_string(state_));
        // Avoid -Wunused-variable when SPDLOG_ACTIVE_LEVEL > TRACE strips
        // the macro: `v` is captured into the format args list which the
        // preprocessor erases, so cast-to-void keeps both compile modes
        // warning-clean.
        (void)v;
    }

private:
    ReconnectConfig    cfg_;
    ReconnectPolicy    policy_;
    Factory            factory_;
    OnDisconnect       on_disconnect_;
    OnReconnect        on_reconnect_;
    AttachFn           attach_;
    DetachFn           detach_;
    OnReconnectEvent   on_event_;             ///< T2.14 fine-grained event hook (optional).

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

    void try_attempt_(uint64_t now_tsc) noexcept;
    void enter_backoff_(uint64_t now_tsc) noexcept;

    /// @brief Emit a `ReconnectEvent` to the user-supplied hook (if any).
    ///        No-op when `on_event_` is empty. Centralised so call sites
    ///        stay one-liners and the structured-payload composition is
    ///        kept in one place.
    /// @param kind         Which transition fired.
    /// @param now_tsc      Caller-provided TSC (passed through verbatim so
    ///                     the event timestamp matches the same reference
    ///                     as the surrounding state mutation).
    /// @param duration_ns  Populated for `Connected` only; else 0.
    /// @param err          Populated for `AttemptFailed` / `AttachFailed`
    ///                     only; else `{Ok, ""}`.
    void emit_event_(ReconnectEventKind kind,
                     uint64_t now_tsc,
                     uint64_t duration_ns = 0,
                     eph::core::ErrorInfo err =
                         eph::core::ErrorInfo{eph::core::Error::Ok, ""}) noexcept {
        if (!on_event_) return;
        // `policy_.attempts()` counts *failed* attempts so far; the in-flight
        // (or just-finished) attempt's natural 1-based number is therefore
        // `attempts() + 1`. Using the +1 form keeps every event consistent
        // with the legacy `OnReconnect` callback (which already adds 1 at
        // its call site for the same reason — first success is "attempt 1").
        // Without the +1, Connected events report N-1 while the legacy
        // callback reports N for the same success, a silent contract drift
        // that no test currently catches.
        on_event_(ReconnectEvent{
            .kind        = kind,
            .attempt     = policy_.attempts() + 1,
            .event_tsc   = now_tsc,
            .duration_ns = duration_ns,
            .error       = err,
        });
    }
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
    // Data-driven loop so newly added enum entries flow through without
    // touching this function. The single gauge entry
    // (`kLastReconnectDurationNs`) is treated as the special case: every
    // other metric is a monotonic counter. If a future metric is also
    // gauge-shaped, extend this branch — keeping the dispatch policy
    // explicit (rather than e.g. a bool[] table) since gauge vs counter
    // is a semantic decision the maintainer should consciously make.
    for (std::size_t i = 0;
         i < static_cast<std::size_t>(M::kCount); ++i) {
        const auto m  = static_cast<M>(i);
        const auto v  = orch.metric(m);
        const auto nm = kReconnectMetricNames[i];
        if (m == M::kLastReconnectDurationNs) {
            sink.push_gauge(nm, static_cast<double>(v), tags);
        } else {
            sink.push_counter(nm, static_cast<int64_t>(v), tags);
        }
    }
}

// ---------------------------------------------------------------------------
// Method implementations (header-only)
// ---------------------------------------------------------------------------

template <Stream S>
inline std::expected<void, eph::core::ErrorInfo>
ReconnectOrchestrator<S>::start(uint64_t now_tsc) noexcept {
    auto* log = detail::reconnect_logger();
    if (state_ != ReconnectState::Idle) [[unlikely]] {
        SPDLOG_LOGGER_ERROR(log,
            "ReconnectOrchestrator::start called in non-Idle state ({})",
            to_string(state_));
        return std::unexpected(eph::core::ErrorInfo{
            eph::core::Error::InvalidConfig,
            "ReconnectOrchestrator::start called in non-Idle state"});
    }
    if (!factory_) [[unlikely]] {
        SPDLOG_LOGGER_ERROR(log, "ReconnectOrchestrator::start: factory is null");
        state_ = ReconnectState::Failed;
        return std::unexpected(eph::core::ErrorInfo{
            eph::core::Error::InvalidConfig,
            "ReconnectOrchestrator: factory callback is required"});
    }
    SPDLOG_LOGGER_DEBUG(log,
        "ReconnectOrchestrator::start: kicking off initial connection");
    // Initial attempt has no preceding disconnect — duration is measured
    // from start() to first Connected, so disconnect_tsc_ = now_tsc.
    disconnect_tsc_ = now_tsc;
    try_attempt_(now_tsc);
    return {};
}

template <Stream S>
inline void ReconnectOrchestrator<S>::tick(uint64_t now_tsc) noexcept {
    if (stopped_) [[unlikely]] return;
    switch (state_) {
        case ReconnectState::Idle:
        case ReconnectState::Failed:
            return;  // No-op
        case ReconnectState::Connecting:
            // Should not normally remain in Connecting across ticks — try_attempt_
            // is synchronous. Defensive no-op for callers that subclass/override.
            return;
        case ReconnectState::Connected: {
            if (!cfg_.auto_detect_via_state || !stream_) return;
            const auto s = stream_->state();
            // Established is the steady state; Listen / SynSent / SynReceived
            // are transient during fresh handshake (we won't see them post-create).
            // Anything else (Closed / FinWait* / TimeWait / etc.) means peer
            // closed or aborted.
            if (s == TcpState::Established) return;
            SPDLOG_LOGGER_INFO(detail::reconnect_logger(),
                "ReconnectOrchestrator: auto-detected disconnect via state={}",
                tcp_state_name(s));
            mark_disconnected(eph::core::ErrorInfo{
                eph::core::Error::Disconnected,
                "auto-detected via stream state"});
            return;
        }
        case ReconnectState::Backoff:
            if (now_tsc >= next_attempt_tsc_) {
                try_attempt_(now_tsc);
            }
            return;
    }
}

template <Stream S>
inline void
ReconnectOrchestrator<S>::mark_disconnected(eph::core::ErrorInfo reason) noexcept {
    auto* log = detail::reconnect_logger();
    if (state_ != ReconnectState::Connected) {
        SPDLOG_LOGGER_WARN(log,
            "ReconnectOrchestrator::mark_disconnected ignored in state {} ({})",
            to_string(state_), reason.detail);
        return;
    }
    SPDLOG_LOGGER_WARN(log,
        "ReconnectOrchestrator: disconnect — {}", reason.detail);

    // 1. Detach from poller (if user supplied a hook).
    if (detach_ && stream_) detach_(stream_.get());

    // 2. User-level event.
    if (on_disconnect_) on_disconnect_(reason);

    // 3. Drop the stream.
    stream_.reset();

    // 4. Record disconnect time (used to compute duration_ns when the next
    //    factory call succeeds).
    disconnect_tsc_ = eph::utils::TSC::now();

    // 5. Emit the structured event BEFORE entering backoff so subscribers
    //    see ordering: DisconnectDetected then BackoffScheduled (or Failed).
    //    Pre-existing OnDisconnect callback already fired above; the event
    //    hook is purely additive and fires regardless of which path
    //    (auto-detect vs manual) tripped this entry.
    emit_event_(ReconnectEventKind::DisconnectDetected, disconnect_tsc_,
                /*duration_ns=*/0, reason);

    // 6. Schedule the next attempt.
    enter_backoff_(disconnect_tsc_);
}

template <Stream S>
inline void ReconnectOrchestrator<S>::stop() noexcept {
    stopped_ = true;
    SPDLOG_LOGGER_INFO(detail::reconnect_logger(),
        "ReconnectOrchestrator::stop: halting further reconnect attempts "
        "(state={}, count={}, failures={})",
        to_string(state_),
        metric(ReconnectMetric::kReconnectCount),
        metric(ReconnectMetric::kReconnectFailures));
}

template <Stream S>
inline void ReconnectOrchestrator<S>::try_attempt_(uint64_t now_tsc) noexcept {
    auto* log = detail::reconnect_logger();
    state_ = ReconnectState::Connecting;
    SPDLOG_LOGGER_DEBUG(log,
        "ReconnectOrchestrator: factory attempt #{}", policy_.attempts() + 1);

    // Emit AttemptStarted *before* the factory call so observers can timestamp
    // the latency floor (factory entry → Connected) externally if they wish.
    emit_event_(ReconnectEventKind::AttemptStarted, now_tsc);

    auto r = factory_();
    if (!r) {
        // Factory failure path.
        metrics_[static_cast<std::size_t>(ReconnectMetric::kReconnectFailures)]
            .fetch_add(1, std::memory_order_relaxed);
        SPDLOG_LOGGER_ERROR(log,
            "ReconnectOrchestrator: factory failed: {}", r.error().detail);
        if (on_disconnect_) on_disconnect_(r.error());
        // Emit AttemptFailed before transitioning to Backoff/Failed so the
        // event order matches the conceptual flow: failure THEN scheduling.
        emit_event_(ReconnectEventKind::AttemptFailed, now_tsc,
                    /*duration_ns=*/0, r.error());
        enter_backoff_(now_tsc);
        return;
    }

    // Factory produced a Stream. Run the optional attach hook.
    auto fresh = std::move(*r);
    if (attach_) {
        auto ar = attach_(fresh.get());
        if (!ar) {
            metrics_[static_cast<std::size_t>(ReconnectMetric::kReconnectFailures)]
                .fetch_add(1, std::memory_order_relaxed);
            SPDLOG_LOGGER_ERROR(log,
                "ReconnectOrchestrator: attach failed: {}", ar.error().detail);
            if (on_disconnect_) on_disconnect_(ar.error());
            // Stream gets dropped here (RAII close). Emit a distinct
            // AttachFailed kind so observers can tell apart "couldn't even
            // build a Stream" (AttemptFailed) from "Stream built but Poller
            // rejected it" (AttachFailed) — they typically need different
            // operator response.
            emit_event_(ReconnectEventKind::AttachFailed, now_tsc,
                        /*duration_ns=*/0, ar.error());
            enter_backoff_(now_tsc);
            return;
        }
    }

    // Success — promote to Connected, record metrics, fire user callback.
    stream_ = std::move(fresh);
    state_ = ReconnectState::Connected;

    const uint64_t cycles = (now_tsc >= disconnect_tsc_)
                               ? (now_tsc - disconnect_tsc_) : 0;
    const auto ns_opt = eph::utils::TSC::to_ns(cycles);
    const uint64_t dur_ns = ns_opt
        ? static_cast<uint64_t>(*ns_opt)
        : 0;  // Uncalibrated TSC — duration unobservable; metric stays at 0.

    metrics_[static_cast<std::size_t>(ReconnectMetric::kReconnectCount)]
        .fetch_add(1, std::memory_order_relaxed);
    metrics_[static_cast<std::size_t>(ReconnectMetric::kReconnectDurationNs)]
        .fetch_add(dur_ns, std::memory_order_relaxed);
    metrics_[static_cast<std::size_t>(ReconnectMetric::kLastReconnectDurationNs)]
        .store(dur_ns, std::memory_order_relaxed);

    const uint32_t attempt = policy_.attempts() + 1;  // +1 so first success is "attempt 1"
    SPDLOG_LOGGER_INFO(log,
        "ReconnectOrchestrator: connected on attempt {} (duration {} ns)",
        attempt, dur_ns);

    if (on_reconnect_) on_reconnect_(attempt, dur_ns);

    // Emit Connected with the full duration_ns payload (matches the
    // legacy on_reconnect_ second arg and the `last_reconnect_duration_ns`
    // metric — see EventDurationMatchesMetric test). The event's `attempt`
    // field is filled by emit_event_ from `policy_.attempts() + 1` and
    // snapshots the value BEFORE policy_.reset() runs below — so a
    // first-success Connected reports `attempt=1`, matching the legacy
    // `attempt` variable on line above.
    emit_event_(ReconnectEventKind::Connected, now_tsc, dur_ns);

    policy_.reset();
}

template <Stream S>
inline void ReconnectOrchestrator<S>::enter_backoff_(uint64_t now_tsc) noexcept {
    auto* log = detail::reconnect_logger();
    if (!policy_.should_reconnect()) {
        state_ = ReconnectState::Failed;
        SPDLOG_LOGGER_WARN(log,
            "ReconnectOrchestrator: reconnect budget exhausted after {} attempts; "
            "transitioning to Failed (terminal)",
            policy_.attempts());
        // Terminal Failed kind — observers should treat this as the end of
        // the reconnect lifecycle for this orchestrator instance.
        emit_event_(ReconnectEventKind::Failed, now_tsc);
        return;
    }
    const auto delay = policy_.next_backoff();
    const auto cycles_opt = eph::utils::TSC::to_cycles(delay);
    // Fallback when TSC is uncalibrated: assume 3 GHz so we still progress
    // in tests / unusual setups. Production paths must call TSC::init() per
    // CLAUDE.md, but we don't crash if they forgot.
    const uint64_t cycles = cycles_opt
        ? *cycles_opt
        : static_cast<uint64_t>(delay.count()) * 3'000'000ULL;
    next_attempt_tsc_ = now_tsc + cycles;
    state_ = ReconnectState::Backoff;
    SPDLOG_LOGGER_DEBUG(log,
        "ReconnectOrchestrator: scheduled retry in {} ms (attempt {} of policy)",
        delay.count(), policy_.attempts());
    // Emit AFTER state_ and next_attempt_tsc_ are committed so an observer
    // that immediately reads orch.state() inside the callback sees Backoff.
    emit_event_(ReconnectEventKind::BackoffScheduled, now_tsc);
}

} // namespace eph::net
