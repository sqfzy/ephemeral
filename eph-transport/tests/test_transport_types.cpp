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

// =======================================================================
// RttStats — edge cases
// =======================================================================

TEST(RttStats, SubtractionPreservesPercentiles) {
    // Verify that min/max/mean/percentiles come from lhs (later snapshot)
    RttStats a{.count = 100, .min_ns = 200, .max_ns = 8000,
               .mean_ns = 3000.0, .p50_ns = 2500, .p99_ns = 7000, .p999_ns = 7800};
    RttStats b{.count = 50,  .min_ns = 500, .max_ns = 6000,
               .mean_ns = 2000.0, .p50_ns = 1800, .p99_ns = 5000, .p999_ns = 5500};
    auto delta = a - b;
    EXPECT_EQ(delta.count, 50);
    // Percentiles are from lhs (a), not subtracted
    EXPECT_EQ(delta.min_ns, a.min_ns);
    EXPECT_EQ(delta.max_ns, a.max_ns);
    EXPECT_DOUBLE_EQ(delta.mean_ns, a.mean_ns);
    EXPECT_EQ(delta.p50_ns, a.p50_ns);
    EXPECT_EQ(delta.p99_ns, a.p99_ns);
    EXPECT_EQ(delta.p999_ns, a.p999_ns);
}

TEST(RttStats, ToJsonAllZeroFieldsValid) {
    RttStats r{};
    auto j = r.to_json();
    EXPECT_NE(j.find("\"count\":0"), std::string::npos);
    EXPECT_NE(j.find("\"min_ns\":0"), std::string::npos);
    EXPECT_NE(j.find("\"max_ns\":0"), std::string::npos);
}

// =======================================================================
// FrameView — default construction
// =======================================================================

TEST(FrameView, DefaultConstructedFieldsAreZero) {
    FrameView f{};
    EXPECT_EQ(f.payload, nullptr);
    EXPECT_EQ(f.payload_len, 0);
    EXPECT_EQ(f.opcode, 0);
    EXPECT_TRUE(f.deliver);  // default is true (deliver by default)
}

// =======================================================================
// TwophaseFilter — boundary conditions
// =======================================================================

TEST(TwophaseFilter, ExactlyMaxFramesPerBatch) {
    // Test with exactly kMaxFramesPerBatch (128) frames
    constexpr size_t N = filter_detail::kMaxFramesPerBatch;
    std::vector<uint8_t> backing(N);
    std::vector<FrameView> frames(N);
    for (size_t i = 0; i < N; ++i) {
        backing[i] = static_cast<uint8_t>(i % 4);
        frames[i] = {.payload = &backing[i], .payload_len = 1, .deliver = true};
    }
    // 4 unique hashes, should keep only the last of each
    filter_detail::apply_twophase_filter(
        std::span<FrameView>(frames),
        [](const uint8_t* data, size_t) -> uint32_t {
            return static_cast<uint32_t>(*data) + 1;
        });

    // Last frame for each hash should be delivered
    // Hash 1: frames 0,4,8,...,124 -> last is 124
    // Hash 2: frames 1,5,9,...,125 -> last is 125
    // Hash 3: frames 2,6,10,...,126 -> last is 126
    // Hash 4: frames 3,7,11,...,127 -> last is 127
    for (size_t i = 0; i < N; ++i) {
        if (i >= N - 4) {
            EXPECT_TRUE(frames[i].deliver) << "Frame " << i << " should be delivered";
        } else {
            EXPECT_FALSE(frames[i].deliver) << "Frame " << i << " should be skipped";
        }
    }
}

TEST(TwophaseFilter, MoreThanMaxFramesPerBatchTruncated) {
    // Test with more than kMaxFramesPerBatch frames
    // Only the first 128 should be processed; rest unchanged
    constexpr size_t N = filter_detail::kMaxFramesPerBatch + 10;
    std::vector<uint8_t> backing(N, 0);
    std::vector<FrameView> frames(N);
    for (size_t i = 0; i < N; ++i) {
        frames[i] = {.payload = &backing[i], .payload_len = 1, .deliver = true};
    }
    filter_detail::apply_twophase_filter(
        std::span<FrameView>(frames),
        [](const uint8_t*, size_t) -> uint32_t { return 42; });

    // Within first 128: only the last (index 127) should be delivered
    for (size_t i = 0; i < filter_detail::kMaxFramesPerBatch - 1; ++i) {
        EXPECT_FALSE(frames[i].deliver) << "Frame " << i << " should be skipped";
    }
    EXPECT_TRUE(frames[filter_detail::kMaxFramesPerBatch - 1].deliver);

    // Beyond kMaxFramesPerBatch: deliver flags untouched (true)
    for (size_t i = filter_detail::kMaxFramesPerBatch; i < N; ++i) {
        EXPECT_TRUE(frames[i].deliver) << "Frame " << i << " should remain delivered";
    }
}

// =======================================================================
// TransportStats — additional edge cases
// =======================================================================

TEST(TransportStats, TcpRxFieldsInJsonAndDump) {
    TransportStats s{};
    s.tcp_rx_packets = 100;
    s.tcp_rx_bursts = 10;
    auto j = s.to_json();
    EXPECT_NE(j.find("\"tcp_rx_packets\":100"), std::string::npos);
    EXPECT_NE(j.find("\"tcp_rx_bursts\":10"), std::string::npos);
    auto d = s.dump();
    EXPECT_NE(d.find("100 segments"), std::string::npos);
    EXPECT_NE(d.find("10 bursts"), std::string::npos);
}

TEST(TransportStats, DeltaSubtractsTcpRxFields) {
    TransportStats a{};
    a.tcp_rx_packets = 200;
    a.tcp_rx_bursts = 50;
    TransportStats b{};
    b.tcp_rx_packets = 100;
    b.tcp_rx_bursts = 30;
    auto d = a - b;
    EXPECT_EQ(d.tcp_rx_packets, 100);
    EXPECT_EQ(d.tcp_rx_bursts, 20);
}

TEST(TransportStats, DeltaCarriesLatencyHistograms) {
    // Latency histograms are not subtractable — the later snapshot's
    // values should be carried through to the delta.
    TransportStats a{};
    a.tx_latency = {.count = 50, .min_ns = 100, .max_ns = 5000,
                    .mean_ns = 2000.0, .p50_ns = 1800, .p99_ns = 4500, .p999_ns = 4900};
    a.rx_latency = {.count = 30, .min_ns = 200, .max_ns = 6000,
                    .mean_ns = 3000.0, .p50_ns = 2800, .p99_ns = 5500, .p999_ns = 5900};
    a.tx_queue_wait = {.count = 50, .p50_ns = 100};
    a.tx_encode = {.count = 50, .p50_ns = 200};
    a.rx_decrypt = {.count = 30, .p50_ns = 300};
    a.rx_decode = {.count = 30, .p50_ns = 400};

    TransportStats b{};
    // b has different values — they should be ignored in the delta
    b.tx_latency = {.count = 20};
    b.rx_latency = {.count = 10};

    auto d = a - b;
    EXPECT_EQ(d.tx_latency.count, a.tx_latency.count);
    EXPECT_EQ(d.tx_latency.p50_ns, a.tx_latency.p50_ns);
    EXPECT_EQ(d.rx_latency.count, a.rx_latency.count);
    EXPECT_EQ(d.rx_latency.p50_ns, a.rx_latency.p50_ns);
    EXPECT_EQ(d.tx_queue_wait.p50_ns, a.tx_queue_wait.p50_ns);
    EXPECT_EQ(d.tx_encode.p50_ns, a.tx_encode.p50_ns);
    EXPECT_EQ(d.rx_decrypt.p50_ns, a.rx_decrypt.p50_ns);
    EXPECT_EQ(d.rx_decode.p50_ns, a.rx_decode.p50_ns);
}

TEST(TransportStats, ToJsonContainsTcpRxAndLatencyFields) {
    TransportStats s{};
    s.tcp_rx_packets = 42;
    s.tcp_rx_bursts = 7;
    s.tx_latency = {.count = 10, .min_ns = 100, .max_ns = 5000,
                    .mean_ns = 2000.0, .p50_ns = 1800, .p99_ns = 4500, .p999_ns = 4900};
    auto j = s.to_json();
    EXPECT_NE(j.find("\"tcp_rx_packets\":42"), std::string::npos);
    EXPECT_NE(j.find("\"tcp_rx_bursts\":7"), std::string::npos);
    // Verify latency sub-object is nested correctly
    EXPECT_NE(j.find("\"tx_latency\":{"), std::string::npos);
}

TEST(TransportStats, DumpContainsTcpRxLine) {
    TransportStats s{};
    s.tcp_rx_packets = 1234;
    s.tcp_rx_bursts = 56;
    auto d = s.dump();
    EXPECT_NE(d.find("1234 segments"), std::string::npos);
    EXPECT_NE(d.find("56 bursts"), std::string::npos);
}

TEST(TransportStats, EqualityDetectsDifference) {
    TransportStats a{};
    TransportStats b{};
    EXPECT_EQ(a, b);
    a.tx_packets = 1;
    EXPECT_NE(a, b);
}

TEST(TransportStats, RateComputationWithLargeUptime) {
    TransportStats s{};
    s.tx_packets = 1'000'000;
    s.rx_packets = 500'000;
    s.tx_bytes = 100'000'000;
    s.rx_bytes = 50'000'000;
    s.uptime_ns = 60'000'000'000;  // 60 seconds
    // tx_pps = 1M / 60 = ~16666.67
    EXPECT_NEAR(s.tx_pps(), 16666.67, 1.0);
    // tx_mbps = 100M * 8 / 60 / 1e6 = ~13.33
    EXPECT_NEAR(s.tx_mbps(), 13.333, 0.01);
}

// =======================================================================
// TransportConfig::to_json() — special characters
// =======================================================================

TEST(TransportConfigJson, SpecialCharsAreEscaped) {
    TransportConfig cfg;
    cfg.remote_host = "host\"with\\quotes";
    cfg.remote_port = 443;
    cfg.ws_path = "/path?a=b&c=d";
    auto j = cfg.to_json();
    // Verify the JSON doesn't contain unescaped quotes from host
    // The json_escape function should handle this
    EXPECT_NE(j.find("host\\\"with"), std::string::npos);
}

// =======================================================================
// ConnectionInfo — additional coverage
// =======================================================================

TEST(ConnectionInfo, ToJsonSpecialChars) {
    ConnectionInfo ci{
        .tls_version = "TLSv1.3",
        .cipher_name = "TLS_AES_256_GCM_SHA384",
        .ws_subprotocol = "graphql-ws",
        .remote_ip = "10.0.0.1",
        .remote_port = 443,
        .use_tls = true
    };
    auto j = ci.to_json();
    EXPECT_NE(j.find("\"ws_subprotocol\":\"graphql-ws\""), std::string::npos);
    EXPECT_NE(j.find("\"remote_port\":443"), std::string::npos);
}

TEST(ConnectionInfo, DumpNoTls) {
    ConnectionInfo ci{.use_tls = false};
    auto d = ci.dump();
    EXPECT_NE(d.find("unknown"), std::string::npos);
}
