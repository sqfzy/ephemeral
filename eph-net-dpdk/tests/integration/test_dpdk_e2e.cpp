/// @file test_dpdk_e2e.cpp
/// End-to-end integration tests for the DPDK datapath.
///
/// Layout: this single binary contains all P0+P1 test cases (TCP, UDP,
/// WS, RST, FIN, RxDispatcher, ARP, DNS) so that EAL is initialized exactly once
/// and the kernel mock dispatcher is forked exactly once for the binary's
/// entire lifetime.  See plan-dpdk-integration-tests-20260410-053355.md
/// for design rationale.
///
/// Network requirements (per bench.conf):
///   * NIC_A bound to host kernel, has SERVER_IP — kernel mocks bind here
///   * NIC_B bound to vfio-pci, no kernel IP — DPDK opens it directly
///
/// If NIC_B is not on vfio-pci at startup, all tests are SKIPPED with a
/// helpful diagnostic.  Run `sudo benchmarks/latency/lat tcp --dpdk` once
/// to transition NIC_B before running this binary.

#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include "dpdk_e2e_env.hpp"
#include "mock_dispatcher.hpp"

#include "eph/dpdk/tcp.hpp"
#include "eph/dpdk/udp.hpp"
#include "eph/dpdk/arp.hpp"
#include "eph/dpdk/dns.hpp"
#include "eph/dpdk/rx_dispatcher.hpp"

using namespace eph::dpdk::test_e2e;
using namespace std::chrono_literals;

namespace {

/// Per-test rotating source-port allocator.  TIME_WAIT on the kernel side
/// (NIC_A) means re-using the same {src_ip, src_port} immediately after a
/// previous TCP teardown will be rejected by the kernel mock with RST.
/// Tests rotate to fresh ports.
inline uint16_t next_src_port() {
    static std::atomic<uint16_t> counter{40000};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

/// Receive exactly `expected` bytes from a TcpSession into `out`, with a
/// 2-second hard deadline.  Returns true on success.
template <typename Session>
bool tcp_recv_exact(Session& s, std::vector<uint8_t>& out, size_t expected) {
    out.assign(expected, 0);
    size_t got = 0;
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (got < expected && std::chrono::steady_clock::now() < deadline) {
        auto r = s.poll_rx([&](const uint8_t* data, uint16_t len) {
            size_t take = std::min(static_cast<size_t>(len), expected - got);
            std::memcpy(out.data() + got, data, take);
            got += take;
        });
        if (!r) return false;
    }
    return got == expected;
}

/// Send exactly `len` bytes through a TcpSession, chunking by MSS.
template <typename Session>
bool tcp_send_all(Session& s, const uint8_t* data, size_t len) {
    size_t sent = 0;
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (sent < len && std::chrono::steady_clock::now() < deadline) {
        size_t chunk = std::min(len - sent, size_t{1460});
        auto r = s.send(data + sent, static_cast<uint16_t>(chunk));
        if (!r) return false;
        sent += *r;
    }
    return sent == len;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// TcpE2E — DPDK TcpSession over real NIC against kernel echo mock
// ═══════════════════════════════════════════════════════════════════════

TEST(TcpE2E, SmokeEcho) {
    EPH_DPDK_E2E_SKIP_IF_NOT_READY();

    auto& env = DpdkE2ETestEnv::env();

    // Sweep four payload sizes through one connection-per-size.
    constexpr size_t kSizes[] = {64, 256, 1024, 4096};
    for (size_t sz : kSizes) {
        SCOPED_TRACE("payload size: " + std::to_string(sz));

        auto tcfg = env.make_tcp_config(next_src_port(), kTcpEchoPort);
        eph::dpdk::TcpSession<> session(tcfg, env.pool);

        ASSERT_TRUE(session.connect(3s).has_value())
            << "TCP connect to mock failed";

        // Build payload — fill with size-derived pattern.
        std::vector<uint8_t> payload(sz);
        for (size_t i = 0; i < sz; ++i) {
            payload[i] = static_cast<uint8_t>((sz + i) & 0xFF);
        }

        ASSERT_TRUE(tcp_send_all(session, payload.data(), payload.size()));

        std::vector<uint8_t> got;
        ASSERT_TRUE(tcp_recv_exact(session, got, sz));
        EXPECT_EQ(got, payload);

        EXPECT_TRUE(session.close().has_value());
    }
}

// ═══════════════════════════════════════════════════════════════════════
// UdpE2E — DPDK UdpSender over real NIC against kernel UDP echo mock
// ═══════════════════════════════════════════════════════════════════════

TEST(UdpE2E, BurstEcho) {
    EPH_DPDK_E2E_SKIP_IF_NOT_READY();

    auto& env = DpdkE2ETestEnv::env();

    auto sender_r = env.make_udp_sender(next_src_port(), kUdpEchoPort);
    ASSERT_TRUE(sender_r.has_value()) << sender_r.error();
    auto& sender = *sender_r;

    // Send 16 packets of varying size and verify all sends report success.
    // RX verification uses rte_eth_rx_burst directly since UdpSender has
    // no recv API (its only job is TX with a precomputed packet template).
    constexpr int kPackets = 16;
    int tx_ok = 0;
    for (int i = 0; i < kPackets; ++i) {
        size_t sz = 64 + i * 32;
        std::vector<uint8_t> p(sz);
        for (size_t j = 0; j < sz; ++j) p[j] = static_cast<uint8_t>((i + j) & 0xFF);
        // UdpSender::send returns bool (not expected) — true on TX burst success.
        if (sender.send(p.data(), static_cast<uint16_t>(sz))) {
            ++tx_ok;
        }
    }
    EXPECT_EQ(tx_ok, kPackets) << "some UDP sends failed";

    // Best-effort RX verification: drain rte_eth_rx_burst for ~500ms,
    // count UDP packets we receive on our local port.  We don't parse
    // payloads here — full datapath parsing is exercised by the TCP test.
    int rx_count = 0;
    auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (rx_count < kPackets && std::chrono::steady_clock::now() < deadline) {
        constexpr uint16_t kBurst = 32;
        rte_mbuf* pkts[kBurst];
        uint16_t n = rte_eth_rx_burst(env.port_id, /*rx_queue=*/0, pkts, kBurst);
        for (uint16_t i = 0; i < n; ++i) {
            // Heuristic: count any IPv4 UDP packet as an echo.
            // We don't strictly validate the 4-tuple here; the next test
            // will use TcpSession's full packet_parse path.
            ++rx_count;
            rte_pktmbuf_free(pkts[i]);
        }
    }
    // We don't fail on partial RX — kernel UDP can drop in flight.  But
    // we expect at least *some* round trip to succeed if the path is alive.
    EXPECT_GT(rx_count, 0)
        << "no UDP packets received from kernel echo within 500ms — "
        << "check NIC_A is up and SERVER_IP responds to UDP "
        << kUdpEchoPort;
}

// ═══════════════════════════════════════════════════════════════════════
// WsE2E — RFC 6455 handshake + accept-hash verification over DPDK
//
// Drives the WS upgrade through a raw eph::dpdk::TcpSession against the
// kernel ws_echo_mock.  This validates the wire-level path (DPDK packet
// I/O + HTTP upgrade exchange + RFC 6455 §1.3 sample accept hash).
//
// A full eph::net::DirectTransport<TcpSession, WsFramer> orchestration
// (TransportConfig + callback set + frame echo) is intentionally a
// separate test that doesn't fit in a smoke pass.
// ═══════════════════════════════════════════════════════════════════════

TEST(WsE2E, HandshakeAndEcho) {
    EPH_DPDK_E2E_SKIP_IF_NOT_READY();
    auto& env = DpdkE2ETestEnv::env();

    auto tcfg = env.make_tcp_config(next_src_port(), kWsEchoPort);
    eph::dpdk::TcpSession<> session(tcfg, env.pool);
    ASSERT_TRUE(session.connect(3s).has_value()) << "TCP connect to ws_echo_mock failed";

    // Send a minimal HTTP/1.1 GET upgrade request.  The mock validates
    // Sec-WebSocket-Key and replies with the matching accept.
    constexpr const char kReq[] =
        "GET / HTTP/1.1\r\n"
        "Host: server\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    ASSERT_TRUE(tcp_send_all(session,
                              reinterpret_cast<const uint8_t*>(kReq),
                              std::strlen(kReq)));

    // Read the upgrade response.  Expect "101" status and the precomputed
    // accept value for the RFC 6455 §1.3 sample key.
    std::string resp;
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (resp.find("\r\n\r\n") == std::string::npos &&
           std::chrono::steady_clock::now() < deadline) {
        auto r = session.poll_rx([&](const uint8_t* data, uint16_t len) {
            resp.append(reinterpret_cast<const char*>(data), len);
        });
        if (!r) break;
    }
    ASSERT_NE(resp.find("HTTP/1.1 101"), std::string::npos)
        << "WS handshake response missing 101: " << resp;
    EXPECT_NE(resp.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), std::string::npos)
        << "WS accept hash mismatch: " << resp;

    EXPECT_TRUE(session.close().has_value());
}

// ═══════════════════════════════════════════════════════════════════════
// FailureE2E — RST handling
// ═══════════════════════════════════════════════════════════════════════

TEST(FailureE2E, PeerRstAfterAccept) {
    EPH_DPDK_E2E_SKIP_IF_NOT_READY();
    auto& env = DpdkE2ETestEnv::env();

    auto tcfg = env.make_tcp_config(next_src_port(), kTcpRstPort);
    eph::dpdk::TcpSession<> session(tcfg, env.pool);
    ASSERT_TRUE(session.connect(3s).has_value());

    // After connect succeeds, the mock's accept handler immediately
    // closes the socket with SO_LINGER {1, 0} — the kernel sends RST.
    // Drive poll_rx until we observe the state transition.  The session
    // should report an error within ~1 second.
    bool got_error = false;
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto r = session.poll_rx([](const uint8_t*, uint16_t) {});
        if (!r) { got_error = true; break; }
        // Some impls return success but transition state; check explicitly.
        if (session.state() == eph::net::TcpState::Closed) {
            got_error = true;
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_TRUE(got_error) << "TcpSession did not detect peer RST within deadline";
}

// ═══════════════════════════════════════════════════════════════════════
// FailureE2E — Graceful FIN handling
// ═══════════════════════════════════════════════════════════════════════

TEST(FailureE2E, PeerFinAfterEcho) {
    EPH_DPDK_E2E_SKIP_IF_NOT_READY();
    auto& env = DpdkE2ETestEnv::env();

    auto tcfg = env.make_tcp_config(next_src_port(), kTcpFinPort);
    eph::dpdk::TcpSession<> session(tcfg, env.pool);
    ASSERT_TRUE(session.connect(3s).has_value());

    // Send one chunk and read the echo back.
    const std::string payload = "hello-fin-mock";
    ASSERT_TRUE(tcp_send_all(session,
        reinterpret_cast<const uint8_t*>(payload.data()),
        payload.size()));

    std::vector<uint8_t> got;
    ASSERT_TRUE(tcp_recv_exact(session, got, payload.size()));
    EXPECT_EQ(std::string(got.begin(), got.end()), payload);

    // Now the mock has called shutdown(SHUT_WR) — drive poll_rx until we
    // observe the FIN-induced state transition (CloseWait or beyond).
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)session.poll_rx([](const uint8_t*, uint16_t) {});
        auto st = session.state();
        if (st == eph::net::TcpState::CloseWait ||
            st == eph::net::TcpState::LastAck   ||
            st == eph::net::TcpState::Closed) {
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    auto final_state = session.state();
    EXPECT_TRUE(final_state == eph::net::TcpState::CloseWait ||
                final_state == eph::net::TcpState::LastAck   ||
                final_state == eph::net::TcpState::Closed)
        << "Session did not transition past Established after peer FIN; state="
        << eph::net::tcp_state_name(final_state);

    // Reciprocate FIN — should complete the close handshake.
    EXPECT_TRUE(session.close().has_value());
}

// ═══════════════════════════════════════════════════════════════════════
// ReactorE2E — multi-connection concurrent echo via the actual RxDispatcher
//
// This test validates eph::dpdk::RxDispatcher's connection multiplexer.  We
// CANNOT just spin N TcpSessions and round-robin poll_rx them: each
// poll_rx call drains the shared rx_queue and DROPS packets that don't
// match its 4-tuple.  With N sessions sharing one queue that's fatally
// destructive to N-1 of them per poll cycle.
//
// The RxDispatcher class is the canonical solution: a single dedicated RX
// thread polls the NIC and dispatches each packet to the matching
// session via on_data callback.
// ═══════════════════════════════════════════════════════════════════════

TEST(RxDispatcherE2E, MultiConnFairness) {
    EPH_DPDK_E2E_SKIP_IF_NOT_READY();
    auto& env = DpdkE2ETestEnv::env();

    // 1. Connect kRxDispatcherConns sessions ONE AT A TIME.  Each connect
    //    uses TcpSession::poll_rx internally, so they must serialize.
    std::vector<std::unique_ptr<eph::dpdk::TcpSession<>>> sessions;
    sessions.reserve(kRxDispatcherConns);
    for (int i = 0; i < kRxDispatcherConns; ++i) {
        auto tcfg = env.make_tcp_config(
            next_src_port(),
            static_cast<uint16_t>(kRxDispatcherPortBase + i));
        auto s = std::make_unique<eph::dpdk::TcpSession<>>(tcfg, env.pool);
        auto cr = s->connect(3s);
        ASSERT_TRUE(cr.has_value())
            << "session " << i << " connect failed: " << cr.error();
        sessions.push_back(std::move(s));
    }

    // 2. Hand all sessions to a RxDispatcher with per-session on_data
    //    callbacks pushing into per-session receive buffers.
    eph::dpdk::RxDispatcherConfig rcfg{};
    rcfg.port_id = env.port_id;
    rcfg.rx_queue_id = 0;
    rcfg.rx_cpu = -1;
    eph::dpdk::RxDispatcher<> reactor(rcfg);

    const size_t kPayload = 256;
    std::vector<std::vector<uint8_t>> recv_buf(kRxDispatcherConns);
    std::vector<std::atomic<size_t>> received(kRxDispatcherConns);
    for (auto& a : received) a.store(0);
    for (auto& v : recv_buf) v.assign(kPayload, 0);

    for (int i = 0; i < kRxDispatcherConns; ++i) {
        auto add_r = reactor.add_connection(sessions[i].get(),
            [i, &recv_buf, &received, kPayload]
            (const uint8_t* d, uint16_t l, size_t /*conn_id*/) {
                size_t cur = received[i].load(std::memory_order_relaxed);
                size_t take = std::min(static_cast<size_t>(l), kPayload - cur);
                if (take > 0) {
                    std::memcpy(recv_buf[i].data() + cur, d, take);
                    received[i].fetch_add(take, std::memory_order_release);
                }
            });
        ASSERT_TRUE(add_r.has_value())
            << "reactor.add_connection " << i << " failed: " << add_r.error();
    }

    ASSERT_TRUE(reactor.start()) << "reactor.start() returned false";

    // 3. Send a distinct 256B payload through each session.  TX is
    //    per-session and remains the test thread's responsibility.
    std::vector<std::vector<uint8_t>> sent_payloads(kRxDispatcherConns);
    for (int i = 0; i < kRxDispatcherConns; ++i) {
        sent_payloads[i].resize(kPayload);
        for (size_t j = 0; j < kPayload; ++j) {
            sent_payloads[i][j] = static_cast<uint8_t>((i * 7 + j) & 0xFF);
        }
        ASSERT_TRUE(tcp_send_all(*sessions[i],
                                  sent_payloads[i].data(), kPayload))
            << "session " << i << " send failed";
    }

    // 4. Wait for all callbacks to fill their buffers.
    auto deadline = std::chrono::steady_clock::now() + 5s;
    bool all_done = false;
    while (!all_done && std::chrono::steady_clock::now() < deadline) {
        all_done = true;
        for (int i = 0; i < kRxDispatcherConns; ++i) {
            if (received[i].load(std::memory_order_acquire) < kPayload) {
                all_done = false;
                break;
            }
        }
        if (!all_done) std::this_thread::sleep_for(5ms);
    }

    reactor.stop();

    for (int i = 0; i < kRxDispatcherConns; ++i) {
        EXPECT_EQ(received[i].load(), kPayload)
            << "session " << i << " incomplete";
        EXPECT_EQ(recv_buf[i], sent_payloads[i])
            << "session " << i << " payload mismatch";
        (void)sessions[i]->close();
    }
}

// ═══════════════════════════════════════════════════════════════════════
// ArpE2E — gateway resolution against kernel ARP responder
// ═══════════════════════════════════════════════════════════════════════

TEST(ArpE2E, ResolveGateway) {
    EPH_DPDK_E2E_SKIP_IF_NOT_READY();
    auto& env = DpdkE2ETestEnv::env();

    // The fixture's create_full() already ARP-resolved the gateway as part
    // of EAL bring-up.  Verify we can re-resolve and get the SAME MAC,
    // proving the kernel ARP responder on the upstream hop is consistent
    // and the parser logic is deterministic.
    auto r = eph::dpdk::arp::resolve(
        env.port_id, /*rx_queue=*/0, env.pool,
        env.src_mac, env.src_ip, env.gw_ip,
        std::chrono::seconds{3});
    ASSERT_TRUE(r.has_value()) << "second ARP resolve failed: " << r.error();
    EXPECT_EQ(0, std::memcmp(&*r, &env.gw_mac, sizeof(rte_ether_addr)))
        << "ARP resolve returned different gateway MAC than fixture init";
}

// ═══════════════════════════════════════════════════════════════════════
// DnsE2E — DPDK dns::resolve over real NIC against kernel DNS mock
// ═══════════════════════════════════════════════════════════════════════
//
// The mock dispatcher binds a UDP responder on (SERVER_IP, kDnsMockPort)
// that answers any A query with kDnsMockResolvedIp.  The DPDK client
// frames DNS over UDP/IPv4/Ethernet and ships it through NIC_B; the
// kernel routes it back via NIC_A to the mock just like the TCP/UDP
// echo paths.

namespace {
inline eph::dpdk::dns::DnsConfig make_dns_mock_cfg(
    uint32_t server_ip,
    std::chrono::milliseconds timeout = std::chrono::seconds{3}) {
    eph::dpdk::dns::DnsConfig cfg{};
    cfg.nameserver_ip = server_ip;
    cfg.port          = kDnsMockPort;
    cfg.timeout       = timeout;
    return cfg;
}
} // namespace

TEST(DnsE2E, ResolveHostname) {
    EPH_DPDK_E2E_SKIP_IF_NOT_READY();
    auto& env = DpdkE2ETestEnv::env();

    auto cfg = make_dns_mock_cfg(env.dst_ip);
    auto r = eph::dpdk::dns::resolve(
        env.port_id, /*queue_id=*/0, env.pool,
        env.src_mac, env.gw_mac, env.src_ip,
        "example.com", cfg);

    ASSERT_TRUE(r.has_value()) << "DNS resolve failed: " << r.error();
    EXPECT_EQ(*r, kDnsMockResolvedIp)
        << "resolved IP mismatch: got 0x" << std::hex << *r
        << ", expected 0x" << kDnsMockResolvedIp;
}

TEST(DnsE2E, ResolveTwoDifferentHostnamesInSequence) {
    EPH_DPDK_E2E_SKIP_IF_NOT_READY();
    auto& env = DpdkE2ETestEnv::env();
    auto cfg = make_dns_mock_cfg(env.dst_ip);

    // Two back-to-back resolves with different hostnames must each
    // succeed: a fresh tx_id is generated per call and any leftover
    // state in the recv loop must not bleed across calls.
    auto r1 = eph::dpdk::dns::resolve(
        env.port_id, 0, env.pool,
        env.src_mac, env.gw_mac, env.src_ip, "first.test", cfg);
    ASSERT_TRUE(r1.has_value()) << "first resolve failed: " << r1.error();
    EXPECT_EQ(*r1, kDnsMockResolvedIp);

    auto r2 = eph::dpdk::dns::resolve(
        env.port_id, 0, env.pool,
        env.src_mac, env.gw_mac, env.src_ip, "second.different.test", cfg);
    ASSERT_TRUE(r2.has_value()) << "second resolve failed: " << r2.error();
    EXPECT_EQ(*r2, kDnsMockResolvedIp);
}

TEST(DnsE2E, EmptyHostnameReturnsError) {
    EPH_DPDK_E2E_SKIP_IF_NOT_READY();
    auto& env = DpdkE2ETestEnv::env();
    auto cfg = make_dns_mock_cfg(env.dst_ip);

    auto r = eph::dpdk::dns::resolve(
        env.port_id, 0, env.pool,
        env.src_mac, env.gw_mac, env.src_ip, "", cfg);

    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("hostname is empty"), std::string::npos)
        << "expected 'hostname is empty' diagnostic, got: " << r.error();
}

TEST(DnsE2E, NullPoolReturnsError) {
    EPH_DPDK_E2E_SKIP_IF_NOT_READY();
    auto& env = DpdkE2ETestEnv::env();
    auto cfg = make_dns_mock_cfg(env.dst_ip);

    auto r = eph::dpdk::dns::resolve(
        env.port_id, 0, /*pool=*/nullptr,
        env.src_mac, env.gw_mac, env.src_ip, "example.com", cfg);

    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("mempool is null"), std::string::npos)
        << "expected 'mempool is null' diagnostic, got: " << r.error();
}

TEST(DnsE2E, InvalidConfigZeroNameserverReturnsError) {
    EPH_DPDK_E2E_SKIP_IF_NOT_READY();
    auto& env = DpdkE2ETestEnv::env();

    eph::dpdk::dns::DnsConfig bad{};
    bad.nameserver_ip = 0;  // explicit invalid

    auto r = eph::dpdk::dns::resolve(
        env.port_id, 0, env.pool,
        env.src_mac, env.gw_mac, env.src_ip, "example.com", bad);

    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("Invalid DNS config"), std::string::npos)
        << "expected validation error, got: " << r.error();
    EXPECT_NE(r.error().find("nameserver_ip"), std::string::npos)
        << "expected nameserver_ip in error, got: " << r.error();
}

TEST(DnsE2E, TimeoutOnUnreachableNameserver) {
    EPH_DPDK_E2E_SKIP_IF_NOT_READY();
    auto& env = DpdkE2ETestEnv::env();

    // Point at a TEST-NET-1 (RFC 5737) host with no listener.  The mock
    // dispatcher does not bind anything on this address, and the kernel
    // routing path will black-hole it.  retry_interval is clamped to
    // 100ms, so a 250ms total timeout exercises the retry loop at least
    // once and then times out.
    eph::dpdk::dns::DnsConfig bh{};
    bh.nameserver_ip = 0xc0000263U;  // 192.0.2.99
    bh.port          = kDnsMockPort;
    bh.timeout       = std::chrono::milliseconds{250};

    auto t0 = std::chrono::steady_clock::now();
    auto r = eph::dpdk::dns::resolve(
        env.port_id, 0, env.pool,
        env.src_mac, env.gw_mac, env.src_ip, "blackhole.test", bh);
    auto elapsed = std::chrono::steady_clock::now() - t0;

    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("timeout"), std::string::npos)
        << "expected timeout diagnostic, got: " << r.error();
    // Sanity check: did we actually wait the configured budget?  We
    // give the upper bound generous slack for scheduler jitter.
    EXPECT_GE(elapsed, std::chrono::milliseconds{200});
    EXPECT_LT(elapsed, std::chrono::seconds{3});
}

TEST(DnsE2E, FastPathDottedDecimalSkipsNetwork) {
    EPH_DPDK_E2E_SKIP_IF_NOT_READY();
    auto& env = DpdkE2ETestEnv::env();
    auto cfg = make_dns_mock_cfg(env.dst_ip);

    // Dotted-decimal must short-circuit before any packet is sent.
    // Use an IP that the mock would NEVER return so we know the
    // result came from the fast path, not the network.
    auto r = eph::dpdk::dns::resolve(
        env.port_id, 0, env.pool,
        env.src_mac, env.gw_mac, env.src_ip, "10.20.30.40", cfg);

    ASSERT_TRUE(r.has_value()) << "fast path failed: " << r.error();
    EXPECT_EQ(*r, 0x0a141e28U)  // 10.20.30.40
        << "fast path returned wrong IP: 0x" << std::hex << *r;
    EXPECT_NE(*r, kDnsMockResolvedIp)
        << "fast path appears to have hit the mock instead of parsing locally";
}

// ═══════════════════════════════════════════════════════════════════════
// main — register test environment, run all suites
// ═══════════════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::warn);  // suppress INFO noise during tests
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new eph::dpdk::test_e2e::DpdkE2ETestEnv);
    return RUN_ALL_TESTS();
}
