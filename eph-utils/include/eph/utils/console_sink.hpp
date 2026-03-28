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

#include "eph/core/metrics_concept.hpp"

namespace eph::utils {

/// Metrics sink that logs to spdlog at INFO level.
/// Useful for development, debugging, and integration testing.
/// Not intended for production hot paths — use NullSink or a
/// buffered network sink instead.
class ConsoleSink {
public:
    void push_counter(std::string_view name, int64_t value,
                      std::span<const core::MetricTag> tags = {}) noexcept {
        SPDLOG_INFO("[COUNTER] {} = {}{}", name, value, format_tags(tags));
    }

    void push_gauge(std::string_view name, double value,
                    std::span<const core::MetricTag> tags = {}) noexcept {
        SPDLOG_INFO("[GAUGE]   {} = {:.2f}{}", name, value, format_tags(tags));
    }

    void push_histogram(std::string_view name, double value,
                        std::span<const core::MetricTag> tags = {}) noexcept {
        SPDLOG_INFO("[HISTO]   {} = {:.2f}{}", name, value, format_tags(tags));
    }

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
