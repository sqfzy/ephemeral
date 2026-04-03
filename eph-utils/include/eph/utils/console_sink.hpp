#pragma once

/// @file console_sink.hpp
/// ConsoleSink — logs metrics to spdlog for development and debugging.
///
/// Formats each metric as a structured log line:
///   [COUNTER] tx_packets = 42 {transport=dpdk, symbol=btcusdt}
///   [GAUGE]   rx_queue_depth = 7.50 {transport=socket}
///   [HISTO]   rx_latency_ns = 350.00 {}
///
/// Use NullSink (metrics_concept.hpp) in production for zero overhead.

#include <format>
#include <span>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "eph/core/metrics_concept.hpp"

namespace eph::utils {

namespace detail {
/// @brief Lazily-initialized logger for the console metrics sink.
inline const std::shared_ptr<spdlog::logger>& console_sink_logger() {
    static auto l = [] {
        auto lg = spdlog::get("utils.console_sink");
        if (!lg) lg = spdlog::stdout_color_mt("utils.console_sink");
        return lg;
    }();
    return l;
}
} // namespace detail

/// @brief Metrics sink that logs to spdlog at INFO level.
///
/// Satisfies the `core::MetricsSink` concept. Useful for development,
/// debugging, and integration testing. Not intended for production hot
/// paths -- use `NullSink` or a buffered network sink instead.
///
/// Output format:
/// @code
///   [COUNTER] tx_packets = 42 {transport=dpdk, symbol=btcusdt}
///   [GAUGE]   rx_queue_depth = 7.50 {transport=socket}
///   [HISTO]   rx_latency_ns = 350.00 {}
/// @endcode
class ConsoleSink {
public:
    /// @brief Log an integer counter metric.
    /// @param name  Metric name (e.g., "tx_packets").
    /// @param value Current counter value.
    /// @param tags  Optional key-value tags for dimensional grouping.
    void push_counter(std::string_view name, int64_t value,
                      std::span<const core::MetricTag> tags = {}) noexcept {
        SPDLOG_LOGGER_INFO(detail::console_sink_logger(),"[COUNTER] {} = {}{}", name, value, format_tags(tags));
    }

    /// @brief Log a floating-point gauge metric.
    /// @param name  Metric name (e.g., "rx_queue_depth").
    /// @param value Current gauge value.
    /// @param tags  Optional key-value tags for dimensional grouping.
    void push_gauge(std::string_view name, double value,
                    std::span<const core::MetricTag> tags = {}) noexcept {
        SPDLOG_LOGGER_INFO(detail::console_sink_logger(),"[GAUGE]   {} = {:.2f}{}", name, value, format_tags(tags));
    }

    /// @brief Log a histogram observation.
    /// @param name  Metric name (e.g., "rx_latency_ns").
    /// @param value Observed value to record.
    /// @param tags  Optional key-value tags for dimensional grouping.
    void push_histogram(std::string_view name, double value,
                        std::span<const core::MetricTag> tags = {}) noexcept {
        SPDLOG_LOGGER_INFO(detail::console_sink_logger(),"[HISTO]   {} = {:.2f}{}", name, value, format_tags(tags));
    }

    /// @brief Flush all buffered log messages to the underlying sink.
    void flush() noexcept {
        spdlog::default_logger()->flush();
    }

private:
    /// Format tags as " {key1=val1, key2=val2}" or " {}" if empty.
    static std::string format_tags(std::span<const core::MetricTag> tags) {
        if (tags.empty()) return " {}";
        std::string result = " {";
        for (size_t i = 0; i < tags.size(); ++i) {
            if (i > 0) result += ", ";
            result += std::format("{}={}", tags[i].key, tags[i].value);
        }
        result += '}';
        return result;
    }
};

static_assert(core::MetricsSink<ConsoleSink>, "ConsoleSink must satisfy MetricsSink");

} // namespace eph::utils
