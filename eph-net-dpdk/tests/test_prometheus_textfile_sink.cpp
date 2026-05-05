/// @file test_prometheus_textfile_sink.cpp
/// Tests for `eph::net::dpdk::detail::PrometheusTextfileSink` (T2.6).
///
/// Verifies:
///   1. Concept conformance (compile-time): satisfies eph::core::MetricsSink.
///   2. Counter / gauge / histogram push + flush round-trips through atomic
///      write+rename to a temp directory.
///   3. Tag serialisation in Prometheus label format.
///   4. Tag value escaping (backslash + quote).
///   5. Empty buffer flush produces an empty file (well-defined no-content).
///   6. Repeated push of same (name, tags) overwrites — counter is
///      "current snapshot value", not "additive". (Matches OpenTelemetry
///      / Prometheus textfile-collector semantics: each scrape sees the
///      current state.)
///   7. Last-error reporting on write failure (path that cannot be opened).

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "eph/net/dpdk/detail/prometheus_textfile_sink.hpp"

namespace {

namespace ec = eph::core;
namespace ed = eph::net::dpdk::detail;

class TmpDir {
public:
    TmpDir() {
        char tpl[] = "/tmp/eph_prom_test_XXXXXX";
        char* d = ::mkdtemp(tpl);
        if (!d) {
            ADD_FAILURE() << "mkdtemp failed";
            path_ = "/tmp/eph_prom_test_fallback";
        } else {
            path_ = d;
        }
    }
    ~TmpDir() {
        // Best-effort cleanup.
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    [[nodiscard]] std::filesystem::path file(const char* name) const {
        return path_ / name;
    }
private:
    std::filesystem::path path_;
};

[[nodiscard]] std::string slurp(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

TEST(PrometheusTextfileSink, ConceptConformance) {
    static_assert(ec::MetricsSink<ed::PrometheusTextfileSink>,
                  "PrometheusTextfileSink must satisfy MetricsSink");
}

TEST(PrometheusTextfileSink, CounterFlushRoundTrip) {
    TmpDir dir;
    const auto path = dir.file("counters.prom");

    ed::PrometheusTextfileSink sink{path};
    sink.push_counter("net_stream_bytes_sent", 12345);
    sink.push_counter("net_stream_bytes_recv", 67890);
    EXPECT_EQ(sink.sample_count(), 2u);
    sink.flush();
    EXPECT_TRUE(sink.last_error().empty()) << sink.last_error();

    ASSERT_TRUE(std::filesystem::exists(path));
    const auto body = slurp(path);
    EXPECT_NE(body.find("net_stream_bytes_sent 12345"), std::string::npos)
        << body;
    EXPECT_NE(body.find("net_stream_bytes_recv 67890"), std::string::npos)
        << body;
}

TEST(PrometheusTextfileSink, GaugeFlushRoundTrip) {
    TmpDir dir;
    const auto path = dir.file("gauges.prom");

    ed::PrometheusTextfileSink sink{path};
    sink.push_gauge("queue_depth", 42.5);
    sink.flush();
    EXPECT_TRUE(sink.last_error().empty());

    const auto body = slurp(path);
    EXPECT_NE(body.find("queue_depth"), std::string::npos);
    EXPECT_NE(body.find("42.5"), std::string::npos) << body;
}

TEST(PrometheusTextfileSink, TagSerialisation) {
    TmpDir dir;
    const auto path = dir.file("tagged.prom");

    ed::PrometheusTextfileSink sink{path};
    const std::array tags = {
        ec::MetricTag{"transport", "dpdk"},
        ec::MetricTag{"symbol",    "btcusdt"},
    };
    sink.push_counter("net_stream_bytes_sent", 999, tags);
    sink.flush();

    const auto body = slurp(path);
    EXPECT_NE(body.find(R"(net_stream_bytes_sent{transport="dpdk",symbol="btcusdt"} 999)"),
              std::string::npos)
        << body;
}

TEST(PrometheusTextfileSink, TagEscaping) {
    TmpDir dir;
    const auto path = dir.file("escaped.prom");

    ed::PrometheusTextfileSink sink{path};
    const std::array tags = {
        ec::MetricTag{"path",  R"(C:\foo\bar)"},
        ec::MetricTag{"quote", R"(she said "hi")"},
    };
    sink.push_gauge("custom_gauge", 1.0, tags);
    sink.flush();

    const auto body = slurp(path);
    // Backslashes doubled, quotes escaped.
    EXPECT_NE(body.find(R"(path="C:\\foo\\bar")"), std::string::npos)
        << body;
    EXPECT_NE(body.find(R"(quote="she said \"hi\"")"), std::string::npos)
        << body;
}

TEST(PrometheusTextfileSink, EmptyFlushProducesEmptyFile) {
    TmpDir dir;
    const auto path = dir.file("empty.prom");

    ed::PrometheusTextfileSink sink{path};
    sink.flush();
    EXPECT_TRUE(sink.last_error().empty());

    ASSERT_TRUE(std::filesystem::exists(path));
    EXPECT_EQ(std::filesystem::file_size(path), 0u);
}

TEST(PrometheusTextfileSink, RepeatedPushOverwrites) {
    TmpDir dir;
    const auto path = dir.file("repeat.prom");

    ed::PrometheusTextfileSink sink{path};
    sink.push_counter("net_stream_bytes_sent", 100);
    sink.push_counter("net_stream_bytes_sent", 200);
    sink.push_counter("net_stream_bytes_sent", 300);
    EXPECT_EQ(sink.sample_count(), 1u);  // same key
    sink.flush();

    const auto body = slurp(path);
    EXPECT_NE(body.find("net_stream_bytes_sent 300"), std::string::npos);
    EXPECT_EQ(body.find("net_stream_bytes_sent 100"), std::string::npos);
    EXPECT_EQ(body.find("net_stream_bytes_sent 200"), std::string::npos);
}

TEST(PrometheusTextfileSink, FlushReportsErrorOnUnopenable) {
    // Path under a non-existent directory that is also unwritable.
    const std::filesystem::path bogus =
        "/proc/self/this-cannot-be-created/foo.prom";

    ed::PrometheusTextfileSink sink{bogus};
    sink.push_counter("dummy", 1);
    sink.flush();

    // Should record an error rather than throw / abort.
    EXPECT_FALSE(sink.last_error().empty());
}
