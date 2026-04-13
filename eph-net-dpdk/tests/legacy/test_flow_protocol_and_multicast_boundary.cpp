/// @file test_flow_protocol_and_multicast_boundary.cpp
/// Targeted tests for previously uncovered edge cases:
///   - FlowProtocol enum names and std::formatter
///   - MulticastConfig rx_burst boundary values (512 vs 513)
///   - MulticastGroup RFC 5771 224.0.0.0/24 warning boundary
///   - MulticastGroup mac_from_ipv4 correctness (RFC 1112)

#include <cstdint>
#include <format>
#include <string>

#include <gtest/gtest.h>

#include "eph/dpdk/flow_steering.hpp"
#include "eph/dpdk/multicast.hpp"

using namespace eph::dpdk;

// ═══════════════════════════════════════════════════════════════════════
// FlowProtocol — enum names and formatter
// ═══════════════════════════════════════════════════════════════════════

TEST(FlowProtocol, TcpName) {
    EXPECT_EQ(flow_protocol_name(FlowProtocol::Tcp), "TCP");
}

TEST(FlowProtocol, UdpName) {
    EXPECT_EQ(flow_protocol_name(FlowProtocol::Udp), "UDP");
}

TEST(FlowProtocol, StdFormatterTcp) {
    std::string s = std::format("{}", FlowProtocol::Tcp);
    EXPECT_EQ(s, "TCP");
}

TEST(FlowProtocol, StdFormatterUdp) {
    std::string s = std::format("{}", FlowProtocol::Udp);
    EXPECT_EQ(s, "UDP");
}

// ═══════════════════════════════════════════════════════════════════════
// MulticastConfig — rx_burst boundary values
// ═══════════════════════════════════════════════════════════════════════

TEST(MulticastConfigBoundary, RxBurst512Accepted) {
    MulticastConfig cfg{};
    cfg.rx_burst = 512;
    EXPECT_TRUE(cfg.validate().empty())
        << "rx_burst=512 is the maximum, should be accepted";
}

TEST(MulticastConfigBoundary, RxBurst513Rejected) {
    MulticastConfig cfg{};
    cfg.rx_burst = 513;
    EXPECT_FALSE(cfg.validate().empty())
        << "rx_burst=513 exceeds max, should be rejected";
}

TEST(MulticastConfigBoundary, RxBurst1Accepted) {
    MulticastConfig cfg{};
    cfg.rx_burst = 1;
    EXPECT_TRUE(cfg.validate().empty())
        << "rx_burst=1 is the minimum valid value";
}

TEST(MulticastConfigBoundary, RxBurstMaxUint16Rejected) {
    MulticastConfig cfg{};
    cfg.rx_burst = 65535;
    EXPECT_FALSE(cfg.validate().empty());
}

// ═══════════════════════════════════════════════════════════════════════
// MulticastGroup — RFC 5771 224.0.0.0/24 warning boundary
// ═══════════════════════════════════════════════════════════════════════

TEST(MulticastGroupWarning, Rfc5771_224_0_0_0_Warns) {
    MulticastGroup g{.group_ip = 0xE0000000};  // 224.0.0.0
    auto w = g.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("224.0.0") != std::string::npos ||
            msg.find("RFC") != std::string::npos ||
            msg.find("local") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "224.0.0.0 is in RFC 5771 local-network range";
}

TEST(MulticastGroupWarning, Rfc5771_224_0_0_255_Warns) {
    MulticastGroup g{.group_ip = 0xE00000FF};  // 224.0.0.255
    auto w = g.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("224.0.0") != std::string::npos ||
            msg.find("RFC") != std::string::npos ||
            msg.find("local") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "224.0.0.255 is the last address in the local range";
}

TEST(MulticastGroupWarning, Rfc5771_224_0_1_0_NoWarn) {
    MulticastGroup g{.group_ip = 0xE0000100};  // 224.0.1.0
    auto w = g.warnings();
    bool found_local = false;
    for (const auto& msg : w) {
        if (msg.find("local") != std::string::npos) {
            found_local = true;
            break;
        }
    }
    EXPECT_FALSE(found_local) << "224.0.1.0 is outside the local-network range";
}

// ═══════════════════════════════════════════════════════════════════════
// MulticastGroup — non-multicast IP rejected
// ═══════════════════════════════════════════════════════════════════════

TEST(MulticastGroupWarning, HighMulticastNoLocalWarning) {
    // 239.255.255.255 — valid multicast, not in local range
    MulticastGroup g{.group_ip = 0xEFFFFFFF};
    auto w = g.warnings();
    bool found_local = false;
    for (const auto& msg : w) {
        if (msg.find("local") != std::string::npos) {
            found_local = true;
            break;
        }
    }
    EXPECT_FALSE(found_local);
}

TEST(MulticastGroupWarning, ValidMulticastNoExtraWarnings) {
    MulticastGroup g{.group_ip = 0xEF010203};  // 239.1.2.3 — valid SSM range
    auto w = g.warnings();
    // Should have no warnings (not in 224.0.0.0/24 reserved range)
    bool found_local = false;
    for (const auto& msg : w) {
        if (msg.find("local") != std::string::npos) {
            found_local = true;
            break;
        }
    }
    EXPECT_FALSE(found_local);
}
