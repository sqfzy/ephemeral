/// @file core/runner.hpp
/// BenchRunner — drives every scenario through warmup → measurement → report.
///
/// Three sweep entry points correspond to the three sweep dimensions:
///
///   - run_rtt_sweep(payloads)        : tcp / udp / ws / exchange/md_udp
///   - run_rtt_inflight_sweep(N list) : exchange/order
///   - run_oneway()                   : exchange/market
///
/// All three call the same internal `run_one_window()` skeleton:
///   prepare → pre-warmup (kPreWarmupRounds dummy rounds) → BenchTimer
///   warmup → measurement loop → report → reset histograms → cleanup.
///
/// Hot-path dispatch is template-driven (no virtual / no std::function),
/// so the compiler inlines `do_one_*()` straight into the measurement
/// loop body.
#pragma once

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/utils/hdr_histogram.hpp"

#include "config.hpp"
#include "hist_report.hpp"
#include "sample.hpp"
#include "scenario_concept.hpp"
#include "signal.hpp"
#include "timer.hpp"
#include "tsc_protocol.hpp"

namespace bench {

class BenchRunner {
public:
    BenchRunner(CommonConfig cfg,
                std::string_view scenario_name,
                std::string_view transport_name)
        : cfg_(std::move(cfg))
        , scenario_name_(scenario_name)
        , transport_name_(transport_name)
        , rtt_(kHistMin, kHistMax, kHistSig)
        , tx_(kHistMin, kHistMax, kHistSig)
        , rx_(kHistMin, kHistMax, kHistSig)
        , srv_(kHistMin, kHistMax, kHistSig)
        , oneway_(kHistMin, kHistMax, kHistSig)
    {}

    /// RTT sweep over payload sizes — used by tcp / udp / ws / md_udp.
    template <RttScenario S>
    void run_rtt_sweep(S& scenario, std::span<const size_t> payloads) {
        for (size_t payload : payloads) {
            if (!g_running.load(std::memory_order_relaxed)) break;
            run_one_rtt_window(scenario, payload, /*inflight*/ -1);
        }
    }

    /// RTT sweep over inflight values — used by exchange/order.
    /// `prepare(inflight)` is called with the inflight value as the
    /// "payload" parameter so the scenario can size its in-flight buffer.
    template <RttScenario S>
    void run_rtt_inflight_sweep(S& scenario, std::span<const int> inflights) {
        for (int n : inflights) {
            if (!g_running.load(std::memory_order_relaxed)) break;
            run_one_rtt_window(scenario, static_cast<size_t>(n), n);
        }
    }

    /// 1-leg sweep — used by exchange/market.
    template <OneWayScenario S>
    void run_oneway(S& scenario) {
        if (!scenario.prepare()) {
            spdlog::error("{} ({}): prepare() failed",
                          scenario_name_, transport_name_);
            return;
        }
        spdlog::info("{} ({}): pre_warmup={} warmup={}s duration={}s (oneway)",
                     scenario_name_, transport_name_,
                     kPreWarmupRounds, cfg_.warmup.count(), cfg_.duration.count());

        OneWaySample sample{};
        for (size_t i = 0; i < kPreWarmupRounds; ++i) {
            if (!g_running.load(std::memory_order_relaxed)) {
                scenario.cleanup();
                return;
            }
            (void)scenario.do_one_recv(sample);
        }

        BenchTimer timer;
        timer.start(cfg_.warmup, cfg_.duration);
        while (timer.is_running() && g_running.load(std::memory_order_relaxed)) {
            if (!scenario.do_one_recv(sample)) continue;
            if (timer.is_warmup()) continue;
            record_oneway(sample);
        }

        BenchResult result{};
        result.rx = compute_stats(oneway_); // report oneway latency in RX leg slot
        std::string label = std::string(scenario_name_) + " (" +
                            std::string(transport_name_) + ", oneway)";
        print_bench_result(label, 0, result);
        oneway_.reset();
        scenario.cleanup();
    }

private:
    template <RttScenario S>
    void run_one_rtt_window(S& scenario, size_t payload_or_inflight,
                            int inflight_marker) {
        if (!scenario.prepare(payload_or_inflight)) {
            spdlog::error("{} ({}): prepare({}) failed",
                          scenario_name_, transport_name_, payload_or_inflight);
            return;
        }

        if (inflight_marker >= 0) {
            spdlog::info("{} ({}): inflight={} pre_warmup={} warmup={}s duration={}s",
                         scenario_name_, transport_name_, inflight_marker,
                         kPreWarmupRounds, cfg_.warmup.count(), cfg_.duration.count());
        } else {
            spdlog::info("{} ({}): payload={}B pre_warmup={} warmup={}s duration={}s",
                         scenario_name_, transport_name_, payload_or_inflight,
                         kPreWarmupRounds, cfg_.warmup.count(), cfg_.duration.count());
        }

        // Pre-warmup: discard kPreWarmupRounds rounds to absorb cold-start
        // (route cache miss, ARP, scheduler placement, NIC ring fill).
        RttSample sample{};
        for (size_t i = 0; i < kPreWarmupRounds; ++i) {
            if (!g_running.load(std::memory_order_relaxed)) {
                scenario.cleanup();
                return;
            }
            (void)scenario.do_one_rtt(sample);
        }

        // Timer-driven warmup + measurement.
        BenchTimer timer;
        timer.start(cfg_.warmup, cfg_.duration);
        while (timer.is_running() && g_running.load(std::memory_order_relaxed)) {
            if (!scenario.do_one_rtt(sample)) continue;
            if (timer.is_warmup()) continue;
            record_rtt(sample);
        }

        BenchResult result{
            compute_stats(rtt_), compute_stats(tx_),
            compute_stats(rx_),  compute_stats(srv_),
        };
        std::string label = std::string(scenario_name_) + " (" +
                            std::string(transport_name_) + ")";
        print_bench_result(label, inflight_marker >= 0 ? 0 : payload_or_inflight, result);

        rtt_.reset(); tx_.reset(); rx_.reset(); srv_.reset();
        scenario.cleanup();
    }

    void record_rtt(const RttSample& s) noexcept {
        if (s.client_recv_tsc > s.client_send_tsc) {
            (void)rtt_.record(tsc::cycles_to_ns(
                s.client_recv_tsc - s.client_send_tsc));
        }
        if (s.server_recv_tsc > 0 && s.server_recv_tsc > s.client_send_tsc) {
            (void)tx_.record(tsc::cycles_to_ns(
                s.server_recv_tsc - s.client_send_tsc));
        }
        if (s.server_send_tsc > 0 && s.client_recv_tsc > s.server_send_tsc) {
            (void)rx_.record(tsc::cycles_to_ns(
                s.client_recv_tsc - s.server_send_tsc));
        }
        if (s.server_recv_tsc > 0 && s.server_send_tsc > s.server_recv_tsc) {
            (void)srv_.record(tsc::cycles_to_ns(
                s.server_send_tsc - s.server_recv_tsc));
        }
    }

    void record_oneway(const OneWaySample& s) noexcept {
        if (s.consumer_tsc > s.producer_tsc) {
            (void)oneway_.record(tsc::cycles_to_ns(
                s.consumer_tsc - s.producer_tsc));
        }
    }

    static constexpr size_t kPreWarmupRounds = 2000;

    CommonConfig cfg_;
    std::string_view scenario_name_;
    std::string_view transport_name_;
    eph::utils::HdrHistogram rtt_;
    eph::utils::HdrHistogram tx_;
    eph::utils::HdrHistogram rx_;
    eph::utils::HdrHistogram srv_;
    eph::utils::HdrHistogram oneway_;
};

} // namespace bench
