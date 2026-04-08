/// @file bench_loop.hpp
/// Benchmark measurement framework — timer, statistics, reporting, JSONL output.
///
/// Provides:
///   - BenchTimer:      warmup + measure phase controller
///   - LegStats:        percentile statistics for one latency leg
///   - BenchResult:     4-leg result (RTT, TX, RX, Server)
///   - compute_stats:   pure function — HdrHistogram → LegStats
///   - print_stats:     spdlog output for one leg
///   - print_bench_result: spdlog output for full 4-leg result
///   - JsonlWriter:     machine-readable JSONL file output
///   - MockHandle:      unified mock server lifecycle
///   - g_running:       signal-safe shutdown flag
///   - pin_or_die:      CPU affinity helper

#pragma once

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <spdlog/spdlog.h>

#include "eph/utils/cpu.hpp"
#include "eph/utils/hdr_histogram.hpp"

namespace bench {

// ── Signal handling ──────────────────────────────────────────────────────

inline std::atomic<bool> g_running{true};

namespace detail {
inline void sig_handler(int) {
    g_running.store(false, std::memory_order_relaxed);
}
} // namespace detail

inline void install_signal_handlers() {
    std::signal(SIGINT, detail::sig_handler);
    std::signal(SIGTERM, detail::sig_handler);
}

// ── CPU pinning ──────────────────────────────────────────────────────────

inline void pin_or_die(int cpu, const char* name) {
    auto r = eph::utils::set_thread_affinity(cpu, name);
    if (!r) {
        spdlog::error("Failed to pin {} to core {}: {}", name, cpu, r.error());
        std::exit(1);
    }
    spdlog::info("Pinned {} to core {}", name, cpu);
}

// ── BenchTimer ───────────────────────────────────────────────────────────

/// Tracks warmup and measurement phases. Zero overhead — all checks are
/// simple comparisons against steady_clock::time_point.
class BenchTimer {
public:
    /// Start the timer. Warmup runs for `warmup` seconds, then measurement
    /// runs for `duration` seconds. Total wall time = warmup + duration.
    void start(std::chrono::seconds warmup, std::chrono::seconds duration) {
        start_ = std::chrono::steady_clock::now();
        warmup_end_ = start_ + warmup;
        measure_end_ = warmup_end_ + duration;
    }

    /// True during warmup phase — samples should be discarded.
    [[nodiscard]] bool is_warmup() const noexcept {
        return std::chrono::steady_clock::now() < warmup_end_;
    }

    /// True while either warmup or measurement is active.
    /// Returns false after warmup + duration has elapsed.
    [[nodiscard]] bool is_running() const noexcept {
        return std::chrono::steady_clock::now() < measure_end_;
    }

    /// Seconds since start().
    [[nodiscard]] int64_t elapsed_s() const noexcept {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_).count();
    }

private:
    std::chrono::steady_clock::time_point start_{};
    std::chrono::steady_clock::time_point warmup_end_{};
    std::chrono::steady_clock::time_point measure_end_{};
};

// ── Statistics ───────────────────────────────────────────────────────────

struct LegStats {
    double p50_us  = 0;
    double p99_us  = 0;
    double p999_us = 0;
    double max_us  = 0;
    uint64_t samples = 0;
};

struct BenchResult {
    LegStats rtt;
    LegStats tx;
    LegStats rx;
    LegStats srv;
};

/// Pure computation: extract percentile statistics from a histogram.
/// Returns zeroed stats if the histogram is empty.
[[nodiscard]] inline LegStats compute_stats(const eph::utils::HdrHistogram& h) {
    if (h.empty()) return {};
    return LegStats{
        .p50_us  = h.get_value_at_percentile(50.0) / 1000.0,
        .p99_us  = h.get_value_at_percentile(99.0) / 1000.0,
        .p999_us = h.get_value_at_percentile(99.9) / 1000.0,
        .max_us  = h.get_max_value() / 1000.0,
        .samples = h.get_total_count(),
    };
}

/// Print one latency leg to spdlog INFO.
inline void print_stats(const char* label, const LegStats& s) {
    if (s.samples == 0) return;
    spdlog::info("  {:<12s} p50={:.1f}us  p99={:.1f}us  p999={:.1f}us  max={:.1f}us  n={}",
                 label, s.p50_us, s.p99_us, s.p999_us, s.max_us, s.samples);
}

/// Print full 4-leg benchmark result.
inline void print_bench_result(const char* scenario_label, size_t payload_size,
                               const BenchResult& r) {
    spdlog::info("──────────────────────────────────────────");
    if (payload_size > 0) {
        spdlog::info("  {} | payload={}B", scenario_label, payload_size);
    } else {
        spdlog::info("  {}", scenario_label);
    }
    spdlog::info("──────────────────────────────────────────");
    print_stats("RTT", r.rtt);
    print_stats("TX (c→s)", r.tx);
    print_stats("RX (s→c)", r.rx);
    print_stats("Server", r.srv);
    spdlog::info("──────────────────────────────────────────");
}

// ── JSONL Writer ─────────────────────────────────────────────────────────

/// Writes machine-readable JSONL (one JSON object per line) to a file.
/// Construct with empty path to disable output (all writes become no-ops).
class JsonlWriter {
public:
    explicit JsonlWriter(const std::string& path) {
        if (path.empty()) return;
        file_ = std::fopen(path.c_str(), "a");
        if (!file_) {
            spdlog::error("JsonlWriter: failed to open {}", path);
        } else {
            spdlog::info("JsonlWriter: writing to {}", path);
        }
    }

    ~JsonlWriter() {
        if (file_) std::fclose(file_);
    }

    JsonlWriter(const JsonlWriter&) = delete;
    JsonlWriter& operator=(const JsonlWriter&) = delete;

    /// Write one 4-leg measurement record. Skips legs with samples=0.
    void write(std::string_view scenario, std::string_view transport,
               size_t payload_size, const BenchResult& result) {
        if (!file_) return;
        write_leg(scenario, transport, payload_size, "rtt", result.rtt);
        write_leg(scenario, transport, payload_size, "tx", result.tx);
        write_leg(scenario, transport, payload_size, "rx", result.rx);
        write_leg(scenario, transport, payload_size, "srv", result.srv);
        std::fflush(file_);
    }

private:
    void write_leg(std::string_view scenario, std::string_view transport,
                   size_t payload, const char* leg, const LegStats& s) {
        if (s.samples == 0) return;
        std::fprintf(file_,
            "{\"scenario\":\"%.*s\",\"transport\":\"%.*s\","
            "\"payload\":%zu,\"leg\":\"%s\","
            "\"p50_us\":%.1f,\"p99_us\":%.1f,\"p999_us\":%.1f,"
            "\"max_us\":%.1f,\"samples\":%llu}\n",
            static_cast<int>(scenario.size()), scenario.data(),
            static_cast<int>(transport.size()), transport.data(),
            payload, leg,
            s.p50_us, s.p99_us, s.p999_us, s.max_us,
            static_cast<unsigned long long>(s.samples));
    }

    FILE* file_ = nullptr;
};

// ── Mock Handle ──────────────────────────────────────────────────────────

/// Unified lifecycle for in-process mock servers (used by DPDK benchmarks).
/// Kernel benchmarks use external mock processes managed by bench_latency.sh.
struct MockHandle {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> running = std::make_shared<std::atomic<bool>>(true);
};

inline void stop_mock(MockHandle& h) {
    if (h.running) h.running->store(false, std::memory_order_release);
    if (h.thread.joinable()) h.thread.join();
}

} // namespace bench
