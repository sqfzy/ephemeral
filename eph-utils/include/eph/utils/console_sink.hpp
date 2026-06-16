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

#include "eph/core/log.hpp"

#include "eph/core/metrics_concept.hpp"

namespace eph::utils {

namespace detail {
/// @brief Lazily-initialized logger for the console metrics sink.
inline spdlog::logger* console_sink_logger() {
    static spdlog::logger* l = ::eph::log::get("utils.console_sink");
    return l;
}

/// @brief Format `tags` as `" {k1=v1, k2=v2}"` (or `" {}"` if empty).
///
/// Values containing `,` / `=` / `{` / `}` / `"` / `\` are quoted; embedded
/// `"` and `\` inside a quoted value are backslash-escaped so the closing
/// quote isn't matched prematurely by downstream log parsers (e.g. structured
/// log shippers keyed on quoted regions).
///
/// Lifted out of `ConsoleSink` so the formatting contract can be unit-tested
/// without parsing spdlog output. `ConsoleSink::format_tags` is just a thin
/// forwarder.
[[nodiscard]] inline std::string format_metric_tags(
    std::span<const core::MetricTag> tags) {
    if (tags.empty()) return " {}";
    std::string result = " {";
    for (size_t i = 0; i < tags.size(); ++i) {
        if (i > 0) result += ", ";
        const auto& val = tags[i].value;
        const bool needs_quoting =
            val.find_first_of(",={}\"\\") != std::string_view::npos;
        if (needs_quoting) {
            result += tags[i].key;
            result += "=\"";
            // Backslash-escape `\` and `"` inside the value so the
            // closing quote isn't matched prematurely.
            for (char c : val) {
                if (c == '\\' || c == '"') result += '\\';
                result += c;
            }
            result += '"';
        } else {
            result += std::format("{}={}", tags[i].key, val);
        }
    }
    result += '}';
    return result;
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
        EPH_LOG_INFO(detail::console_sink_logger(),"[COUNTER] {} = {}{}", name, value, format_tags(tags));
    }

    /// @brief Log a floating-point gauge metric.
    /// @param name  Metric name (e.g., "rx_queue_depth").
    /// @param value Current gauge value.
    /// @param tags  Optional key-value tags for dimensional grouping.
    void push_gauge(std::string_view name, double value,
                    std::span<const core::MetricTag> tags = {}) noexcept {
        EPH_LOG_INFO(detail::console_sink_logger(),"[GAUGE]   {} = {:.2f}{}", name, value, format_tags(tags));
    }

    /// @brief Log a histogram observation.
    /// @param name  Metric name (e.g., "rx_latency_ns").
    /// @param value Observed value to record.
    /// @param tags  Optional key-value tags for dimensional grouping.
    void push_histogram(std::string_view name, double value,
                        std::span<const core::MetricTag> tags = {}) noexcept {
        EPH_LOG_INFO(detail::console_sink_logger(),"[HISTO]   {} = {:.2f}{}", name, value, format_tags(tags));
    }

    /// @brief Flush all buffered log messages to the underlying sink.
    void flush() noexcept {
        detail::console_sink_logger()->flush();
    }

private:
    /// Thin forwarder to `detail::format_metric_tags` — the formatting logic
    /// lives in detail/ so it can be unit-tested without parsing spdlog
    /// output. See `test_console_sink.cpp::FormatTags*`.
    static std::string format_tags(std::span<const core::MetricTag> tags) {
        return detail::format_metric_tags(tags);
    }
};

static_assert(core::MetricsSink<ConsoleSink>, "ConsoleSink must satisfy MetricsSink");

} // namespace eph::utils
