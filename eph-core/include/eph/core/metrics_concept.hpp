#pragma once

/// @file metrics_concept.hpp
/// MetricsSink concept — defines the interface for pushing metrics to
/// external monitoring systems (Prometheus, StatsD, InfluxDB, etc.).
///
/// Three metric types following Prometheus conventions:
///   - Counter: monotonically increasing integer (packets, bytes, errors)
///   - Gauge: point-in-time double (queue depth, latency percentile)
///   - Histogram: distribution sample (individual latency measurement)
///
/// Tags (key-value pairs) allow dimensional labeling:
///   sink.push_counter("tx_packets", count, {{"transport", "dpdk"}, {"symbol", "btcusdt"}});
///
/// Includes NullSink — a zero-cost sink whose methods compile to nothing.
/// For a logging-based sink, see eph/utils/console_sink.hpp.

#include <concepts>
#include <cstdint>
#include <span>
#include <string_view>

namespace eph::core {

/// @brief A single tag (key-value pair) for metric dimensional labeling.
///
/// Views into caller-owned storage — does not allocate.
struct MetricTag {
    std::string_view key;   ///< Tag key (e.g., "transport", "symbol").
    std::string_view value; ///< Tag value (e.g., "dpdk", "btcusdt").
};

/// @brief MetricsSink concept — any type that can receive counter, gauge, and
/// histogram metric values with optional dimensional tags.
///
/// Implementations must provide:
///   push_counter(name, int64_t value, tags)   — monotonic counters
///   push_gauge(name, double value, tags)       — point-in-time values
///   push_histogram(name, double value, tags)   — distribution samples
///   flush()                                     — flush buffered metrics
///
/// All methods should be noexcept — metrics collection must never
/// disrupt the data path.
///
/// @tparam T  Type to check against the MetricsSink requirements.
template <typename T>
concept MetricsSink = requires(T& sink,
    std::string_view name,
    std::span<const MetricTag> tags) {
    { sink.push_counter(name, int64_t{}, tags) } noexcept -> std::same_as<void>;
    { sink.push_gauge(name, double{}, tags) } noexcept -> std::same_as<void>;
    { sink.push_histogram(name, double{}, tags) } noexcept -> std::same_as<void>;
    { sink.flush() } noexcept -> std::same_as<void>;
};

/// @brief Zero-cost metrics sink — all methods are inline noexcept no-ops.
///
/// When used as a template parameter, the compiler eliminates all
/// metric push calls entirely at -O2.
struct NullSink {
    /// @brief No-op counter push.
    void push_counter(std::string_view, int64_t,
                      std::span<const MetricTag> = {}) noexcept {}
    /// @brief No-op gauge push.
    void push_gauge(std::string_view, double,
                    std::span<const MetricTag> = {}) noexcept {}
    /// @brief No-op histogram push.
    void push_histogram(std::string_view, double,
                        std::span<const MetricTag> = {}) noexcept {}
    /// @brief No-op flush.
    void flush() noexcept {}
};

static_assert(MetricsSink<NullSink>, "NullSink must satisfy MetricsSink");

} // namespace eph::core
