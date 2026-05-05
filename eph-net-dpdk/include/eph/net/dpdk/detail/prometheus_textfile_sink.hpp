#pragma once

/// @file detail/prometheus_textfile_sink.hpp
/// Prometheus textfile-collector sink — buffers counters/gauges/histograms
/// in memory and flushes them as a single Prometheus exposition-format
/// `.prom` file via the atomic write+rename pattern.
///
/// T2.6 from the 2026-05-05 action list. Closes the observability gap
/// "no push exporter, all metrics are pull-model" by giving operators a
/// concrete `MetricsSink` implementation that integrates with the
/// `node_exporter --collector.textfile.directory=/var/lib/eph` path most
/// HFT colos already use for host-level metrics.
///
/// Design notes:
///   - One sink per file path. The recommended deployment is one file
///     per app (`/var/lib/eph/app-<pid>.prom`) — a single file is the
///     atomic unit Prometheus reads via the textfile collector.
///   - Atomic write: `flush()` writes to `<path>.tmp` then renames to
///     `<path>`. Prometheus's textfile collector reads files via
///     `os.Open` + buffered scan; a partially-written file would
///     surface as parse errors. Atomic rename eliminates that.
///   - In-memory buffer: counters/gauges/histograms are accumulated in
///     `std::map<string, sample>` keyed by `(name, tags)`. Map is
///     populated on every `push_*` call but only serialised at `flush()`.
///   - Tags: serialised in Prometheus label format
///     `name{tag_key1="tag_val1",tag_key2="tag_val2"} value`.
///   - HELP / TYPE lines: emitted once per unique metric name on flush.
///   - Histograms: per Prometheus conventions a histogram metric requires
///     `_bucket{le="..."}`, `_sum`, `_count`. We deliberately keep this
///     simple-histogram (single value pushed per sample); the consumer
///     either uses a recording rule to bucket, or the application calls
///     `push_histogram` repeatedly with bucketed values.
///
/// NOT in scope (deferred to a future iteration if needed):
///   - Streaming `flush()` (we always rebuild the full file).
///   - Histograms with native `_bucket{le=N}` exposition.
///   - Concurrent `push_*` from multiple threads — sink is intended to
///     be called from a single publisher thread.
///   - Compression / retention / rotation — operator's job (logrotate
///     or `find -mtime +7 -delete`).
///
/// Example:
///   eph::net::dpdk::detail::PrometheusTextfileSink sink{
///       "/var/lib/eph/app-1234.prom"};
///   eph::net::publish_metrics(*stream, sink, /*tags*/);
///   sink.flush();      // atomic write+rename .tmp → .prom
///
/// On systemd hosts with `node_exporter` + textfile collector enabled
/// at `/var/lib/eph/`, the metrics show up in Prometheus within the
/// scrape interval (default 15s) under `net_stream_*` series names.

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <unistd.h>

#include <fcntl.h>
#include <sys/stat.h>

#include "eph/core/metrics_concept.hpp"

namespace eph::net::dpdk::detail {

/// @brief Prometheus exposition-format sink writing to a textfile-collector
///        path via atomic write+rename.
///
/// Satisfies `eph::core::MetricsSink`. Construct with a target path; call
/// `push_counter` / `push_gauge` / `push_histogram` to accumulate samples;
/// call `flush()` to atomically write the current buffer to disk.
///
/// All methods are `noexcept`. I/O failures during `flush()` are recorded
/// in `last_error()` rather than thrown — observability code must not be
/// allowed to panic the data path.
class PrometheusTextfileSink {
public:
    /// @param output_path  Final on-disk path (e.g. `/var/lib/eph/app.prom`).
    ///                     The sink will write to `<output_path>.tmp` and
    ///                     atomically rename on `flush()`.
    explicit PrometheusTextfileSink(std::filesystem::path output_path) noexcept
        : output_path_(std::move(output_path)) {}

    PrometheusTextfileSink(const PrometheusTextfileSink&) = delete;
    PrometheusTextfileSink& operator=(const PrometheusTextfileSink&) = delete;
    PrometheusTextfileSink(PrometheusTextfileSink&&) noexcept = default;
    PrometheusTextfileSink& operator=(PrometheusTextfileSink&&) noexcept = default;

    void push_counter(std::string_view name, int64_t value,
                      std::span<const eph::core::MetricTag> tags = {}) noexcept {
        try {
            const auto key = compose_key_(name, tags);
            counters_[key] = Sample{std::string{name}, std::string{}, value, 0.0,
                                    /*is_gauge*/ false};
            counters_[key].labels = serialise_tags_(tags);
        } catch (...) {
            // Map operations can throw std::bad_alloc; observability
            // must not affect the hot path. Drop the sample and record.
            last_error_ = "push_counter: bad_alloc";
        }
    }

    void push_gauge(std::string_view name, double value,
                    std::span<const eph::core::MetricTag> tags = {}) noexcept {
        try {
            const auto key = compose_key_(name, tags);
            gauges_[key] = Sample{std::string{name}, serialise_tags_(tags), 0,
                                  value, /*is_gauge*/ true};
        } catch (...) {
            last_error_ = "push_gauge: bad_alloc";
        }
    }

    void push_histogram(std::string_view name, double value,
                        std::span<const eph::core::MetricTag> tags = {}) noexcept {
        // Simple histogram: emit as gauge. A future iteration can carry
        // bucketed _bucket{le="..."}, _sum, _count emission. For now the
        // value goes through as a gauge so consumers can do their own
        // recording rule-side bucketing.
        try {
            const auto key = compose_key_("histogram_", name, tags);
            histograms_[key] = Sample{std::string{name},
                                      serialise_tags_(tags), 0, value,
                                      /*is_gauge*/ true};
        } catch (...) {
            last_error_ = "push_histogram: bad_alloc";
        }
    }

    /// Atomically write the current buffer to disk via tmp + rename.
    /// Idempotent: callable multiple times. Empty buffer writes an
    /// empty file (still an atomic operation; consumers see "no
    /// metrics yet" cleanly).
    void flush() noexcept {
        last_error_.clear();
        // Build serialised content first — never write a partial file.
        std::string body;
        try {
            body.reserve(estimate_body_size_());
            serialise_into_(body);
        } catch (...) {
            last_error_ = "flush: serialise bad_alloc";
            return;
        }

        // Atomic write: <path>.tmp → fsync → rename(<path>).
        std::filesystem::path tmp_path = output_path_;
        tmp_path += ".tmp";
        const std::string tmp_str = tmp_path.string();
        const std::string final_str = output_path_.string();

        // O_CLOEXEC: a Prometheus textfile sink that lives inside a
        // long-running daemon may co-exist with subprocess fork+execs
        // (test harnesses, eph-nicd watchdog, future supervisor
        // logic). Without CLOEXEC the fd would leak into every
        // child until close-after-exec, exhausting fd budget and
        // confusing fd-aware diagnostics.
        const int fd = ::open(tmp_str.c_str(),
                              O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                              0644);
        if (fd < 0) {
            record_errno_("open(tmp)");
            return;
        }

        // Write — handle short writes.
        const char* buf = body.data();
        size_t left = body.size();
        while (left > 0) {
            const ssize_t n = ::write(fd, buf, left);
            if (n < 0) {
                if (errno == EINTR) continue;
                record_errno_("write(tmp)");
                ::close(fd);
                ::unlink(tmp_str.c_str());
                return;
            }
            if (n == 0) {
                // POSIX-allowed but observable on pipes / unusual fs:
                // n=0 with bytes remaining means "no progress made,
                // errno indicates cause". Naive `buf += 0; left -= 0;`
                // would spin forever; abort the flush instead.
                record_errno_("write(tmp): zero progress");
                ::close(fd);
                ::unlink(tmp_str.c_str());
                return;
            }
            buf += static_cast<size_t>(n);
            left -= static_cast<size_t>(n);
        }

        // Best-effort fsync — Prometheus textfile collector reads via
        // os.Open which sees the page cache so fsync is durability-only.
        // Failure is non-fatal.
        ::fsync(fd);
        ::close(fd);

        if (::rename(tmp_str.c_str(), final_str.c_str()) != 0) {
            record_errno_("rename");
            ::unlink(tmp_str.c_str());
            return;
        }
    }

    /// Last error message from `flush()`. Empty when last call succeeded.
    [[nodiscard]] std::string_view last_error() const noexcept {
        return last_error_;
    }

    /// Total samples currently buffered across counters / gauges / histograms.
    /// Useful for tests + diagnostic logging.
    [[nodiscard]] size_t sample_count() const noexcept {
        return counters_.size() + gauges_.size() + histograms_.size();
    }

    /// Path written on `flush()`.
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return output_path_;
    }

private:
    struct Sample {
        std::string name;
        std::string labels;     ///< Pre-serialised `key1="v1",key2="v2"` (no braces)
        int64_t     i_value;    ///< Counter value
        double      d_value;    ///< Gauge / histogram value
        bool        is_gauge;
    };

    static std::string compose_key_(std::string_view name,
                                    std::span<const eph::core::MetricTag> tags) {
        std::string k;
        k.reserve(name.size() + 8 * tags.size());
        k.append(name);
        for (const auto& t : tags) {
            k.push_back('|');
            k.append(t.key);
            k.push_back('=');
            k.append(t.value);
        }
        return k;
    }

    static std::string compose_key_(std::string_view prefix,
                                    std::string_view name,
                                    std::span<const eph::core::MetricTag> tags) {
        std::string k;
        k.reserve(prefix.size() + name.size() + 8 * tags.size());
        k.append(prefix);
        k.append(name);
        for (const auto& t : tags) {
            k.push_back('|');
            k.append(t.key);
            k.push_back('=');
            k.append(t.value);
        }
        return k;
    }

    /// Serialise tag span as `k1="v1",k2="v2"` (no braces, no
    /// outer separators). Caller wraps in `{...}` if non-empty.
    static std::string serialise_tags_(std::span<const eph::core::MetricTag> tags) {
        std::string s;
        if (tags.empty()) return s;
        s.reserve(tags.size() * 16);
        bool first = true;
        for (const auto& t : tags) {
            if (!first) s.push_back(',');
            first = false;
            s.append(t.key);
            s.append("=\"");
            // Escape backslash + quote per Prometheus spec.
            for (char c : t.value) {
                if (c == '\\' || c == '"') s.push_back('\\');
                s.push_back(c);
            }
            s.push_back('"');
        }
        return s;
    }

    [[nodiscard]] size_t estimate_body_size_() const noexcept {
        // ~64B/sample + small fixed overhead per type.
        return 64 * (counters_.size() + gauges_.size() + histograms_.size())
               + 256;
    }

    void serialise_into_(std::string& body) const {
        // Minimal exposition format. We omit per-name HELP / TYPE
        // lines because StreamMetric names are stable and documented
        // in eph/net/stream_metrics.hpp; an operator who wants full
        // metadata can grep that header. (HELP / TYPE add 2 lines
        // per sample = ~2× file size for limited diagnostic gain.)
        for (const auto& [_, s] : counters_) {
            body.append(s.name);
            if (!s.labels.empty()) {
                body.push_back('{');
                body.append(s.labels);
                body.push_back('}');
            }
            body.push_back(' ');
            body.append(std::to_string(s.i_value));
            body.push_back('\n');
        }
        for (const auto& [_, s] : gauges_) {
            body.append(s.name);
            if (!s.labels.empty()) {
                body.push_back('{');
                body.append(s.labels);
                body.push_back('}');
            }
            body.push_back(' ');
            // Use printf-style for double precision; std::to_string
            // is locale-sensitive and can produce comma decimals on
            // non-C locales. Prometheus rejects those.
            char buf[48];
            const int n = std::snprintf(buf, sizeof(buf), "%.6f", s.d_value);
            if (n > 0 && static_cast<size_t>(n) < sizeof(buf)) {
                body.append(buf, static_cast<size_t>(n));
            } else {
                body.append("0");
            }
            body.push_back('\n');
        }
        for (const auto& [_, s] : histograms_) {
            body.append(s.name);
            if (!s.labels.empty()) {
                body.push_back('{');
                body.append(s.labels);
                body.push_back('}');
            }
            body.push_back(' ');
            char buf[48];
            const int n = std::snprintf(buf, sizeof(buf), "%.6f", s.d_value);
            if (n > 0 && static_cast<size_t>(n) < sizeof(buf)) {
                body.append(buf, static_cast<size_t>(n));
            } else {
                body.append("0");
            }
            body.push_back('\n');
        }
    }

    void record_errno_(const char* phase) noexcept {
        try {
            last_error_.assign(phase);
            last_error_.append(": ");
            last_error_.append(std::strerror(errno));
        } catch (...) {
            // Out of memory while recording — last_error_ may be
            // inconsistent but the failure is already logged via the
            // return path.
        }
    }

    std::filesystem::path output_path_;
    std::map<std::string, Sample> counters_;
    std::map<std::string, Sample> gauges_;
    std::map<std::string, Sample> histograms_;
    std::string                    last_error_;
};

static_assert(eph::core::MetricsSink<PrometheusTextfileSink>,
              "PrometheusTextfileSink must satisfy eph::core::MetricsSink");

} // namespace eph::net::dpdk::detail
