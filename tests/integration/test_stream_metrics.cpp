/// @file test_stream_metrics.cpp
/// Integration tests for the stream observability layer.
///
/// Verifies that:
///   - `eph::net::publish_metrics()` emits one counter per StreamMetric
///     entry (identity check via name table)
///   - Each backend's hot-path `inc_<M>()` calls actually fire when the
///     corresponding event happens (drive a real loopback / fake stream
///     and assert the counter advanced)
///
/// Uses `RecordingSink` (a duck-typed MetricsSink that captures every
/// push for later assertion) to keep tests independent of any concrete
/// sink implementation.

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "eph/codec/raw_stream_codec.hpp"
#include "eph/core/metrics_concept.hpp"
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/net/stream_metrics.hpp"

namespace ek = eph::net::kernel;
namespace en = eph::net;
namespace ec = eph::codec;
namespace ecore = eph::core;

using PlainTcpStream = ek::KernelTcpStream<ec::RawStreamCodec, /*EnableTls=*/false>;

// ─── RecordingSink — captures every push for assertion ────────────────────

namespace {

struct RecordingSink {
    struct Record {
        std::string             name;
        std::int64_t            counter_value{0};
        double                  scalar_value{0.0};
        enum class Kind { Counter, Gauge, Histogram } kind{Kind::Counter};
    };

    std::vector<Record> records;

    void push_counter(std::string_view name, std::int64_t value,
                      std::span<const ecore::MetricTag> = {}) noexcept {
        records.push_back({std::string(name), value, 0.0,
                           Record::Kind::Counter});
    }
    void push_gauge(std::string_view name, double value,
                    std::span<const ecore::MetricTag> = {}) noexcept {
        records.push_back({std::string(name), 0, value,
                           Record::Kind::Gauge});
    }
    void push_histogram(std::string_view name, double value,
                        std::span<const ecore::MetricTag> = {}) noexcept {
        records.push_back({std::string(name), 0, value,
                           Record::Kind::Histogram});
    }
    void flush() noexcept {}

    /// @brief Find the captured record for a metric name. Returns nullptr
    ///        when the name was never published.
    const Record* find(std::string_view name) const noexcept {
        for (const auto& r : records) {
            if (r.name == name) return &r;
        }
        return nullptr;
    }
};

static_assert(ecore::MetricsSink<RecordingSink>,
              "RecordingSink must satisfy MetricsSink");

// ─── Loopback echo helper (single client, per-test thread) ─────────────────

struct EchoServer {
    int                  listen_fd{-1};
    uint16_t             port{0};
    std::thread          th;
    std::atomic<bool>    stop{false};

    EchoServer() {
        listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        int yes = 1;
        ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in sa{};
        sa.sin_family      = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port        = 0;
        ::bind(listen_fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
        socklen_t len = sizeof(sa);
        ::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&sa), &len);
        port = ntohs(sa.sin_port);
        ::listen(listen_fd, 1);
        th = std::thread([this] { run(); });
    }

    void run() {
        sockaddr_in peer{};
        socklen_t pl = sizeof(peer);
        int cfd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &pl);
        if (cfd < 0) return;
        while (!stop.load(std::memory_order_relaxed)) {
            char buf[1024];
            ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            (void)::send(cfd, buf, n, MSG_NOSIGNAL);
        }
        ::close(cfd);
    }

    ~EchoServer() {
        stop.store(true, std::memory_order_relaxed);
        if (listen_fd >= 0) ::close(listen_fd);
        if (th.joinable()) th.join();
    }
};

} // namespace

// ─── Plumbing tests ───────────────────────────────────────────────────────

TEST(StreamMetrics, EnumNameTableIsConsistent) {
    // The static_assert in the header enforces this at compile time;
    // duplicate the check at runtime so a regression here surfaces with
    // a clear test failure rather than only a build error.
    EXPECT_EQ(en::kStreamMetricNames.size(),
              static_cast<std::size_t>(en::StreamMetric::kCount));
    for (auto sv : en::kStreamMetricNames) {
        EXPECT_FALSE(sv.empty());
        // OTel-style hierarchical naming convention.
        EXPECT_NE(sv.find("net.stream"), std::string_view::npos)
            << "metric name '" << sv << "' should start with net.stream.*";
    }
}

TEST(StreamMetrics, PublishMetricsEmitsOneCounterPerEntry) {
    // Use a freshly-created (but unconnected) stream — counters all 0.
    EchoServer srv;
    ek::StreamConfig cfg{};
    cfg.remote = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, srv.port};
    auto stream = PlainTcpStream::create(cfg).value();

    RecordingSink sink;
    en::publish_metrics(*stream, sink);

    // One push_counter per StreamMetric entry, all values 0 (no I/O yet).
    ASSERT_EQ(sink.records.size(),
              static_cast<std::size_t>(en::StreamMetric::kCount));
    for (const auto& r : sink.records) {
        EXPECT_EQ(r.kind, RecordingSink::Record::Kind::Counter);
        EXPECT_EQ(r.counter_value, 0);
    }
    // Spot-check name presence.
    EXPECT_NE(sink.find("net.stream.bytes_sent"), nullptr);
    EXPECT_NE(sink.find("net.stream.bytes_recv"), nullptr);
    EXPECT_NE(sink.find("net.stream.frames_decoded"), nullptr);
    EXPECT_NE(sink.find("net.stream.reasm_overflows"), nullptr);
    EXPECT_NE(sink.find("net.stream.codec_errors"), nullptr);
    EXPECT_NE(sink.find("net.stream.tls.cross_record_frames"), nullptr);
}

// ─── KernelTcpStream end-to-end metric assertion ─────────────────────────

TEST(StreamMetrics, KernelTcpStreamRecordsBytesSentAndRecv) {
    EchoServer srv;

    auto poller = ek::KernelPoller::create().value();

    ek::StreamConfig cfg{};
    cfg.remote = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, srv.port};
    auto stream = PlainTcpStream::create(cfg).value();

    std::vector<uint8_t> captured;
    stream->on_message = [&](std::span<const uint8_t> app_frame) {
        captured.insert(captured.end(), app_frame.begin(), app_frame.end());
    };

    ASSERT_TRUE(poller->add(stream.get()).has_value());

    const uint8_t payload[] = {'p', 'i', 'n', 'g'};
    ASSERT_TRUE(stream->send(payload).has_value());

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (captured.size() < sizeof(payload)
           && std::chrono::steady_clock::now() < deadline) {
        poller->poll(std::chrono::milliseconds{50});
    }
    ASSERT_EQ(captured.size(), sizeof(payload));

    // ── Direct-read assertions ──
    EXPECT_GE(stream->metric(en::StreamMetric::kBytesSent), sizeof(payload));
    EXPECT_GE(stream->metric(en::StreamMetric::kBytesRecv), sizeof(payload));
    EXPECT_GE(stream->metric(en::StreamMetric::kFramesDecoded), 1u);
    EXPECT_EQ(stream->metric(en::StreamMetric::kReasmOverflows), 0u);
    EXPECT_EQ(stream->metric(en::StreamMetric::kCodecErrors), 0u);
    EXPECT_EQ(stream->metric(en::StreamMetric::kTlsCrossRecordFrames), 0u);

    // ── Sink-bridge round-trip ──
    RecordingSink sink;
    std::array tags{ecore::MetricTag{"backend", "kernel"},
                    ecore::MetricTag{"transport", "tcp"}};
    en::publish_metrics(*stream, sink, tags);

    ASSERT_NE(sink.find("net.stream.bytes_sent"), nullptr);
    EXPECT_GE(sink.find("net.stream.bytes_sent")->counter_value,
              static_cast<int64_t>(sizeof(payload)));
    EXPECT_GE(sink.find("net.stream.bytes_recv")->counter_value,
              static_cast<int64_t>(sizeof(payload)));
    EXPECT_GE(sink.find("net.stream.frames_decoded")->counter_value, 1);
}

TEST(StreamMetrics, MetricCountersAreMonotonic) {
    EchoServer srv;
    auto poller = ek::KernelPoller::create().value();

    ek::StreamConfig cfg{};
    cfg.remote = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, srv.port};
    auto stream = PlainTcpStream::create(cfg).value();

    bool got = false;
    stream->on_message = [&](std::span<const uint8_t>) { got = true; };
    ASSERT_TRUE(poller->add(stream.get()).has_value());

    auto drain = [&] {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds{2};
        got = false;
        while (!got && std::chrono::steady_clock::now() < deadline) {
            poller->poll(std::chrono::milliseconds{50});
        }
    };

    const uint8_t payload[] = {'a'};
    ASSERT_TRUE(stream->send(payload).has_value());
    drain();
    const auto sent_after_first = stream->metric(en::StreamMetric::kBytesSent);
    const auto recv_after_first = stream->metric(en::StreamMetric::kBytesRecv);
    EXPECT_GE(sent_after_first, 1u);
    EXPECT_GE(recv_after_first, 1u);

    ASSERT_TRUE(stream->send(payload).has_value());
    drain();
    EXPECT_GE(stream->metric(en::StreamMetric::kBytesSent), sent_after_first);
    EXPECT_GE(stream->metric(en::StreamMetric::kBytesRecv), recv_after_first);
}
