/// @file test_tcp.cpp
/// Unit tests for DPDK TcpConfig validation, formatting, and TcpSession
/// compile-time concept satisfaction.

#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep
#include "eph/dpdk/tcp.hpp"

using namespace eph::dpdk;

// ─────────────────────────────────────────────────────────────────────────────
// TcpConfig::validate — happy path
// ─────────────────────────────────────────────────────────────────────────────

static TcpConfig make_valid_config() {
    TcpConfig cfg;
    cfg.tuple.src_ip   = 0x0A000001; // 10.0.0.1
    cfg.tuple.dst_ip   = 0x0A000002; // 10.0.0.2
    cfg.tuple.src_port = 12345;
    cfg.tuple.dst_port = 443;
    cfg.mss            = 1460;
    cfg.recv_window    = 65535;
    return cfg;
}

TEST(TcpConfig, ValidConfigPassesValidation) {
    auto cfg = make_valid_config();
    EXPECT_TRUE(cfg.validate().empty()) << cfg.validate();
}

// ─────────────────────────────────────────────────────────────────────────────
// TcpConfig::validate — error cases
// ─────────────────────────────────────────────────────────────────────────────

TEST(TcpConfig, ZeroSrcIpFails) {
    auto cfg = make_valid_config();
    cfg.tuple.src_ip = 0;
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("src_ip"), std::string_view::npos);
}

TEST(TcpConfig, ZeroDstIpFails) {
    auto cfg = make_valid_config();
    cfg.tuple.dst_ip = 0;
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("dst_ip"), std::string_view::npos);
}

TEST(TcpConfig, ZeroSrcPortFails) {
    auto cfg = make_valid_config();
    cfg.tuple.src_port = 0;
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("src_port"), std::string_view::npos);
}

TEST(TcpConfig, ZeroDstPortFails) {
    auto cfg = make_valid_config();
    cfg.tuple.dst_port = 0;
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("dst_port"), std::string_view::npos);
}

TEST(TcpConfig, ZeroMssFails) {
    auto cfg = make_valid_config();
    cfg.mss = 0;
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("mss"), std::string_view::npos);
}

TEST(TcpConfig, MssExceedsJumboFrameFails) {
    auto cfg = make_valid_config();
    cfg.mss = 9001;
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("jumbo"), std::string_view::npos);
}

TEST(TcpConfig, MssAtJumboFrameLimit) {
    auto cfg = make_valid_config();
    cfg.mss = 9000;
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(TcpConfig, ZeroRecvWindowFails) {
    auto cfg = make_valid_config();
    cfg.recv_window = 0;
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("recv_window"), std::string_view::npos);
}

TEST(TcpConfig, RecvWindowExceeds65535Fails) {
    auto cfg = make_valid_config();
    cfg.recv_window = 65536;
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("recv_window"), std::string_view::npos);
}

TEST(TcpConfig, RecvWindowAt65535Passes) {
    auto cfg = make_valid_config();
    cfg.recv_window = 65535;
    EXPECT_TRUE(cfg.validate().empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// TcpConfig::validate — max_rx_burst boundary
// ─────────────────────────────────────────────────────────────────────────────

TEST(TcpConfig, MaxRxBurstZeroAutoIsValid) {
    auto cfg = make_valid_config();
    cfg.max_rx_burst = 0;  // auto-calculate from MSS
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(TcpConfig, MaxRxBurstAt32Passes) {
    auto cfg = make_valid_config();
    cfg.max_rx_burst = 32;
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(TcpConfig, MaxRxBurstExceeds32Fails) {
    auto cfg = make_valid_config();
    cfg.max_rx_burst = 33;
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("max_rx_burst"), std::string_view::npos);
}

TEST(TcpConfig, MaxRxBurstOneIsValid) {
    auto cfg = make_valid_config();
    cfg.max_rx_burst = 1;
    EXPECT_TRUE(cfg.validate().empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// TcpConfig::validate — keepalive_interval bounds
// ─────────────────────────────────────────────────────────────────────────────

TEST(TcpConfig, NegativeKeepaliveIntervalFails) {
    // chrono::milliseconds is signed int64; bypass the public KeepaliveConfig
    // path by constructing TcpConfig directly with a negative value, then
    // assert validate() rejects it. Without the rejection,
    // tick_keepalive's `static_cast<uint64_t>(negative * 3.0)` fallback is
    // implementation-defined (typically 0 or a huge wrap, both wrong).
    auto cfg = make_valid_config();
    cfg.keepalive_interval = std::chrono::milliseconds{-100};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("keepalive_interval"), std::string_view::npos);
}

TEST(TcpConfig, ZeroKeepaliveIntervalIsValid) {
    auto cfg = make_valid_config();
    cfg.keepalive_interval = std::chrono::milliseconds::zero();
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(TcpConfig, KeepaliveZeroProbesWhileEnabledFails) {
    auto cfg = make_valid_config();
    cfg.keepalive_interval = std::chrono::milliseconds{5000};
    cfg.keepalive_probes = 0;
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("keepalive_probes"), std::string_view::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// net::format_mac (canonical formatter; TcpConfig::format_mac was removed)
// ─────────────────────────────────────────────────────────────────────────────

TEST(NetFormatMac, FormatMacZero) {
    rte_ether_addr mac{};
    std::memset(&mac, 0, sizeof(mac));
    EXPECT_STREQ(net::format_mac(mac).data(), "00:00:00:00:00:00");
}

TEST(NetFormatMac, FormatMacBroadcast) {
    rte_ether_addr mac{};
    std::memset(&mac, 0xFF, sizeof(mac));
    EXPECT_STREQ(net::format_mac(mac).data(), "ff:ff:ff:ff:ff:ff");
}

TEST(NetFormatMac, FormatMacCustom) {
    rte_ether_addr mac{};
    mac.addr_bytes[0] = 0xDE;
    mac.addr_bytes[1] = 0xAD;
    mac.addr_bytes[2] = 0xBE;
    mac.addr_bytes[3] = 0xEF;
    mac.addr_bytes[4] = 0x01;
    mac.addr_bytes[5] = 0x23;
    EXPECT_STREQ(net::format_mac(mac).data(), "de:ad:be:ef:01:23");
}

// ─────────────────────────────────────────────────────────────────────────────
// TcpConfig::dump and to_json
// ─────────────────────────────────────────────────────────────────────────────

TEST(TcpConfig, DumpContainsKeyFields) {
    auto cfg = make_valid_config();
    auto dump = cfg.dump();
    EXPECT_NE(dump.find("TcpConfig"), std::string::npos);
    EXPECT_NE(dump.find("mss: 1460"), std::string::npos);
    EXPECT_NE(dump.find("recv_window: 65535"), std::string::npos);
}

TEST(TcpConfig, ToJsonIsValidStructure) {
    auto cfg = make_valid_config();
    auto json = cfg.to_json();
    EXPECT_NE(json.find("\"src_ip\""), std::string::npos);
    EXPECT_NE(json.find("\"dst_ip\""), std::string::npos);
    EXPECT_NE(json.find("\"mss\":1460"), std::string::npos);
    // Verify braces
    EXPECT_EQ(json.front(), '{');
    EXPECT_EQ(json.back(), '}');
}

// ─────────────────────────────────────────────────────────────────────────────
// TcpConfig equality
// ─────────────────────────────────────────────────────────────────────────────

TEST(TcpConfig, EqualityIdentical) {
    auto a = make_valid_config();
    auto b = make_valid_config();
    EXPECT_EQ(a, b);
}

TEST(TcpConfig, EqualityDifferentPort) {
    auto a = make_valid_config();
    auto b = make_valid_config();
    b.tuple.src_port = 9999;
    EXPECT_NE(a, b);
}

TEST(TcpConfig, EqualityDifferentMac) {
    auto a = make_valid_config();
    auto b = make_valid_config();
    b.src_mac.addr_bytes[0] = 0xFF;
    EXPECT_NE(a, b);
}

// Keepalive fields participate in equality — see operator==, which must
// compare them so two configs that differ only in keepalive do not collapse
// to equal. Regression guard for a silent drop observed during review.
TEST(TcpConfig, EqualityDifferentKeepaliveInterval) {
    auto a = make_valid_config();
    auto b = make_valid_config();
    b.keepalive_interval = std::chrono::milliseconds{5000};
    EXPECT_NE(a, b);
}

TEST(TcpConfig, EqualityDifferentKeepaliveProbes) {
    auto a = make_valid_config();
    auto b = make_valid_config();
    // When keepalive_interval is zero, probes is unused but still a
    // distinguishing field for config snapshot / round-trip purposes.
    a.keepalive_interval = std::chrono::milliseconds{5000};
    b.keepalive_interval = std::chrono::milliseconds{5000};
    a.keepalive_probes = 3;
    b.keepalive_probes = 7;
    EXPECT_NE(a, b);
}

// ─────────────────────────────────────────────────────────────────────────────
// TcpSession concept satisfaction
// ─────────────────────────────────────────────────────────────────────────────

TEST(TcpSession, SatisfiesTcpTransportConcept) {
    static_assert(eph::net::TcpTransport<TcpSession<>>,
                  "TcpSession must satisfy TcpTransport concept");
}

TEST(TcpSession, TypeTraits) {
    // TcpSession manages NIC resources — copy would be unsafe
    static_assert(!std::is_copy_constructible_v<TcpSession<>>);
    static_assert(!std::is_copy_assignable_v<TcpSession<>>);
    // Move is allowed for ownership transfer (e.g., into unique_ptr)
    static_assert(std::is_move_constructible_v<TcpSession<>>);
}

// Default ReorderSlots is now 64 (Layer 1)
TEST(TcpSession, DefaultReorderSlots64) {
    // TcpSession<> should use 64 slots by default
    static_assert(sizeof(TcpSession<>) > sizeof(TcpSession<8>),
                  "Default TcpSession should be larger than TcpSession<8>");
    // Both must satisfy the concept
    static_assert(eph::net::TcpTransport<TcpSession<64>>);
    static_assert(eph::net::TcpTransport<TcpSession<8>>);
}

// ─────────────────────────────────────────────────────────────────────────────
// Stats telemetry fields (Layer 3)
// ─────────────────────────────────────────────────────────────────────────────

using Stats = TcpSession<>::Stats;

TEST(TcpStats, GapBucketZero) {
    EXPECT_EQ(Stats::gap_bucket(0), 0);
}

TEST(TcpStats, GapBucketPowersOfTwo) {
    EXPECT_EQ(Stats::gap_bucket(1), 0);    // [1,2) → bucket 0
    EXPECT_EQ(Stats::gap_bucket(2), 1);    // [2,4) → bucket 1
    EXPECT_EQ(Stats::gap_bucket(3), 1);    // [2,4) → bucket 1
    EXPECT_EQ(Stats::gap_bucket(4), 2);    // [4,8) → bucket 2
    EXPECT_EQ(Stats::gap_bucket(1460), 10); // MSS-sized gap → bucket 10 ([1024,2048))
    EXPECT_EQ(Stats::gap_bucket(65535), 15); // Max window → bucket 15
}

TEST(TcpStats, GapBucketLargeValues) {
    EXPECT_EQ(Stats::gap_bucket(1u << 20), 20);
    EXPECT_EQ(Stats::gap_bucket(UINT32_MAX), 31);
}

TEST(TcpStats, DumpIncludesTelemetryFields) {
    Stats s{};
    s.reorder_hits = 42;
    s.reorder_overflows = 3;
    s.max_gap_size = 2920;
    s.gap_histogram[10] = 5;  // bucket [1024, 2048)
    s.keepalive_probes_sent   = 7;
    s.keepalive_send_failures = 2;

    auto dump = s.dump();
    EXPECT_NE(dump.find("reorder_hits: 42"), std::string::npos);
    EXPECT_NE(dump.find("reorder_overflows: 3"), std::string::npos);
    EXPECT_NE(dump.find("max_gap_size: 2920"), std::string::npos);
    EXPECT_NE(dump.find("gap[2^10..2^11): 5"), std::string::npos);
    // Keepalive observability — both fields must surface so operators
    // can attribute connection drops to silent peer (probes_sent rising,
    // no replies) vs stuck NIC (send_failures rising).
    EXPECT_NE(dump.find("keepalive_probes_sent: 7"), std::string::npos);
    EXPECT_NE(dump.find("keepalive_send_failures: 2"), std::string::npos);
}

TEST(TcpStats, ToJsonIncludesTelemetryFields) {
    Stats s{};
    s.reorder_hits = 10;
    s.reorder_overflows = 1;
    s.max_gap_size = 1460;
    s.gap_histogram[10] = 7;
    s.keepalive_probes_sent   = 4;
    s.keepalive_send_failures = 1;

    auto json = s.to_json();
    EXPECT_NE(json.find("\"reorder_hits\":10"), std::string::npos);
    EXPECT_NE(json.find("\"reorder_overflows\":1"), std::string::npos);
    EXPECT_NE(json.find("\"max_gap_size\":1460"), std::string::npos);
    EXPECT_NE(json.find("\"gap_histogram\":{\"10\":7}"), std::string::npos);
    EXPECT_NE(json.find("\"keepalive_probes_sent\":4"), std::string::npos);
    EXPECT_NE(json.find("\"keepalive_send_failures\":1"), std::string::npos);
}

TEST(TcpStats, ToJsonOmitsEmptyGapHistogram) {
    Stats s{};
    auto json = s.to_json();
    EXPECT_EQ(json.find("gap_histogram"), std::string::npos);
}

TEST(TcpStats, OperatorMinusDiffsTelemetryFields) {
    Stats s1{};
    s1.reorder_hits = 50;
    s1.reorder_overflows = 5;
    s1.max_gap_size = 3000;
    s1.gap_histogram[10] = 20;
    s1.gap_histogram[11] = 3;

    Stats s2{};
    s2.reorder_hits = 30;
    s2.reorder_overflows = 2;
    s2.max_gap_size = 1500;
    s2.gap_histogram[10] = 15;
    s2.gap_histogram[11] = 1;

    auto delta = s1 - s2;
    EXPECT_EQ(delta.reorder_hits, 20u);
    EXPECT_EQ(delta.reorder_overflows, 3u);
    // max_gap_size is point-in-time (latest snapshot), not diffed
    EXPECT_EQ(delta.max_gap_size, 3000u);
    EXPECT_EQ(delta.gap_histogram[10], 5u);
    EXPECT_EQ(delta.gap_histogram[11], 2u);
}

TEST(TcpStats, GapBucketIsConstexpr) {
    // Verify gap_bucket is constexpr-evaluable
    static_assert(Stats::gap_bucket(0) == 0);
    static_assert(Stats::gap_bucket(1) == 0);
    static_assert(Stats::gap_bucket(1024) == 10);
}

TEST(TcpStats, EqualityDefaultConstructed) {
    Stats a{};
    Stats b{};
    EXPECT_EQ(a, b);
}

TEST(TcpStats, EqualityDifferentFields) {
    Stats a{};
    Stats b{};
    a.tx_packets = 1;
    EXPECT_NE(a, b);
}

TEST(TcpStats, EqualityGapHistogram) {
    Stats a{};
    Stats b{};
    a.gap_histogram[5] = 42;
    EXPECT_NE(a, b);
    b.gap_histogram[5] = 42;
    EXPECT_EQ(a, b);
}

TEST(TcpStats, OperatorMinusDefaultIsZero) {
    Stats a{};
    auto delta = a - a;
    EXPECT_EQ(delta, Stats{});
}

// ─────────────────────────────────────────────────────────────────────────────
// TcpConfig::warnings — advisory diagnostics
// ─────────────────────────────────────────────────────────────────────────────

TEST(TcpConfigWarnings, ValidConfigNoWarnings) {
    auto cfg = make_valid_config();
    // Set non-zero MACs to avoid the "all zeros" warning
    cfg.src_mac.addr_bytes[0] = 0xDE;
    cfg.dst_mac.addr_bytes[0] = 0xBE;
    auto w = cfg.warnings();
    EXPECT_TRUE(w.empty()) << "Unexpected warning: " << (w.empty() ? "" : w[0]);
}

TEST(TcpConfigWarnings, LoopbackSrcIp) {
    auto cfg = make_valid_config();
    cfg.src_mac.addr_bytes[0] = 0xDE;
    cfg.dst_mac.addr_bytes[0] = 0xBE;
    cfg.tuple.src_ip = 0x7F000001; // 127.0.0.1
    auto w = cfg.warnings();
    ASSERT_FALSE(w.empty());
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("loopback") != std::string::npos &&
            msg.find("src_ip") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected loopback warning for src_ip";
}

TEST(TcpConfigWarnings, LoopbackDstIp) {
    auto cfg = make_valid_config();
    cfg.src_mac.addr_bytes[0] = 0xDE;
    cfg.dst_mac.addr_bytes[0] = 0xBE;
    cfg.tuple.dst_ip = 0x7F000001; // 127.0.0.1
    auto w = cfg.warnings();
    ASSERT_FALSE(w.empty());
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("loopback") != std::string::npos &&
            msg.find("dst_ip") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected loopback warning for dst_ip";
}

TEST(TcpConfigWarnings, SelfConnect) {
    auto cfg = make_valid_config();
    cfg.src_mac.addr_bytes[0] = 0xDE;
    cfg.dst_mac.addr_bytes[0] = 0xBE;
    cfg.tuple.src_ip = 0x0A000001;
    cfg.tuple.dst_ip = 0x0A000001; // same as src
    auto w = cfg.warnings();
    ASSERT_FALSE(w.empty());
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("self-connect") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected self-connect warning";
}

TEST(TcpConfigWarnings, LowMss) {
    auto cfg = make_valid_config();
    cfg.src_mac.addr_bytes[0] = 0xDE;
    cfg.dst_mac.addr_bytes[0] = 0xBE;
    cfg.mss = 256;
    auto w = cfg.warnings();
    ASSERT_FALSE(w.empty());
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("mss=256") != std::string::npos &&
            msg.find("fragmentation") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected low MSS warning";
}

TEST(TcpConfigWarnings, JumboMss) {
    auto cfg = make_valid_config();
    cfg.src_mac.addr_bytes[0] = 0xDE;
    cfg.dst_mac.addr_bytes[0] = 0xBE;
    cfg.mss = 8000;
    auto w = cfg.warnings();
    ASSERT_FALSE(w.empty());
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("jumbo") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected jumbo frame warning";
}

TEST(TcpConfigWarnings, ZeroSrcMac) {
    auto cfg = make_valid_config();
    cfg.dst_mac.addr_bytes[0] = 0xBE;
    // src_mac defaults to all zeros
    auto w = cfg.warnings();
    ASSERT_FALSE(w.empty());
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("src_mac") != std::string::npos &&
            msg.find("zeros") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected zero src_mac warning";
}

TEST(TcpConfigWarnings, ZeroDstMac) {
    auto cfg = make_valid_config();
    cfg.src_mac.addr_bytes[0] = 0xDE;
    // dst_mac defaults to all zeros
    auto w = cfg.warnings();
    ASSERT_FALSE(w.empty());
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("dst_mac") != std::string::npos &&
            msg.find("zeros") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected zero dst_mac warning";
}

TEST(TcpConfigWarnings, SharedNonZeroQueueWarns) {
    auto cfg = make_valid_config();
    cfg.src_mac.addr_bytes[0] = 0xDE;
    cfg.dst_mac.addr_bytes[0] = 0xBE;
    cfg.tx_queue_id = 2;
    cfg.rx_queue_id = 2;
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("tx_queue_id == rx_queue_id") != std::string::npos &&
            msg.find("contention") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected shared queue contention warning";
}

TEST(TcpConfigWarnings, SharedQueueZeroNoWarning) {
    auto cfg = make_valid_config();
    cfg.src_mac.addr_bytes[0] = 0xDE;
    cfg.dst_mac.addr_bytes[0] = 0xBE;
    cfg.tx_queue_id = 0;
    cfg.rx_queue_id = 0;
    auto w = cfg.warnings();
    // Queue 0 shared is the default -- should NOT warn
    for (const auto& msg : w) {
        EXPECT_EQ(msg.find("tx_queue_id == rx_queue_id"), std::string::npos)
            << "Should not warn about shared queue 0: " << msg;
    }
}

TEST(TcpConfigWarnings, DifferentQueuesNoContention) {
    auto cfg = make_valid_config();
    cfg.src_mac.addr_bytes[0] = 0xDE;
    cfg.dst_mac.addr_bytes[0] = 0xBE;
    cfg.tx_queue_id = 1;
    cfg.rx_queue_id = 2;
    auto w = cfg.warnings();
    for (const auto& msg : w) {
        EXPECT_EQ(msg.find("contention"), std::string::npos)
            << "Should not warn about contention with different queues: " << msg;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// TcpConfig::validate boundary: mss == 1 (minimum valid)
// ─────────────────────────────────────────────────────────────────────────────

TEST(TcpConfig, MssAtMinimumOne) {
    auto cfg = make_valid_config();
    cfg.mss = 1;
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(TcpConfig, MaxRxBurstBoundary) {
    // Test the full valid range [0, 32]
    auto cfg = make_valid_config();
    for (uint16_t v = 0; v <= 32; ++v) {
        cfg.max_rx_burst = v;
        EXPECT_TRUE(cfg.validate().empty()) << "max_rx_burst=" << v;
    }
    cfg.max_rx_burst = 33;
    EXPECT_FALSE(cfg.validate().empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// TcpConfig::dump and to_json — max_rx_burst inclusion
// ─────────────────────────────────────────────────────────────────────────────

TEST(TcpConfig, DumpIncludesMaxRxBurst) {
    auto cfg = make_valid_config();
    cfg.max_rx_burst = 16;
    auto dump = cfg.dump();
    EXPECT_NE(dump.find("max_rx_burst: 16"), std::string::npos);
}

TEST(TcpConfig, ToJsonIncludesMaxRxBurst) {
    auto cfg = make_valid_config();
    cfg.max_rx_burst = 24;
    auto json = cfg.to_json();
    EXPECT_NE(json.find("\"max_rx_burst\":24"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// TcpStats — multi-bucket gap histogram in JSON
// ─────────────────────────────────────────────────────────────────────────────

TEST(TcpStats, ToJsonMultipleBucketsGapHistogram) {
    Stats s{};
    s.gap_histogram[5] = 10;
    s.gap_histogram[10] = 3;
    s.gap_histogram[15] = 1;
    auto json = s.to_json();
    // All three buckets should appear in the gap_histogram object
    EXPECT_NE(json.find("\"gap_histogram\":{"), std::string::npos);
    EXPECT_NE(json.find("\"5\":10"), std::string::npos);
    EXPECT_NE(json.find("\"10\":3"), std::string::npos);
    EXPECT_NE(json.find("\"15\":1"), std::string::npos);
}

TEST(TcpStats, DumpMultipleBucketsGapHistogram) {
    Stats s{};
    s.gap_histogram[0] = 100;
    s.gap_histogram[31] = 1;
    auto dump = s.dump();
    EXPECT_NE(dump.find("gap[2^0..2^1): 100"), std::string::npos);
    EXPECT_NE(dump.find("gap[2^31..2^32): 1"), std::string::npos);
}

TEST(TcpStats, DumpSkipsZeroBuckets) {
    Stats s{};
    s.gap_histogram[5] = 42;
    auto dump = s.dump();
    EXPECT_NE(dump.find("gap[2^5..2^6): 42"), std::string::npos);
    // Bucket 4 and 6 (neighbors) should NOT appear since they're zero
    EXPECT_EQ(dump.find("gap[2^4..2^5)"), std::string::npos);
    EXPECT_EQ(dump.find("gap[2^6..2^7)"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// Delayed-ACK timer logic (eph::dpdk::detail::ack_timer_expired)
//
// Pure-function tests for the core delayed-ACK timer expiry check used by
// TcpSession::flush_pending_ack(). Validates the four-quadrant truth table
// (pending × elapsed) plus the unsigned-wraparound edge case.
//
// Integration of the timer with the full TcpSession state machine is
// covered by the latency benchmark suite (`benchmarks/latency/order_rtt`)
// — running 500k+ rounds with the production mock confirms that
// `tcp_data_queue` slow-path counts stay near zero on the receiver side.
// ─────────────────────────────────────────────────────────────────────────────

TEST(DelayedAckTimer, NotPendingNeverFires) {
    // pending_since_tsc == 0 means "no ACK owed" — must always return false
    // regardless of elapsed time or delay.
    EXPECT_FALSE(detail::ack_timer_expired(/*pending=*/0, /*now=*/0,        /*delay=*/100));
    EXPECT_FALSE(detail::ack_timer_expired(/*pending=*/0, /*now=*/1'000'000,/*delay=*/100));
    EXPECT_FALSE(detail::ack_timer_expired(/*pending=*/0, /*now=*/UINT64_MAX,/*delay=*/0));
}

TEST(DelayedAckTimer, PendingButInsideDelayWindowDefersFire) {
    // elapsed = 0 (instant) — must defer
    EXPECT_FALSE(detail::ack_timer_expired(/*pending=*/100, /*now=*/100, /*delay=*/50));
    // elapsed = 49 < delay 50 — must defer
    EXPECT_FALSE(detail::ack_timer_expired(/*pending=*/100, /*now=*/149, /*delay=*/50));
    // Realistic datacenter scenario: 20 µs RTT = ~20k cycles, well under 40k delay
    EXPECT_FALSE(detail::ack_timer_expired(
        /*pending=*/1'000'000, /*now=*/1'020'000, /*delay=*/40'000));
}

TEST(DelayedAckTimer, FiresAtExactDelayBoundary) {
    // elapsed == delay → fire (>= comparison is correct boundary)
    EXPECT_TRUE(detail::ack_timer_expired(/*pending=*/100, /*now=*/150, /*delay=*/50));
}

TEST(DelayedAckTimer, FiresWellAfterDelay) {
    // elapsed >> delay → fire
    EXPECT_TRUE(detail::ack_timer_expired(
        /*pending=*/100, /*now=*/1'000'000, /*delay=*/50));
    // RX-only stream scenario: server idle for 1 ms, ACK long overdue
    EXPECT_TRUE(detail::ack_timer_expired(
        /*pending=*/1'000'000, /*now=*/2'000'000, /*delay=*/40'000));
}

TEST(DelayedAckTimer, ZeroDelayFiresImmediatelyWhenPending) {
    // delay_cycles == 0 (degenerate config: timer always expired if pending)
    EXPECT_TRUE(detail::ack_timer_expired(/*pending=*/100, /*now=*/100, /*delay=*/0));
    EXPECT_TRUE(detail::ack_timer_expired(/*pending=*/100, /*now=*/200, /*delay=*/0));
    // But still no-op when there's nothing pending
    EXPECT_FALSE(detail::ack_timer_expired(/*pending=*/0,   /*now=*/100, /*delay=*/0));
}

TEST(DelayedAckTimer, UnsignedSubtractionHandlesTscWraparound) {
    // cntvct_el0 is a 64-bit monotonic counter; at 1 GHz it won't wrap for
    // ~500 years. But the function MUST work correctly across the wrap point
    // because it relies on `(now - pending) mod 2^64`. Verify both directions.

    // Case 1: pending and now both pre-wrap, normal arithmetic
    EXPECT_TRUE(detail::ack_timer_expired(
        /*pending=*/UINT64_MAX - 100, /*now=*/UINT64_MAX - 50, /*delay=*/40));

    // Case 2: pending pre-wrap, now post-wrap. Elapsed should compute correctly:
    //   pending = 2^64 - 50
    //   now     = 49     (so 100 cycles after pending, modulo 2^64)
    //   elapsed = 49 - (2^64 - 50) = 49 + 50 + 1 ≡ 100 (mod 2^64)
    // expected fire because 100 >= 40
    EXPECT_TRUE(detail::ack_timer_expired(
        /*pending=*/UINT64_MAX - 50, /*now=*/49, /*delay=*/40));

    // Case 3: same setup, but delay > elapsed → still defer
    //   elapsed = 100, delay = 200 → defer
    EXPECT_FALSE(detail::ack_timer_expired(
        /*pending=*/UINT64_MAX - 50, /*now=*/49, /*delay=*/200));
}
