/// @file core/runner.hpp
/// BenchRunner — drives every scenario through warmup → measurement → report.
///
/// Three sweep entry points:
///   - run_rtt_sweep(payloads)        : tcp / udp / ws / exchange/md_udp
///   - run_rtt_inflight_sweep(N list) : exchange/order
///   - run_oneway()                   : exchange/market
///
/// All three share the same skeleton: prepare → pre-warmup → PhasedTimer
/// warmup/measurement → print → reset → cleanup. Latency samples are fed
/// into 4 `eph::utils::Recorder` instances (one per leg) that own the
/// HdrHistogram and cycle→ns conversion.
#pragma once

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

#include "eph/utils/phased_timer.hpp"
#include "eph/utils/recorder.hpp"
#include "eph/utils/time.hpp"

#include "config.hpp"
#include "sample.hpp"
#include "scenario_concept.hpp"
#include "signal.hpp"

namespace bench {

/// Orchestrates a latency benchmark from warmup through measurement and
/// result reporting. Holds 5 `Recorder` instances (4 RTT legs + 1 oneway)
/// and feeds each scenario sample into the right recorders. All hot-path
/// dispatch goes through template member functions so scenarios can be
/// fully inlined into the measurement loop.
///
/// Construction captures the warmup/duration from `CommonConfig` or
/// `BenchConfig`; after that the caller picks one of:
///   - `run_rtt_sweep`          for tcp / udp / ws / exchange/md_udp
///   - `run_rtt_inflight_sweep` for exchange/order
///   - `run_oneway`             for exchange/market
class BenchRunner {
public:
    BenchRunner(CommonConfig cfg,
                std::string_view scenario_name,
                std::string_view transport_name)
        : warmup_(cfg.warmup)
        , duration_(cfg.duration)
        , label_(std::string(scenario_name) + " (" + std::string(transport_name) + ")")
        , rtt_(std::string(scenario_name) + "/rtt")
        , tx_ (std::string(scenario_name) + "/tx")
        , rx_ (std::string(scenario_name) + "/rx")
        , srv_(std::string(scenario_name) + "/srv")
        , oneway_(std::string(scenario_name) + "/rx")
    {}

    /// BenchConfig overload — used by stage-4+ lat_*.cpp scenarios that
    /// load all configuration from bench.conf instead of CLI flags.
    BenchRunner(const BenchConfig& cfg,
                std::string_view scenario_name,
                std::string_view transport_name)
        : warmup_(cfg.warmup)
        , duration_(cfg.duration)
        , label_(std::string(scenario_name) + " (" + std::string(transport_name) + ")")
        , rtt_(std::string(scenario_name) + "/rtt")
        , tx_ (std::string(scenario_name) + "/tx")
        , rx_ (std::string(scenario_name) + "/rx")
        , srv_(std::string(scenario_name) + "/srv")
        , oneway_(std::string(scenario_name) + "/rx")
    {}

    /// RTT sweep over payload sizes — used by tcp / udp / ws / md_udp.
    template <RttScenario S>
    void run_rtt_sweep(S& scenario, std::span<const size_t> payloads) {
        for (size_t p : payloads) {
            if (!g_running.load(std::memory_order_relaxed)) break;
            run_rtt_window(scenario, p, /*inflight*/ -1);
        }
    }

    /// RTT sweep over inflight values — used by exchange/order.
    template <RttScenario S>
    void run_rtt_inflight_sweep(S& scenario, std::span<const int> inflights) {
        for (int n : inflights) {
            if (!g_running.load(std::memory_order_relaxed)) break;
            run_rtt_window(scenario, static_cast<size_t>(n), n);
        }
    }

    /// 1-leg sweep — used by exchange/market.
    template <OneWayScenario S>
    void run_oneway(S& scenario) {
        if (!scenario.prepare()) {
            spdlog::error("{}: prepare() failed", label_);
            return;
        }
        spdlog::info("{}: pre_warmup={} warmup={}s duration={}s (oneway)",
                     label_, kPreWarmupRounds,
                     warmup_.count(), duration_.count());

        OneWaySample sample{};
        for (size_t i = 0; i < kPreWarmupRounds; ++i) {
            if (!g_running.load(std::memory_order_relaxed)) {
                scenario.cleanup();
                return;
            }
            (void)scenario.do_one_recv(sample);
        }

        eph::utils::PhasedTimer timer;
        timer.start(warmup_, duration_);
        while (timer.is_running() && g_running.load(std::memory_order_relaxed)) {
            if (!scenario.do_one_recv(sample)) continue;
            if (timer.is_warmup()) continue;
            if (sample.consumer_tsc > sample.producer_tsc) {
                (void)oneway_.record(sample.consumer_tsc - sample.producer_tsc);
            }
        }

        spdlog::info("== BENCH {} (oneway) ==", label_);
        print_leg("RX", oneway_);
        oneway_.reset();
        scenario.cleanup();
    }

private:
    template <RttScenario S>
    void run_rtt_window(S& scenario, size_t sweep_value, int inflight_marker) {
        if (!scenario.prepare(sweep_value)) {
            spdlog::error("{}: prepare({}) failed", label_, sweep_value);
            return;
        }
        if (inflight_marker >= 0) {
            spdlog::info("{}: inflight={} pre_warmup={} warmup={}s duration={}s",
                         label_, inflight_marker, kPreWarmupRounds,
                         warmup_.count(), duration_.count());
        } else {
            spdlog::info("{}: payload={}B pre_warmup={} warmup={}s duration={}s",
                         label_, sweep_value, kPreWarmupRounds,
                         warmup_.count(), duration_.count());
        }

        // Pre-warmup absorbs route cache / ARP / scheduler cold start.
        RttSample s{};
        for (size_t i = 0; i < kPreWarmupRounds; ++i) {
            if (!g_running.load(std::memory_order_relaxed)) {
                scenario.cleanup();
                return;
            }
            (void)scenario.do_one_rtt(s);
        }

        eph::utils::PhasedTimer timer;
        timer.start(warmup_, duration_);
        while (timer.is_running() && g_running.load(std::memory_order_relaxed)) {
            if (!scenario.do_one_rtt(s)) continue;
            if (timer.is_warmup()) continue;
            record_rtt(s);
        }

        // Report: "== BENCH tcp (kernel) payload=64B ==" then 4 legs.
        if (inflight_marker >= 0) {
            spdlog::info("== BENCH {} inflight={} ==", label_, inflight_marker);
        } else {
            spdlog::info("== BENCH {} payload={}B ==", label_, sweep_value);
        }
        print_leg("RTT", rtt_);
        print_leg("TX ", tx_);
        print_leg("RX ", rx_);
        print_leg("SRV", srv_);

        rtt_.reset(); tx_.reset(); rx_.reset(); srv_.reset();
        scenario.cleanup();
    }

    void record_rtt(const RttSample& s) noexcept {
        // Each leg is a TSC cycle delta; Recorder converts to ns at
        // compute_stats time.
        if (s.client_recv_tsc > s.client_send_tsc) {
            (void)rtt_.record(s.client_recv_tsc - s.client_send_tsc);
        }
        if (s.server_recv_tsc > 0 && s.server_recv_tsc > s.client_send_tsc) {
            (void)tx_.record(s.server_recv_tsc - s.client_send_tsc);
        }
        if (s.server_send_tsc > 0 && s.client_recv_tsc > s.server_send_tsc) {
            (void)rx_.record(s.client_recv_tsc - s.server_send_tsc);
        }
        if (s.server_recv_tsc > 0 && s.server_send_tsc > s.server_recv_tsc) {
            (void)srv_.record(s.server_send_tsc - s.server_recv_tsc);
        }
    }

    static void print_leg(std::string_view leg, const eph::utils::Recorder& r) {
        auto stats = r.compute_stats();
        if (!stats) {
            spdlog::info("    {} (no samples)", leg);
            return;
        }
        spdlog::info("    {} n={:>9} min={:>7.0f} p50={:>7.0f} p99={:>7.0f} "
                     "p999={:>7.0f} max={:>7.0f}",
                     leg, stats->count,
                     stats->min_ns, stats->p50_ns, stats->p99_ns,
                     stats->p999_ns, stats->max_ns);
    }

    static constexpr size_t kPreWarmupRounds = 2000;

    std::chrono::seconds warmup_;
    std::chrono::seconds duration_;
    std::string  label_;
    eph::utils::Recorder rtt_;
    eph::utils::Recorder tx_;
    eph::utils::Recorder rx_;
    eph::utils::Recorder srv_;
    eph::utils::Recorder oneway_;
};

} // namespace bench
