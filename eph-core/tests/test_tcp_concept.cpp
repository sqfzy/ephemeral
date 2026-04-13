/// @file test_tcp_concept.cpp
/// Unit tests for TcpTransport concept and TcpState formatter.
///
/// The concept itself is validated via static_assert against a mock type
/// that satisfies all requirements. The std::formatter<TcpState> specialization
/// is tested via std::format to ensure RFC 793 state names render correctly.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <string>

#include "eph/core/tcp_concept.hpp"

namespace en = eph::net;

// ---------------------------------------------------------------------------
// Mock TcpTransport for concept validation
// ---------------------------------------------------------------------------

struct MockTcp {
    std::expected<void, std::string>
    connect(std::chrono::milliseconds) { return {}; }

    std::expected<void, std::string> close() { return {}; }

    void reset() noexcept {}

    std::expected<size_t, std::string>
    send(const void*, size_t len) { return len; }

    std::expected<uint16_t, std::string>
    poll_rx(auto&&) { return uint16_t{0}; }

    uint64_t last_rx_burst_tsc() const { return 0; }
    uint16_t mss() const { return 1460; }
    en::TcpState state() const { return en::TcpState::Established; }
    bool is_established() const { return true; }
};

// ---------------------------------------------------------------------------
// Concept positive/negative checks
// ---------------------------------------------------------------------------

TEST(TcpTransport, MockSatisfiesConcept) {
    static_assert(en::TcpTransport<MockTcp>);
    SUCCEED();
}

TEST(TcpTransport, IntDoesNotSatisfy) {
    static_assert(!en::TcpTransport<int>);
    SUCCEED();
}

TEST(TcpTransport, StringDoesNotSatisfy) {
    static_assert(!en::TcpTransport<std::string>);
    SUCCEED();
}

// Missing connect
struct NoConnect {
    std::expected<void, std::string> close() { return {}; }
    void reset() noexcept {}
    std::expected<size_t, std::string> send(const void*, size_t) { return 0; }
    std::expected<uint16_t, std::string> poll_rx(auto&&) { return uint16_t{0}; }
    uint64_t last_rx_burst_tsc() const { return 0; }
    uint16_t mss() const { return 0; }
    en::TcpState state() const { return en::TcpState::Closed; }
    bool is_established() const { return false; }
};

TEST(TcpTransport, MissingConnectDoesNotSatisfy) {
    static_assert(!en::TcpTransport<NoConnect>);
    SUCCEED();
}

// Missing send
struct NoSend {
    std::expected<void, std::string> connect(std::chrono::milliseconds) { return {}; }
    std::expected<void, std::string> close() { return {}; }
    void reset() noexcept {}
    std::expected<uint16_t, std::string> poll_rx(auto&&) { return uint16_t{0}; }
    uint64_t last_rx_burst_tsc() const { return 0; }
    uint16_t mss() const { return 0; }
    en::TcpState state() const { return en::TcpState::Closed; }
    bool is_established() const { return false; }
};

TEST(TcpTransport, MissingSendDoesNotSatisfy) {
    static_assert(!en::TcpTransport<NoSend>);
    SUCCEED();
}

// ---------------------------------------------------------------------------
// std::formatter<TcpState>
// ---------------------------------------------------------------------------

TEST(TcpStateFormatter, FormatsEstablished) {
    auto s = std::format("{}", en::TcpState::Established);
    EXPECT_EQ(s, "ESTABLISHED");
}

TEST(TcpStateFormatter, FormatsAllStates) {
    EXPECT_EQ(std::format("{}", en::TcpState::Closed),      "CLOSED");
    EXPECT_EQ(std::format("{}", en::TcpState::Listen),       "LISTEN");
    EXPECT_EQ(std::format("{}", en::TcpState::SynSent),      "SYN_SENT");
    EXPECT_EQ(std::format("{}", en::TcpState::SynReceived),  "SYN_RECEIVED");
    EXPECT_EQ(std::format("{}", en::TcpState::Established),  "ESTABLISHED");
    EXPECT_EQ(std::format("{}", en::TcpState::FinWait1),     "FIN_WAIT_1");
    EXPECT_EQ(std::format("{}", en::TcpState::FinWait2),     "FIN_WAIT_2");
    EXPECT_EQ(std::format("{}", en::TcpState::CloseWait),    "CLOSE_WAIT");
    EXPECT_EQ(std::format("{}", en::TcpState::Closing),      "CLOSING");
    EXPECT_EQ(std::format("{}", en::TcpState::LastAck),      "LAST_ACK");
    EXPECT_EQ(std::format("{}", en::TcpState::TimeWait),     "TIME_WAIT");
}

TEST(TcpStateFormatter, FormatsUnknown) {
    auto bad = static_cast<en::TcpState>(200);
    EXPECT_EQ(std::format("{}", bad), "UNKNOWN");
}

TEST(TcpStateFormatter, WorksInFormatString) {
    auto s = std::format("state={}", en::TcpState::Established);
    EXPECT_EQ(s, "state=ESTABLISHED");
}
