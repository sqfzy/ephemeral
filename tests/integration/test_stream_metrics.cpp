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

#include "eph/codec/raw_datagram_codec.hpp"
#include "eph/codec/raw_stream_codec.hpp"
#include "eph/core/metrics_concept.hpp"
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/net/kernel/udp_socket.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/net/stream_metrics.hpp"

namespace ek = eph::net::kernel;
namespace en = eph::net;
namespace ec = eph::codec;
namespace ecore = eph::core;

using PlainTcpStream = ek::KernelTcpStream<ec::RawStreamCodec, /*EnableTls=*/false>;
using PlainUdp       = ek::KernelUdpSocket<ec::RawDatagramCodec>;

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

// ─── KernelUdpSocket end-to-end ───────────────────────────────────────────

TEST(StreamMetrics, KernelUdpSocketRecordsBytesSentAndRecv) {
    // Bind two ephemeral UDP sockets on loopback; A sends to B, drive
    // poller, assert B's counters reflect the round-trip.
    auto poller = ek::KernelPoller::create().value();

    ek::UdpConfig cfg_a{};
    cfg_a.bind = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, 0};
    auto a = PlainUdp::create(cfg_a).value();

    ek::UdpConfig cfg_b{};
    cfg_b.bind = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, 0};
    auto b = PlainUdp::create(cfg_b).value();

    std::vector<uint8_t> rx;
    b->on_datagram = [&](std::span<const uint8_t> app_datagram,
                          const en::SocketAddr&) {
        rx.insert(rx.end(), app_datagram.begin(), app_datagram.end());
    };

    ASSERT_TRUE(poller->add(a.get()).has_value());
    ASSERT_TRUE(poller->add(b.get()).has_value());

    // We need B's bound port to send to. Pick a small payload.
    sockaddr_in sa{};
    socklen_t sl = sizeof(sa);
    ::getsockname(b->fd(), reinterpret_cast<sockaddr*>(&sa), &sl);
    en::SocketAddr b_addr{en::Ipv4Addr{127, 0, 0, 1}, ntohs(sa.sin_port)};

    const uint8_t payload[] = {'u', 'd', 'p'};
    ASSERT_TRUE(a->send_to(payload, b_addr).has_value());

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (rx.empty()
           && std::chrono::steady_clock::now() < deadline) {
        poller->poll(std::chrono::milliseconds{50});
    }
    ASSERT_EQ(rx.size(), sizeof(payload));

    EXPECT_GE(a->metric(en::StreamMetric::kBytesSent), sizeof(payload));
    EXPECT_GE(b->metric(en::StreamMetric::kBytesRecv), sizeof(payload));
    EXPECT_GE(b->metric(en::StreamMetric::kFramesDecoded), 1u);

    // UDP backend N/A entries stay at 0:
    EXPECT_EQ(a->metric(en::StreamMetric::kReasmOverflows), 0u);
    EXPECT_EQ(b->metric(en::StreamMetric::kReasmOverflows), 0u);
    EXPECT_EQ(a->metric(en::StreamMetric::kTlsCrossRecordFrames), 0u);
    EXPECT_EQ(b->metric(en::StreamMetric::kTlsCrossRecordFrames), 0u);
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

// ─── Phase F new metrics: per-backend shape + zero defaults ──────────────

TEST(StreamMetrics, NewTcpIcmpMetricsPresentInNameTable) {
    // Enum additions must be reflected in the name table; the header's
    // static_assert enforces this, but a runtime check catches accidental
    // re-ordering during back-port.
    ASSERT_EQ(static_cast<std::size_t>(en::StreamMetric::kCount),
              en::kStreamMetricNames.size());
    const auto names_begin = en::kStreamMetricNames.begin();
    const auto names_end   = en::kStreamMetricNames.end();
    for (auto n : {
            "net.stream.tcp.resets_received",
            "net.stream.tcp.out_of_order_segments",
            "net.stream.tcp.reorder_buffer_hits",
            "net.stream.tcp.reorder_buffer_overflows",
            "net.stream.tcp.keepalive_probes_sent",
            "net.stream.tcp.keepalive_send_failures",
            "net.stream.tcp.mss_negotiation_applied",
            "net.stream.icmp.frag_needed_received",
        }) {
        EXPECT_TRUE(std::find(names_begin, names_end, std::string_view{n})
                    != names_end)
            << "missing metric name: " << n;
    }
}

TEST(StreamMetrics, KernelTcpReturnsZeroForNewTcpIcmpMetrics) {
    // Kernel backend does not implement MSS-negotiation / ICMP /
    // reorder-buffer telemetry. Its metric() must return 0 for the new
    // enum entries so `publish_metrics` does not emit bogus counters.
    EchoServer srv;
    ek::StreamConfig cfg{};
    cfg.remote = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, srv.port};
    auto stream = PlainTcpStream::create(cfg).value();

    for (auto m : {
            en::StreamMetric::kTcpResetsReceived,
            en::StreamMetric::kTcpOutOfOrderSegments,
            en::StreamMetric::kTcpReorderBufferHits,
            en::StreamMetric::kTcpReorderBufferOverflows,
            en::StreamMetric::kTcpKeepaliveProbesSent,
            en::StreamMetric::kTcpKeepaliveSendFailures,
            en::StreamMetric::kTcpMssNegotiationApplied,
            en::StreamMetric::kIcmpFragNeededReceived,
        }) {
        EXPECT_EQ(stream->metric(m), 0u);
    }

    // publish_metrics still emits one row per enum entry — count matches.
    RecordingSink sink;
    en::publish_metrics(*stream, sink);
    EXPECT_EQ(sink.records.size(),
              static_cast<std::size_t>(en::StreamMetric::kCount));
}

TEST(StreamMetrics, KernelUdpReturnsZeroForNewTcpIcmpMetrics) {
    auto sock = PlainUdp::create(
        ek::UdpConfig{.bind = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, 0}})
        .value();
    for (auto m : {
            en::StreamMetric::kTcpResetsReceived,
            en::StreamMetric::kTcpKeepaliveProbesSent,
            en::StreamMetric::kIcmpFragNeededReceived,
        }) {
        EXPECT_EQ(sock->metric(m), 0u);
    }
}

// ─── Phase 5: metric() bounds check — safety for out-of-range enums ──────

TEST(StreamMetrics, MetricAcceptsEveryValidStreamMetricWithoutCrash) {
    EchoServer srv;
    ek::StreamConfig cfg{};
    cfg.remote = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, srv.port};
    auto stream = PlainTcpStream::create(cfg).value();
    // Every enumerator in [0, kCount) must be readable. If the backend's
    // metric() falls off the end of its counter array, ASan would catch
    // it here; without ASan the test just passes silently, which is
    // still the point — no crash.
    for (std::size_t i = 0;
         i < static_cast<std::size_t>(en::StreamMetric::kCount); ++i) {
        const auto m = static_cast<en::StreamMetric>(i);
        const auto v = stream->metric(m);
        (void)v;
    }
    SUCCEED();
}

// ─── T2.11: derived ws.deflate_ratio gauge ────────────────────────────────

namespace {

/// @brief Minimal duck-typed Stream stand-in for the
/// `publish_ws_deflate_ratio` helper — we don't need a real Stream because
/// the helper only exercises `metric()`. Keeps the test independent of any
/// backend's send/recv plumbing and avoids the cost of spinning up a TCP
/// loopback for a pure publisher-side function.
struct MockStreamForRatio {
    std::uint64_t bytes_in{0};
    std::uint64_t bytes_out{0};

    [[nodiscard]] std::uint64_t
    metric(en::StreamMetric m) const noexcept {
        switch (m) {
            case en::StreamMetric::kWsDeflateBytesIn:  return bytes_in;
            case en::StreamMetric::kWsDeflateBytesOut: return bytes_out;
            default: return 0;
        }
    }
};

} // namespace

TEST(StreamMetricsWsDeflateRatio, ComputesRatioFromCounters) {
    // 6.66x compression — a typical ratio on JSON market data
    // (bytes_out is the *plaintext* output, so smaller bytes_out vs
    // bytes_in means MORE compression on the wire).
    MockStreamForRatio s{.bytes_in = 100, .bytes_out = 666};
    RecordingSink sink;
    en::publish_ws_deflate_ratio(s, sink);

    ASSERT_EQ(sink.records.size(), 1u);
    EXPECT_EQ(sink.records[0].kind, RecordingSink::Record::Kind::Gauge);
    EXPECT_EQ(sink.records[0].name, "net.stream.ws.deflate_ratio");
    EXPECT_DOUBLE_EQ(sink.records[0].scalar_value, 6.66);
}

TEST(StreamMetricsWsDeflateRatio, ZeroBytesInEmitsZeroNotNan) {
    // Divide-by-zero guard: most TSDBs treat NaN samples as missing-data
    // markers and would alert spuriously on a freshly-created stream.
    // The helper deliberately emits 0.0 instead.
    MockStreamForRatio s{.bytes_in = 0, .bytes_out = 0};
    RecordingSink sink;
    en::publish_ws_deflate_ratio(s, sink);

    ASSERT_EQ(sink.records.size(), 1u);
    EXPECT_EQ(sink.records[0].kind, RecordingSink::Record::Kind::Gauge);
    EXPECT_DOUBLE_EQ(sink.records[0].scalar_value, 0.0);
}

TEST(StreamMetricsWsDeflateRatio, ZeroBytesInWithNonZeroOutEmitsZero) {
    // Pathological: bytes_out got bumped before bytes_in (atomic snapshot
    // race window). Helper still must not divide by zero.
    MockStreamForRatio s{.bytes_in = 0, .bytes_out = 999};
    RecordingSink sink;
    en::publish_ws_deflate_ratio(s, sink);

    ASSERT_EQ(sink.records.size(), 1u);
    EXPECT_DOUBLE_EQ(sink.records[0].scalar_value, 0.0);
}

TEST(StreamMetricsWsDeflateRatio, RatioForwardsTags) {
    // Tags are forwarded verbatim — this matches the existing publisher
    // helpers and lets the caller annotate per-stream context (venue,
    // symbol, etc).
    MockStreamForRatio s{.bytes_in = 200, .bytes_out = 50};
    RecordingSink sink;
    std::array tags{ecore::MetricTag{"venue", "binance"},
                    ecore::MetricTag{"symbol", "BTCUSDT"}};
    en::publish_ws_deflate_ratio(s, sink, tags);
    ASSERT_EQ(sink.records.size(), 1u);
    // 50/200 = 0.25 (4x compression).
    EXPECT_DOUBLE_EQ(sink.records[0].scalar_value, 0.25);
}

TEST(StreamMetricsWsDeflateRatio, MetricNameConstantMatchesDocumented) {
    // The task spec calls for the stable name `net.stream.ws.deflate_ratio`
    // so dashboards can hard-code it. Lock that in here.
    EXPECT_EQ(en::kWsDeflateRatioMetricName, "net.stream.ws.deflate_ratio");
}

TEST(StreamMetricsWsDeflateRatio, BytesOutGreaterThanBytesInEmitsRatioAboveOne) {
    // Pathological-but-legal case explicitly called out in the helper's
    // doc comment ("legal in pathological cases (very small payloads where
    // deflate overhead exceeds savings) and useful as a signal that the
    // upstream's compression strategy is degrading"). The helper must NOT
    // clamp the ratio at 1.0 — dashboards rely on the verbatim quotient
    // to detect a degraded compression regime. 150 / 100 = 1.5.
    MockStreamForRatio s{.bytes_in = 100, .bytes_out = 150};
    RecordingSink sink;
    en::publish_ws_deflate_ratio(s, sink);

    ASSERT_EQ(sink.records.size(), 1u);
    EXPECT_EQ(sink.records[0].kind, RecordingSink::Record::Kind::Gauge);
    EXPECT_DOUBLE_EQ(sink.records[0].scalar_value, 1.5);
}

TEST(StreamMetricsWsDeflateRatio, EqualBytesEmitsRatioOfOne) {
    // Boundary: bytes_out == bytes_in (deflate produced the same byte
    // count as the wire payload). Must emit exactly 1.0 — not zero
    // (no special-case break-even handling) and not subject to FP drift.
    MockStreamForRatio s{.bytes_in = 1024, .bytes_out = 1024};
    RecordingSink sink;
    en::publish_ws_deflate_ratio(s, sink);

    ASSERT_EQ(sink.records.size(), 1u);
    EXPECT_DOUBLE_EQ(sink.records[0].scalar_value, 1.0);
}

TEST(StreamMetrics, MetricReturnsZeroForOutOfRangeEnumValue) {
    // Simulate ABI drift: a caller passing an enumerator value that is
    // past the current `kCount`. The bounds check in each backend's
    // metric() must return 0 rather than index past counters_.
    EchoServer srv;
    ek::StreamConfig cfg{};
    cfg.remote = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, srv.port};
    auto stream = PlainTcpStream::create(cfg).value();

    auto udp = PlainUdp::create(
        ek::UdpConfig{.bind = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, 0}})
        .value();

    for (auto drifted : {
            static_cast<en::StreamMetric>(
                static_cast<std::size_t>(en::StreamMetric::kCount)),
            static_cast<en::StreamMetric>(
                static_cast<std::size_t>(en::StreamMetric::kCount) + 1),
            static_cast<en::StreamMetric>(
                static_cast<std::size_t>(en::StreamMetric::kCount) + 42),
        }) {
        EXPECT_EQ(stream->metric(drifted), 0u);
        EXPECT_EQ(udp->metric(drifted), 0u);
    }
}
