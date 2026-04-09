/// @file tests/unit/bench/test_tsc_protocol.cpp
/// Unit tests for the binary + JSON TSC stamp protocol.

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "core/tsc_protocol.hpp"

using namespace bench;

// ── binary stamp / parse round-trip ──────────────────────────────────────
TEST(BenchTscProtocolBinary, RoundTripPreservesAllThreeStamps) {
    std::array<uint8_t, 64> buf{};
    constexpr uint64_t cs = 0x0123456789ABCDEFULL;
    constexpr uint64_t sr = 0xFEDCBA9876543210ULL;
    constexpr uint64_t ss = 0x1122334455667788ULL;

    tsc::stamp_binary(buf.data(), cs, sr, ss);

    uint64_t cs2 = 0, sr2 = 0, ss2 = 0;
    tsc::parse_binary(buf.data(), cs2, sr2, ss2);
    EXPECT_EQ(cs2, cs);
    EXPECT_EQ(sr2, sr);
    EXPECT_EQ(ss2, ss);
}

TEST(BenchTscProtocolBinary, HeaderSizeIs24) {
    EXPECT_EQ(tsc::kBinaryHeaderSize, 24u);
}

TEST(BenchTscProtocolBinary, ZeroIsValidValue) {
    std::array<uint8_t, 32> buf{};
    tsc::stamp_binary(buf.data(), 0, 0, 0);
    uint64_t a = 1, b = 1, c = 1;
    tsc::parse_binary(buf.data(), a, b, c);
    EXPECT_EQ(a, 0u);
    EXPECT_EQ(b, 0u);
    EXPECT_EQ(c, 0u);
}

// ── JSON parsing ─────────────────────────────────────────────────────────

TEST(BenchTscProtocolJson, ParseTAtStartOfObject) {
    std::string json = R"({"T":12345,"s":"BTCUSDT"})";
    auto v = tsc::parse_T(reinterpret_cast<const uint8_t*>(json.data()), json.size());
    EXPECT_EQ(v, 12345u);
}

TEST(BenchTscProtocolJson, ParseTInMiddleOfObject) {
    std::string json = R"({"s":"BTCUSDT","T":67890,"p":"50000"})";
    auto v = tsc::parse_T(reinterpret_cast<const uint8_t*>(json.data()), json.size());
    EXPECT_EQ(v, 67890u);
}

TEST(BenchTscProtocolJson, ParseTDoesNotMatchTRecv) {
    // Plain "T_recv" is the only T-prefixed field — parse_T must NOT pick it.
    std::string json = R"({"T_recv":111,"x":1})";
    auto v = tsc::parse_T(reinterpret_cast<const uint8_t*>(json.data()), json.size());
    EXPECT_EQ(v, 0u);
}

TEST(BenchTscProtocolJson, ParseTDoesNotMatchTSend) {
    std::string json = R"({"T_send":222,"x":1})";
    auto v = tsc::parse_T(reinterpret_cast<const uint8_t*>(json.data()), json.size());
    EXPECT_EQ(v, 0u);
}

TEST(BenchTscProtocolJson, ParseTRecvAndTSendCoexist) {
    std::string json = R"({"T":1,"T_recv":2,"T_send":3})";
    auto t  = tsc::parse_T(reinterpret_cast<const uint8_t*>(json.data()), json.size());
    auto tr = tsc::parse_T_recv(reinterpret_cast<const uint8_t*>(json.data()), json.size());
    auto ts = tsc::parse_T_send(reinterpret_cast<const uint8_t*>(json.data()), json.size());
    EXPECT_EQ(t,  1u);
    EXPECT_EQ(tr, 2u);
    EXPECT_EQ(ts, 3u);
}

TEST(BenchTscProtocolJson, MissingFieldReturnsZero) {
    std::string json = R"({"x":1})";
    EXPECT_EQ(tsc::parse_T(reinterpret_cast<const uint8_t*>(json.data()), json.size()), 0u);
    EXPECT_EQ(tsc::parse_T_recv(reinterpret_cast<const uint8_t*>(json.data()), json.size()), 0u);
    EXPECT_EQ(tsc::parse_T_send(reinterpret_cast<const uint8_t*>(json.data()), json.size()), 0u);
}

TEST(BenchTscProtocolJson, LargeUint64Value) {
    std::string json = R"({"T":18446744073709551000})";
    auto v = tsc::parse_T(reinterpret_cast<const uint8_t*>(json.data()), json.size());
    EXPECT_EQ(v, 18446744073709551000ULL);
}
