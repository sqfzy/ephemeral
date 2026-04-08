/// @file framework/bench_runner.hpp
/// BenchRunner — the unified harness for raw and WS scenarios.
///
/// Each scenario implements a small struct with three methods:
///
///   bool prepare(size_t payload);      // resize buffers; return false to skip
///   bool do_one_round(RoundTrip& rt);  // perform one send→recv; return false on err
///   void cleanup();                    // release per-payload resources
///
/// The runner handles:
///   - per-payload pre-warmup (kPreWarmupRounds dummy round-trips)
///   - BenchTimer warmup + measurement phases
///   - HdrHistogram lifetime (one set of histograms reused across all payloads)
///   - 4-leg statistics computation, spdlog reporting, JSONL output
///
/// Hot-path dispatch is compile-time via template parameter — no virtual
/// calls, no std::function. The compiler can fully inline scenario.do_one_round
/// into the runner's measurement loop.

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/utils/hdr_histogram.hpp"

#include "bench_config.hpp"
#include "bench_stats.hpp"
#include "bench_timer.hpp"
#include "signal.hpp"
#include "tsc_protocol.hpp"

namespace bench {

/// One round-trip's raw measurements. Fields are TSC cycles (not ns).
/// Server-side fields may be 0 if the scenario does not provide them.
struct RoundTrip {
    uint64_t client_send_tsc = 0;
    uint64_t server_recv_tsc = 0;
    uint64_t server_send_tsc = 0;
    uint64_t client_recv_tsc = 0;
};

/// Concept: every scenario type must satisfy this interface.
template <typename T>
concept ScenarioLike = requires(T& s, size_t payload, RoundTrip& rt) {
    { s.prepare(payload) } -> std::same_as<bool>;
    { s.do_one_round(rt) } -> std::same_as<bool>;
    { s.cleanup() } -> std::same_as<void>;
};

/// Runner: manages histograms, timer, warmup, reporting for a scenario.
///
/// This is a non-templated class so callers can write
/// `BenchRunner runner{cfg, ...}` without specifying the scenario type.
/// `run_sweep` and `run_single` are member templates so the compiler
/// still inlines `do_one_round` calls into the measurement loop.
class BenchRunner {
public:
    BenchRunner(const BenchConfig& cfg,
                std::string_view scenario_name,
                std::string_view transport_name,
                JsonlWriter& jsonl)
        : cfg_(cfg)
        , scenario_name_(scenario_name)
        , transport_name_(transport_name)
        , jsonl_(jsonl)
        , rtt_{kHistMin, kHistMax, kHistSig}
        , tx_{kHistMin, kHistMax, kHistSig}
        , rx_{kHistMin, kHistMax, kHistSig}
        , srv_{kHistMin, kHistMax, kHistSig}
    {}

    /// Run a payload sweep. For each payload size, do pre-warmup → warmup
    /// → measure → report → reset histograms.
    template <ScenarioLike Scenario>
    void run_sweep(Scenario& scenario, const std::vector<size_t>& payloads) {
        for (size_t payload : payloads) {
            if (!g_running.load(std::memory_order_relaxed)) break;
            run_one_payload(scenario, payload);
        }
    }

    /// For 1-leg / no-payload-sweep scenarios (market_rx, order_rtt) that
    /// run a single measurement window. The scenario's prepare() will be
    /// called with payload=0.
    template <ScenarioLike Scenario>
    void run_single(Scenario& scenario) {
        run_one_payload(scenario, 0);
    }

private:
    template <ScenarioLike Scenario>
    void run_one_payload(Scenario& scenario, size_t payload) {
        if (!scenario.prepare(payload)) {
            spdlog::error("{} ({}): prepare({}) failed",
                          scenario_name_, transport_name_, payload);
            return;
        }

        if (payload > 0) {
            spdlog::info("{} ({}): payload={}B, pre_warmup={}, warmup={}s, duration={}s",
                         scenario_name_, transport_name_, payload,
                         kPreWarmupRounds, cfg_.warmup.count(), cfg_.duration.count());
        } else {
            spdlog::info("{} ({}): pre_warmup={}, warmup={}s, duration={}s",
                         scenario_name_, transport_name_,
                         kPreWarmupRounds, cfg_.warmup.count(), cfg_.duration.count());
        }

        // ── Pre-warmup: discard fixed N rounds to prime caches/routes/scheduler.
        // This is in addition to BenchTimer warmup. Pre-warmup absorbs the
        // first-iteration cold start that even 1s of timer warmup can miss
        // (route cache miss, ARP, mock thread scheduling, NIC ring fill).
        RoundTrip rt;
        for (size_t i = 0; i < kPreWarmupRounds; ++i) {
            if (!g_running.load(std::memory_order_relaxed)) {
                scenario.cleanup();
                return;
            }
            (void)scenario.do_one_round(rt);
        }

        // ── BenchTimer-controlled warmup + measurement
        BenchTimer timer;
        timer.start(cfg_.warmup, cfg_.duration);

        while (timer.is_running() && g_running.load(std::memory_order_relaxed)) {
            if (!scenario.do_one_round(rt)) continue;
            if (timer.is_warmup()) continue;
            record(rt);
        }

        // ── Report and reset
        BenchResult result{
            compute_stats(rtt_), compute_stats(tx_),
            compute_stats(rx_),  compute_stats(srv_),
        };

        // Use a temporary buffer for the label so spdlog can format it.
        // (scenario_name_ is a string_view; print_bench_result expects const char*.)
        std::string label = std::string(scenario_name_) +
                            " (" + std::string(transport_name_) + ")";
        print_bench_result(label.c_str(), payload, result);
        jsonl_.write(scenario_name_, transport_name_, payload, result);

        rtt_.reset();
        tx_.reset();
        rx_.reset();
        srv_.reset();

        scenario.cleanup();
    }

    void record(const RoundTrip& rt) {
        // RTT: client_send → client_recv
        if (rt.client_recv_tsc > rt.client_send_tsc) {
            [[maybe_unused]] auto _ = rtt_.record(
                tsc::cycles_to_ns(rt.client_recv_tsc - rt.client_send_tsc));
        }
        // TX: client_send → server_recv
        if (rt.server_recv_tsc > 0 && rt.server_recv_tsc > rt.client_send_tsc) {
            [[maybe_unused]] auto _ = tx_.record(
                tsc::cycles_to_ns(rt.server_recv_tsc - rt.client_send_tsc));
        }
        // RX: server_send → client_recv
        if (rt.server_send_tsc > 0 && rt.client_recv_tsc > rt.server_send_tsc) {
            [[maybe_unused]] auto _ = rx_.record(
                tsc::cycles_to_ns(rt.client_recv_tsc - rt.server_send_tsc));
        }
        // Server processing: server_recv → server_send
        if (rt.server_recv_tsc > 0 && rt.server_send_tsc > rt.server_recv_tsc) {
            [[maybe_unused]] auto _ = srv_.record(
                tsc::cycles_to_ns(rt.server_send_tsc - rt.server_recv_tsc));
        }
    }

    static constexpr uint64_t kHistMin = 10;
    static constexpr uint64_t kHistMax = 1'000'000'000ULL;
    static constexpr int      kHistSig = 3;
    static constexpr size_t   kPreWarmupRounds = 2000;

    const BenchConfig& cfg_;
    std::string_view scenario_name_;
    std::string_view transport_name_;
    JsonlWriter& jsonl_;

    eph::utils::HdrHistogram rtt_;
    eph::utils::HdrHistogram tx_;
    eph::utils::HdrHistogram rx_;
    eph::utils::HdrHistogram srv_;
};

} // namespace bench
