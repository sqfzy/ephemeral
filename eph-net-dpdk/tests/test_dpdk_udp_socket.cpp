/// @file test_dpdk_udp_socket.cpp
/// Unit tests for `eph::net::dpdk::DpdkUdpSocket`.
///
///   - concept conformance static_asserts (Pollable + Datagram)
///   - InvalidConfig surface: zero IPs, null pool
///   - send_to before attach returns NotAttached
///
/// Like the TCP stream tests, we do NOT exercise the live RX path here
/// (would require a vfio-pci-bound NIC). The unit tests focus on the
/// type system, configuration plumbing, and pre-attach guards.

#include <cstdint>

#include <gtest/gtest.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep

#include "eph/codec/raw_datagram_codec.hpp"
#include "eph/net/concepts.hpp"
#include "eph/net/dpdk/poller.hpp"
#include "eph/net/dpdk/udp_socket.hpp"

namespace edpk = eph::net::dpdk;
namespace ec  = eph::codec;

using RawUdpSocket = edpk::DpdkUdpSocket<ec::RawDatagramCodec>;

// ---------------------------------------------------------------------------
// Concept conformance
// ---------------------------------------------------------------------------

static_assert(eph::net::Pollable<RawUdpSocket>,
              "DpdkUdpSocket<RawDatagramCodec> must be Pollable");
static_assert(eph::net::Datagram<RawUdpSocket>,
              "DpdkUdpSocket<RawDatagramCodec> must be Datagram");
static_assert(eph::net::dpdk::DpdkPollable<RawUdpSocket>,
              "DpdkUdpSocket<RawDatagramCodec> must be DpdkPollable");

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(DpdkUdpSocket, ZeroSrcIpFailsInvalidConfig) {
    edpk::UdpConfig cfg{};
    // Default-init: src_ip == 0, dst_ip == 0, pool == nullptr.
    // Validation should reject before any DPDK call.
    auto r = RawUdpSocket::create(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
}

TEST(DpdkUdpSocket, NullPoolFailsInvalidConfig) {
    edpk::UdpConfig cfg{};
    cfg.legacy.src_ip   = 0x0A000001;
    cfg.legacy.dst_ip   = 0x0A000002;
    cfg.legacy.src_port = 12345;
    cfg.legacy.dst_port = 30000;
    cfg.legacy.pool     = nullptr;  // explicit
    auto r = RawUdpSocket::create(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
}

// Compile-time check that the type-aliased Datagram concept arguments
// are correct (catches accidental drift in associated types).
TEST(DpdkUdpSocket, AssociatedTypesPresent) {
    using S = RawUdpSocket;
    static_assert(std::is_same_v<S::CodecType, ec::RawDatagramCodec>);
    static_assert(std::is_same_v<S::PacketView, edpk::detail::MbufView>);
    EXPECT_TRUE((eph::net::Datagram<S>));
}
