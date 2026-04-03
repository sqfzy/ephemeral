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
// TcpConfig::format_mac
// ─────────────────────────────────────────────────────────────────────────────

TEST(TcpConfig, FormatMacZero) {
    rte_ether_addr mac{};
    std::memset(&mac, 0, sizeof(mac));
    EXPECT_EQ(TcpConfig::format_mac(mac), "00:00:00:00:00:00");
}

TEST(TcpConfig, FormatMacBroadcast) {
    rte_ether_addr mac{};
    std::memset(&mac, 0xFF, sizeof(mac));
    EXPECT_EQ(TcpConfig::format_mac(mac), "ff:ff:ff:ff:ff:ff");
}

TEST(TcpConfig, FormatMacCustom) {
    rte_ether_addr mac{};
    mac.addr_bytes[0] = 0xDE;
    mac.addr_bytes[1] = 0xAD;
    mac.addr_bytes[2] = 0xBE;
    mac.addr_bytes[3] = 0xEF;
    mac.addr_bytes[4] = 0x01;
    mac.addr_bytes[5] = 0x23;
    EXPECT_EQ(TcpConfig::format_mac(mac), "de:ad:be:ef:01:23");
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

// ─────────────────────────────────────────────────────────────────────────────
// TcpSession concept satisfaction
// ─────────────────────────────────────────────────────────────────────────────

TEST(TcpSession, SatisfiesTcpTransportConcept) {
    static_assert(eph::net::TcpTransport<TcpSession<>>,
                  "TcpSession must satisfy TcpTransport concept");
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

    auto dump = s.dump();
    EXPECT_NE(dump.find("reorder_hits: 42"), std::string::npos);
    EXPECT_NE(dump.find("reorder_overflows: 3"), std::string::npos);
    EXPECT_NE(dump.find("max_gap_size: 2920"), std::string::npos);
    EXPECT_NE(dump.find("gap[2^10..2^11): 5"), std::string::npos);
}

TEST(TcpStats, ToJsonIncludesTelemetryFields) {
    Stats s{};
    s.reorder_hits = 10;
    s.reorder_overflows = 1;
    s.max_gap_size = 1460;
    s.gap_histogram[10] = 7;

    auto json = s.to_json();
    EXPECT_NE(json.find("\"reorder_hits\":10"), std::string::npos);
    EXPECT_NE(json.find("\"reorder_overflows\":1"), std::string::npos);
    EXPECT_NE(json.find("\"max_gap_size\":1460"), std::string::npos);
    EXPECT_NE(json.find("\"gap_histogram\":{\"10\":7}"), std::string::npos);
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
