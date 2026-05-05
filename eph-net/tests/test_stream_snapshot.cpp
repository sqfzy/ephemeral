/// @file test_stream_snapshot.cpp
/// Unit tests for `eph::net::StreamSnapshot` defaults & shape.
///
/// `StreamSnapshot` is the cross-backend post-create state struct
/// returned by `Stream::snapshot()` / `Datagram::snapshot()`. Both
/// kernel and DPDK backends populate the same struct, and a contract
/// of the design is that backend-specific fields (e.g. `Dpdk::rx_queue`)
/// are zero-filled on backends where they don't apply, so cross-backend
/// diagnostic code never has to branch.
///
/// These tests pin the default-constructed shape so a future field
/// addition that's not zero-initialised (e.g. `int field = -1` instead
/// of zero, or a non-trivially-default-constructed sub-struct) trips
/// here rather than as a runtime "weird zero/nonzero" surprise inside
/// a backend's snapshot population.

#include <gtest/gtest.h>

#include <chrono>
#include <type_traits>

#include "eph/net/stream_snapshot.hpp"

using eph::net::StreamSnapshot;

// ---------------------------------------------------------------------------
// Default-constructed: every field is the documented "absent" sentinel
// ---------------------------------------------------------------------------

TEST(StreamSnapshot, EndpointDefaultsAllZero) {
    StreamSnapshot s{};
    EXPECT_EQ(s.endpoint.src_ip, 0u);
    EXPECT_EQ(s.endpoint.src_port, 0u);
    EXPECT_EQ(s.endpoint.dst_ip, 0u);
    EXPECT_EQ(s.endpoint.dst_port, 0u);
    EXPECT_FALSE(s.endpoint.src_port_rewritten);
}

TEST(StreamSnapshot, TcpDefaultsInert) {
    StreamSnapshot s{};
    // tcp.enabled=false is the UDP-socket sentinel; every other tcp.*
    // field is meaningless when enabled=false. Pin them anyway so a
    // future caller cannot misread an uninitialised value as a real
    // negotiation state.
    EXPECT_FALSE(s.tcp.enabled);
    EXPECT_EQ(s.tcp.recv_window, 0u);
    EXPECT_EQ(s.tcp.local_mss, 0u);
    EXPECT_EQ(s.tcp.peer_mss, 0u);
    EXPECT_EQ(s.tcp.effective_mss, 0u);
    EXPECT_FALSE(s.tcp.peer_mss_negotiated);
    EXPECT_FALSE(s.tcp.icmp_pmtu_shrunk);
}

TEST(StreamSnapshot, KeepaliveDefaultsDisabled) {
    StreamSnapshot s{};
    EXPECT_FALSE(s.keepalive.active);
    EXPECT_EQ(s.keepalive.interval, std::chrono::milliseconds{0});
    EXPECT_EQ(s.keepalive.probes, 0u);
}

TEST(StreamSnapshot, TlsDefaultsDisabled) {
    StreamSnapshot s{};
    EXPECT_FALSE(s.tls.enabled);
    EXPECT_FALSE(s.tls.was_resumed);
    EXPECT_FALSE(s.tls.send_desynced);
    EXPECT_TRUE(s.tls.sni.empty());
}

TEST(StreamSnapshot, WsDefaultsDisabled) {
    StreamSnapshot s{};
    EXPECT_FALSE(s.ws.enabled);
    EXPECT_TRUE(s.ws.path.empty());
    EXPECT_TRUE(s.ws.host.empty());
    EXPECT_FALSE(s.ws.permessage_deflate_active);
}

TEST(StreamSnapshot, DpdkDefaultsZeroExceptPoolLcoreHintMinusOne) {
    // pool_lcore_hint_resolved defaults to -1 ("single shared pool"
    // sentinel). Every other Dpdk field is 0 / nullopt by design.
    StreamSnapshot s{};
    EXPECT_EQ(s.dpdk.rx_queue, 0u);
    EXPECT_EQ(s.dpdk.tx_queue, 0u);
    EXPECT_EQ(s.dpdk.pool_lcore_hint_resolved, -1)
        << "pool_lcore_hint_resolved sentinel must default to -1, "
           "matching the Dpdk struct's documented contract";
    EXPECT_FALSE(s.dpdk.flow_rule_handle.has_value());
}

// ---------------------------------------------------------------------------
// Type-shape invariants — pin so refactors don't accidentally bloat the
// struct or introduce non-trivial types into the snapshot path.
// ---------------------------------------------------------------------------

TEST(StreamSnapshot, IsCopyableAndDestructible) {
    // The header docs say "cheap to copy (~120 bytes); not on any hot
    // path". Pin trivial-style traits where the field types allow.
    static_assert(std::is_default_constructible_v<StreamSnapshot>);
    static_assert(std::is_copy_constructible_v<StreamSnapshot>);
    static_assert(std::is_copy_assignable_v<StreamSnapshot>);
    static_assert(std::is_nothrow_default_constructible_v<StreamSnapshot>);
    static_assert(std::is_nothrow_destructible_v<StreamSnapshot>);
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Independent sub-struct construction — verify each Tcp/Tls/Ws/Keepalive/
// Dpdk piece can be aggregate-initialised in isolation. This is the
// pattern backends use to populate the snapshot piece-by-piece.
// ---------------------------------------------------------------------------

TEST(StreamSnapshot, AggregateInitWorksForSubStructs) {
    StreamSnapshot s{};
    s.endpoint.src_ip   = 0x0A000001;
    s.endpoint.src_port = 12345;
    s.endpoint.dst_ip   = 0x0A000002;
    s.endpoint.dst_port = 443;
    s.tls.enabled = true;
    s.tls.sni = "example.com";
    s.ws.enabled = true;
    s.ws.path = "/ws/btcusdt";

    EXPECT_EQ(s.endpoint.src_ip, 0x0A000001u);
    EXPECT_EQ(s.endpoint.dst_port, 443u);
    EXPECT_TRUE(s.tls.enabled);
    EXPECT_EQ(s.tls.sni, "example.com");
    EXPECT_TRUE(s.ws.enabled);
    EXPECT_EQ(s.ws.path, "/ws/btcusdt");
    // Untouched sub-struct still inert.
    EXPECT_FALSE(s.tcp.enabled);
    EXPECT_FALSE(s.keepalive.active);
    EXPECT_EQ(s.dpdk.rx_queue, 0u);
}
