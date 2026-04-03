/// @file test_transport_types.cpp
/// Tests for transport types: enums, stats, frame filter, formatters.

#include <gtest/gtest.h>

#include "eph/transport/transport_types.hpp"

using namespace eph::net;

// =======================================================================
// TransportEvent / TransportState name functions
// =======================================================================

TEST(TransportEventName, AllEnumValuesHaveNames) {
    EXPECT_EQ(transport_event_name(TransportEvent::kConnected), "CONNECTED");
    EXPECT_EQ(transport_event_name(TransportEvent::kDisconnected), "DISCONNECTED");
    EXPECT_EQ(transport_event_name(TransportEvent::kReconnecting), "RECONNECTING");
    EXPECT_EQ(transport_event_name(TransportEvent::kStopped), "STOPPED");
}

TEST(TransportStateName, AllEnumValuesHaveNames) {
    EXPECT_EQ(transport_state_name(TransportState::kConnected), "CONNECTED");
    EXPECT_EQ(transport_state_name(TransportState::kReconnecting), "RECONNECTING");
    EXPECT_EQ(transport_state_name(TransportState::kStopped), "STOPPED");
}

TEST(TransportEventAdl, ErrorNameMatchesEventName) {
    EXPECT_EQ(error_name(TransportEvent::kConnected),
              transport_event_name(TransportEvent::kConnected));
    EXPECT_EQ(error_name(TransportState::kStopped),
              transport_state_name(TransportState::kStopped));
}

// =======================================================================
// RttStats
// =======================================================================

TEST(RttStats, ZeroCountDumpReturnsNoSamples) {
    RttStats r{};
    EXPECT_EQ(r.dump(), "RttStats: no samples");
}

TEST(RttStats, ConversionHelpers) {
    RttStats r{};
    r.count = 100;
    r.min_ns = 1000;
    r.max_ns = 10000;
    r.mean_ns = 5000.0;
    r.p50_ns = 4500;
    r.p99_ns = 9000;
    r.p999_ns = 9500;

    EXPECT_DOUBLE_EQ(r.min_us(), 1.0);
    EXPECT_DOUBLE_EQ(r.max_us(), 10.0);
    EXPECT_DOUBLE_EQ(r.mean_us(), 5.0);
    EXPECT_DOUBLE_EQ(r.p50_us(), 4.5);
    EXPECT_DOUBLE_EQ(r.p99_us(), 9.0);
    EXPECT_DOUBLE_EQ(r.p999_us(), 9.5);
}

TEST(RttStats, DeltaOperatorSubtractsCounters) {
    RttStats a{.count = 100, .min_ns = 500, .max_ns = 5000,
               .mean_ns = 2000.0, .p50_ns = 1800, .p99_ns = 4500, .p999_ns = 4900};
    RttStats b{.count = 80, .min_ns = 600, .max_ns = 4000,
               .mean_ns = 1500.0, .p50_ns = 1400, .p99_ns = 3800, .p999_ns = 3900};
    auto delta = a - b;
    // Count is subtracted
    EXPECT_EQ(delta.count, 20);
    // Percentiles come from the later snapshot (a)
    EXPECT_EQ(delta.p50_ns, 1800);
    EXPECT_EQ(delta.p99_ns, 4500);
}

TEST(RttStats, DumpWithSamplesContainsMetrics) {
    RttStats r{.count = 10, .min_ns = 100, .max_ns = 5000,
               .mean_ns = 2500.0, .p50_ns = 2000, .p99_ns = 4500, .p999_ns = 4900};
    auto d = r.dump();
    EXPECT_NE(d.find("10 samples"), std::string::npos);
}

TEST(RttStats, ToJsonContainsFields) {
    RttStats r{.count = 5, .min_ns = 100, .max_ns = 1000,
               .mean_ns = 500.0, .p50_ns = 450, .p99_ns = 900, .p999_ns = 950};
    auto j = r.to_json();
    EXPECT_NE(j.find("\"count\":5"), std::string::npos);
    EXPECT_NE(j.find("\"min_ns\":100"), std::string::npos);
}

TEST(RttStats, EqualityOperator) {
    RttStats a{.count = 10, .min_ns = 100};
    RttStats b{.count = 10, .min_ns = 100};
    EXPECT_EQ(a, b);
    b.count = 11;
    EXPECT_NE(a, b);
}

// =======================================================================
// ThreadStats
// =======================================================================

TEST(ThreadStats, SnapshotCapturesValues) {
    ThreadStats ts;
    ts.packets.store(100, std::memory_order_relaxed);
    ts.bytes.store(5000, std::memory_order_relaxed);
    ts.text_packets.store(20, std::memory_order_relaxed);
    ts.text_bytes.store(1000, std::memory_order_relaxed);
    ts.dropped.store(3, std::memory_order_relaxed);
    ts.crypto_errors.store(1, std::memory_order_relaxed);

    auto snap = ts.snapshot();
    EXPECT_EQ(snap.packets, 100);
    EXPECT_EQ(snap.bytes, 5000);
    EXPECT_EQ(snap.text_packets, 20);
    EXPECT_EQ(snap.text_bytes, 1000);
    EXPECT_EQ(snap.dropped, 3);
    EXPECT_EQ(snap.crypto_errors, 1);
}

TEST(ThreadStats, ResetZerosAll) {
    ThreadStats ts;
    ts.packets.store(42, std::memory_order_relaxed);
    ts.bytes.store(100, std::memory_order_relaxed);
    ts.reset();
    auto snap = ts.snapshot();
    EXPECT_EQ(snap.packets, 0);
    EXPECT_EQ(snap.bytes, 0);
}

TEST(ThreadStatsSnapshot, DeltaOperator) {
    ThreadStats::Snapshot a{.packets = 200, .bytes = 10000, .text_packets = 50,
                            .text_bytes = 2000, .dropped = 5, .crypto_errors = 2};
    ThreadStats::Snapshot b{.packets = 100, .bytes = 5000, .text_packets = 20,
                            .text_bytes = 1000, .dropped = 2, .crypto_errors = 1};
    auto d = a - b;
    EXPECT_EQ(d.packets, 100);
    EXPECT_EQ(d.bytes, 5000);
    EXPECT_EQ(d.text_packets, 30);
    EXPECT_EQ(d.dropped, 3);
    EXPECT_EQ(d.crypto_errors, 1);
}

TEST(ThreadStatsSnapshot, DumpContainsPackets) {
    ThreadStats::Snapshot s{.packets = 42, .bytes = 1000};
    auto d = s.dump();
    EXPECT_NE(d.find("42"), std::string::npos);
}

TEST(ThreadStatsSnapshot, ToJsonIsValid) {
    ThreadStats::Snapshot s{.packets = 10, .bytes = 500};
    auto j = s.to_json();
    EXPECT_NE(j.find("\"packets\":10"), std::string::npos);
    EXPECT_NE(j.find("\"bytes\":500"), std::string::npos);
}

// =======================================================================
// TransportStats
// =======================================================================

TEST(TransportStats, RateHelpersWithZeroUptime) {
    TransportStats s{};
    EXPECT_DOUBLE_EQ(s.tx_pps(), 0.0);
    EXPECT_DOUBLE_EQ(s.rx_pps(), 0.0);
    EXPECT_DOUBLE_EQ(s.tx_bps(), 0.0);
    EXPECT_DOUBLE_EQ(s.rx_bps(), 0.0);
    EXPECT_DOUBLE_EQ(s.tx_mbps(), 0.0);
    EXPECT_DOUBLE_EQ(s.rx_mbps(), 0.0);
}

TEST(TransportStats, RateHelpersWithNonZeroUptime) {
    TransportStats s{};
    s.tx_packets = 1000;
    s.rx_packets = 500;
    s.tx_bytes = 100000;
    s.rx_bytes = 50000;
    s.uptime_ns = 1'000'000'000;  // 1 second
    EXPECT_DOUBLE_EQ(s.tx_pps(), 1000.0);
    EXPECT_DOUBLE_EQ(s.rx_pps(), 500.0);
    EXPECT_DOUBLE_EQ(s.tx_bps(), 100000.0);
    EXPECT_DOUBLE_EQ(s.rx_bps(), 50000.0);
}

TEST(TransportStats, HandshakeMs) {
    TransportStats s{};
    s.handshake_ns = 5'000'000;  // 5ms
    EXPECT_DOUBLE_EQ(s.handshake_ms(), 5.0);
}

TEST(TransportStats, TlsSeqUsage) {
    TransportStats s{};
    s.tls_write_seq = 50;
    s.tls_read_seq = 25;
    s.tls_seq_limit = 100;
    EXPECT_DOUBLE_EQ(s.tls_write_seq_usage(), 0.5);
    EXPECT_DOUBLE_EQ(s.tls_read_seq_usage(), 0.25);
}

TEST(TransportStats, TlsSeqUsageZeroLimit) {
    TransportStats s{};
    s.tls_seq_limit = 0;
    EXPECT_DOUBLE_EQ(s.tls_write_seq_usage(), 0.0);
    EXPECT_DOUBLE_EQ(s.tls_read_seq_usage(), 0.0);
}

TEST(TransportStats, UptimeChronoDuration) {
    TransportStats s{};
    s.uptime_ns = 5'000'000'000;  // 5 seconds
    EXPECT_EQ(s.uptime(), std::chrono::nanoseconds{5'000'000'000});
}

TEST(TransportStats, DeltaOperator) {
    TransportStats a{};
    a.tx_packets = 200;
    a.rx_packets = 100;
    a.uptime_ns = 2'000'000'000;
    a.tx_queue_hwm = 50;
    a.remote_ip = "1.2.3.4";

    TransportStats b{};
    b.tx_packets = 100;
    b.rx_packets = 50;
    b.uptime_ns = 1'000'000'000;
    b.tx_queue_hwm = 30;

    auto d = a - b;
    EXPECT_EQ(d.tx_packets, 100);
    EXPECT_EQ(d.rx_packets, 50);
    EXPECT_EQ(d.uptime_ns, 1'000'000'000);
    // HWM takes latest value
    EXPECT_EQ(d.tx_queue_hwm, 50u);
    // Remote IP from latest snapshot
    EXPECT_EQ(d.remote_ip, "1.2.3.4");
}

TEST(TransportStats, DumpContainsUptime) {
    TransportStats s{};
    s.uptime_ns = 1'000'000'000;
    auto d = s.dump();
    EXPECT_NE(d.find("uptime"), std::string::npos);
}

TEST(TransportStats, ToJsonContainsFields) {
    TransportStats s{};
    s.tx_packets = 42;
    auto j = s.to_json();
    EXPECT_NE(j.find("\"tx_packets\":42"), std::string::npos);
}

// =======================================================================
// ConnectionInfo
// =======================================================================

TEST(ConnectionInfo, DumpContainsFields) {
    ConnectionInfo ci{
        .tls_version = "TLSv1.3",
        .cipher_name = "AES_256_GCM",
        .ws_subprotocol = "graphql",
        .remote_ip = "1.2.3.4",
        .remote_port = 443,
        .use_tls = true
    };
    auto d = ci.dump();
    EXPECT_NE(d.find("TLSv1.3"), std::string::npos);
    EXPECT_NE(d.find("AES_256_GCM"), std::string::npos);
    EXPECT_NE(d.find("1.2.3.4"), std::string::npos);
}

TEST(ConnectionInfo, ToJsonContainsFields) {
    ConnectionInfo ci{
        .tls_version = "TLSv1.3",
        .cipher_name = "AES_256_GCM",
        .remote_ip = "1.2.3.4",
        .remote_port = 443,
        .use_tls = true
    };
    auto j = ci.to_json();
    EXPECT_NE(j.find("\"tls_version\":\"TLSv1.3\""), std::string::npos);
    EXPECT_NE(j.find("\"use_tls\":true"), std::string::npos);
}

TEST(ConnectionInfo, EmptyFieldsShowDefaults) {
    ConnectionInfo ci{};
    auto d = ci.dump();
    EXPECT_NE(d.find("unknown"), std::string::npos);
    EXPECT_NE(d.find("(none)"), std::string::npos);
}

TEST(ConnectionInfo, Equality) {
    ConnectionInfo a{.tls_version = "TLSv1.3", .cipher_name = "AES", .remote_port = 443};
    ConnectionInfo b = a;
    EXPECT_EQ(a, b);
    b.tls_version = "none";
    EXPECT_NE(a, b);
}

// =======================================================================
// Two-phase frame filter
// =======================================================================

TEST(TwophaseFilter, EmptyFrameSpanIsNoop) {
    std::span<FrameView> empty;
    // Should not crash
    filter_detail::apply_twophase_filter(empty, [](const uint8_t*, size_t) -> uint32_t {
        return 1;
    });
}

TEST(TwophaseFilter, SingleFrameAlwaysDelivered) {
    uint8_t payload[] = {1, 2, 3};
    FrameView f{.payload = payload, .payload_len = 3, .deliver = true};
    std::span<FrameView> frames(&f, 1);

    filter_detail::apply_twophase_filter(frames,
        [](const uint8_t*, size_t) -> uint32_t { return 42; });

    EXPECT_TRUE(f.deliver);
}

TEST(TwophaseFilter, DuplicateHashKeepsOnlyLatest) {
    uint8_t payload[3] = {0};
    FrameView frames[3] = {
        {.payload = payload, .payload_len = 3, .deliver = true},
        {.payload = payload, .payload_len = 3, .deliver = true},
        {.payload = payload, .payload_len = 3, .deliver = true},
    };
    std::span<FrameView> span(frames, 3);

    // All frames have the same hash -> only last should be delivered
    filter_detail::apply_twophase_filter(span,
        [](const uint8_t*, size_t) -> uint32_t { return 100; });

    EXPECT_FALSE(frames[0].deliver);
    EXPECT_FALSE(frames[1].deliver);
    EXPECT_TRUE(frames[2].deliver);
}

TEST(TwophaseFilter, DifferentHashesAllDelivered) {
    uint8_t p1[1] = {1}, p2[1] = {2}, p3[1] = {3};
    FrameView frames[3] = {
        {.payload = p1, .payload_len = 1, .deliver = true},
        {.payload = p2, .payload_len = 1, .deliver = true},
        {.payload = p3, .payload_len = 1, .deliver = true},
    };
    std::span<FrameView> span(frames, 3);

    int call = 0;
    filter_detail::apply_twophase_filter(span,
        [&call](const uint8_t*, size_t) -> uint32_t { return ++call; });

    EXPECT_TRUE(frames[0].deliver);
    EXPECT_TRUE(frames[1].deliver);
    EXPECT_TRUE(frames[2].deliver);
}

TEST(TwophaseFilter, UnrecognizedHashAlwaysDelivered) {
    uint8_t payload[1] = {0};
    FrameView frames[2] = {
        {.payload = payload, .payload_len = 1, .deliver = true},
        {.payload = payload, .payload_len = 1, .deliver = true},
    };
    std::span<FrameView> span(frames, 2);

    // Return 0 = unrecognized -> all delivered
    filter_detail::apply_twophase_filter(span,
        [](const uint8_t*, size_t) -> uint32_t { return 0; });

    EXPECT_TRUE(frames[0].deliver);
    EXPECT_TRUE(frames[1].deliver);
}

TEST(TwophaseFilter, SentinelHashAlwaysDelivered) {
    uint8_t payload[1] = {0};
    FrameView frames[2] = {
        {.payload = payload, .payload_len = 1, .deliver = true},
        {.payload = payload, .payload_len = 1, .deliver = true},
    };
    std::span<FrameView> span(frames, 2);

    // Return UINT32_MAX (kEmptySlotHash sentinel) -> all delivered
    filter_detail::apply_twophase_filter(span,
        [](const uint8_t*, size_t) -> uint32_t { return UINT32_MAX; });

    EXPECT_TRUE(frames[0].deliver);
    EXPECT_TRUE(frames[1].deliver);
}

TEST(TwophaseFilter, MixedHashesKeepLatestPerSymbol) {
    uint8_t p[4][1] = {{1}, {2}, {3}, {4}};
    FrameView frames[4] = {
        {.payload = p[0], .payload_len = 1, .deliver = true},  // hash A
        {.payload = p[1], .payload_len = 1, .deliver = true},  // hash B
        {.payload = p[2], .payload_len = 1, .deliver = true},  // hash A (latest)
        {.payload = p[3], .payload_len = 1, .deliver = true},  // hash B (latest)
    };
    std::span<FrameView> span(frames, 4);

    // Alternate between two hashes
    int idx = 0;
    uint32_t hashes[] = {10, 20, 10, 20};
    filter_detail::apply_twophase_filter(span,
        [&idx, &hashes](const uint8_t*, size_t) -> uint32_t { return hashes[idx++]; });

    EXPECT_FALSE(frames[0].deliver);  // hash A, not latest
    EXPECT_FALSE(frames[1].deliver);  // hash B, not latest
    EXPECT_TRUE(frames[2].deliver);   // hash A, latest
    EXPECT_TRUE(frames[3].deliver);   // hash B, latest
}

TEST(TwophaseFilter, MakeTwophaseFilterFunctionPointer) {
    uint8_t payload[1] = {0};
    FrameView frames[2] = {
        {.payload = payload, .payload_len = 1, .deliver = true},
        {.payload = payload, .payload_len = 1, .deliver = true},
    };
    std::span<FrameView> span(frames, 2);

    auto filter = make_twophase_filter(
        +[](const uint8_t*, size_t) -> uint32_t { return 42; });
    filter(span);

    EXPECT_FALSE(frames[0].deliver);
    EXPECT_TRUE(frames[1].deliver);
}

TEST(TwophaseFilter, MakeTwophaseFilterStdFunction) {
    uint8_t payload[1] = {0};
    FrameView frames[2] = {
        {.payload = payload, .payload_len = 1, .deliver = true},
        {.payload = payload, .payload_len = 1, .deliver = true},
    };
    std::span<FrameView> span(frames, 2);

    std::function<uint32_t(const uint8_t*, size_t)> ext =
        [](const uint8_t*, size_t) -> uint32_t { return 42; };
    auto filter = make_twophase_filter(ext);
    filter(span);

    EXPECT_FALSE(frames[0].deliver);
    EXPECT_TRUE(frames[1].deliver);
}

// =======================================================================
// std::formatter specializations
// =======================================================================

TEST(Formatters, TransportEventFormats) {
    auto s = std::format("{}", TransportEvent::kConnected);
    EXPECT_EQ(s, "CONNECTED");
}

TEST(Formatters, TransportStateFormats) {
    auto s = std::format("{}", TransportState::kStopped);
    EXPECT_EQ(s, "STOPPED");
}

TEST(Formatters, RttStatsNoSamples) {
    RttStats r{};
    auto s = std::format("{}", r);
    EXPECT_NE(s.find("no samples"), std::string::npos);
}

TEST(Formatters, RttStatsWithSamples) {
    RttStats r{.count = 10, .min_ns = 100, .max_ns = 5000,
               .mean_ns = 2500.0, .p50_ns = 2000, .p99_ns = 4500, .p999_ns = 4900};
    auto s = std::format("{}", r);
    EXPECT_NE(s.find("n=10"), std::string::npos);
}

TEST(Formatters, ConnectionInfoFormats) {
    ConnectionInfo ci{.tls_version = "TLSv1.3", .cipher_name = "AES",
                      .remote_ip = "1.2.3.4", .remote_port = 443, .use_tls = true};
    auto s = std::format("{}", ci);
    EXPECT_NE(s.find("1.2.3.4"), std::string::npos);
    EXPECT_NE(s.find("TLSv1.3"), std::string::npos);
}

TEST(Formatters, TransportConfigFormats) {
    TransportConfig cfg;
    cfg.remote_host = "host.example";
    cfg.remote_port = 443;
    cfg.ws_path = "/ws";
    auto s = std::format("{}", cfg);
    EXPECT_NE(s.find("host.example"), std::string::npos);
}
