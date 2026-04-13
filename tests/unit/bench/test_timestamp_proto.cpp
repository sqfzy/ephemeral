/// @file tests/unit/bench/test_timestamp_proto.cpp
/// Unit tests for the bench timestamp-block protocol
/// (`benchmarks/latency/core/timestamp_proto.hpp`).
///
/// Three cases:
///   1. roundtrip_24_bytes         — write_client_ts zero-pads [8:24] and
///                                    read_ts returns client_ns intact
///   2. compute_legs_basic          — known TS block + recv_ns → known legs
///   3. mock_overwrite_preserves_client — simulate mock overwriting bytes
///                                        [8:24] and ensure client_ns at
///                                        [0:8] survives unchanged
///
/// The header is pure-function and noexcept; we only need to lock in the
/// memcpy layout + subtraction math that the scenarios rely on.

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

#include <gtest/gtest.h>

#include "core/timestamp_proto.hpp"

namespace {

TEST(TimestampProto, RoundtripTwentyFourBytes) {
    std::array<uint8_t, 64> buf{};
    // Fill with a sentinel so we can tell which bytes we wrote. write_
    // client_ts must zero bytes [8:24] and leave [24:] alone.
    std::memset(buf.data(), 0xCC, buf.size());

    const uint64_t t0 = 0x0123'4567'89AB'CDEFull;
    bench::write_client_ts(
        std::span<uint8_t>{buf.data(), bench::kTimestampBlockSize}, t0);

    // Read back — client_ns should equal t0; other two zeros.
    auto ts = bench::read_ts(
        std::span<const uint8_t>{buf.data(), bench::kTimestampBlockSize});
    EXPECT_EQ(ts.client_ns, t0);
    EXPECT_EQ(ts.mock_recv_ns, 0u);
    EXPECT_EQ(ts.mock_send_ns, 0u);

    // Bytes [24:] must be untouched sentinel.
    for (std::size_t i = bench::kTimestampBlockSize; i < buf.size(); ++i) {
        EXPECT_EQ(buf[i], 0xCCu) << "byte " << i << " was clobbered";
    }
}

TEST(TimestampProto, ComputeLegsBasic) {
    // Construct a known TimestampBlock and a known recv time. Pick
    // integer values that produce distinct non-zero legs so a bug in
    // any one of the three subtractions is caught.
    const bench::TimestampBlock ts{
        .client_ns = 1'000'000,
        .mock_recv_ns = 1'003'000,  // TX leg = 3000 ns
        .mock_send_ns = 1'004'500,  // srv gap = 1500 ns (not recorded)
    };
    const uint64_t recv_ns = 1'010'000;  // RX leg = 5500 ns, RTT = 10000 ns

    const auto lg = bench::compute_legs(ts, recv_ns);
    EXPECT_EQ(lg.rtt_ns, 10'000u);
    EXPECT_EQ(lg.tx_ns,   3'000u);
    EXPECT_EQ(lg.rx_ns,   5'500u);
    // Sanity: tx + srv_gap + rx should equal rtt.
    EXPECT_EQ(lg.tx_ns + 1500u + lg.rx_ns, lg.rtt_ns);
}

TEST(TimestampProto, MockOverwritePreservesClient) {
    // Simulate the wire protocol: client writes its ts, sends, mock
    // overwrites bytes [8:24] with its two timestamps, then client
    // reads all three back. The client_ns field at [0:8] must be
    // bit-identical to what the client wrote.
    std::array<uint8_t, 24> wire{};
    const uint64_t t_client = 0xDEAD'BEEF'CAFE'BABEull;
    bench::write_client_ts(
        std::span<uint8_t>{wire.data(), wire.size()}, t_client);

    // Mock side: overwrite bytes [8:24].
    const uint64_t t_recv = 0x1111'2222'3333'4444ull;
    const uint64_t t_send = 0x5555'6666'7777'8888ull;
    std::memcpy(wire.data() + 8,  &t_recv, sizeof(t_recv));
    std::memcpy(wire.data() + 16, &t_send, sizeof(t_send));

    // Client side: read back all three.
    auto ts = bench::read_ts(
        std::span<const uint8_t>{wire.data(), wire.size()});
    EXPECT_EQ(ts.client_ns,    t_client)
        << "mock overwrite of [8:24] must not touch [0:8]";
    EXPECT_EQ(ts.mock_recv_ns, t_recv);
    EXPECT_EQ(ts.mock_send_ns, t_send);
}

} // namespace
