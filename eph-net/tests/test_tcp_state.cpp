/// @file test_tcp_state.cpp
/// Unit tests for `eph::net::TcpState` and `tcp_state_name`.
///
/// Exhaustively covers every enumerator to ensure the name-mapping
/// switch in `tcp_state_name` never drifts behind the enum.

#include <cstring>
#include <gtest/gtest.h>

#include "eph/net/tcp_state.hpp"

namespace en = eph::net;

TEST(TcpState, UnderlyingIsUint8) {
    static_assert(sizeof(en::TcpState) == 1);
    static_assert(std::is_same_v<std::underlying_type_t<en::TcpState>, uint8_t>);
}

TEST(TcpState, ClosedIsZero) {
    // Documented contract: default-constructed TcpState is Closed.
    EXPECT_EQ(static_cast<uint8_t>(en::TcpState::Closed), 0);
    en::TcpState s{};
    EXPECT_EQ(s, en::TcpState::Closed);
}

TEST(TcpState, NameForEveryEnumerator) {
    struct Case { en::TcpState s; const char* name; };
    constexpr Case cases[] = {
        {en::TcpState::Closed,       "CLOSED"},
        {en::TcpState::Listen,       "LISTEN"},
        {en::TcpState::SynSent,      "SYN_SENT"},
        {en::TcpState::SynReceived,  "SYN_RECEIVED"},
        {en::TcpState::Established,  "ESTABLISHED"},
        {en::TcpState::FinWait1,     "FIN_WAIT_1"},
        {en::TcpState::FinWait2,     "FIN_WAIT_2"},
        {en::TcpState::CloseWait,    "CLOSE_WAIT"},
        {en::TcpState::Closing,      "CLOSING"},
        {en::TcpState::LastAck,      "LAST_ACK"},
        {en::TcpState::TimeWait,     "TIME_WAIT"},
    };
    for (auto& c : cases) {
        auto* n = en::tcp_state_name(c.s);
        ASSERT_NE(n, nullptr);
        EXPECT_STREQ(n, c.name);
    }
}

TEST(TcpState, NameForUnknownValue) {
    // Values outside the defined range must not crash; they should return
    // "UNKNOWN" so logging stays structured even under ABI drift.
    auto bad = static_cast<en::TcpState>(200);
    EXPECT_STREQ(en::tcp_state_name(bad), "UNKNOWN");
}

TEST(TcpState, NamesAreUnique) {
    // Defense against typos in the name table.
    constexpr en::TcpState all[] = {
        en::TcpState::Closed, en::TcpState::Listen, en::TcpState::SynSent,
        en::TcpState::SynReceived, en::TcpState::Established,
        en::TcpState::FinWait1, en::TcpState::FinWait2, en::TcpState::CloseWait,
        en::TcpState::Closing, en::TcpState::LastAck, en::TcpState::TimeWait,
    };
    for (size_t i = 0; i < std::size(all); ++i) {
        for (size_t j = i + 1; j < std::size(all); ++j) {
            EXPECT_STRNE(en::tcp_state_name(all[i]), en::tcp_state_name(all[j]))
                << "duplicate name at i=" << i << ", j=" << j;
        }
    }
}
