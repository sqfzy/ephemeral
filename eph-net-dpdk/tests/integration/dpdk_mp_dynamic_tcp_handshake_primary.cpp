/// @file dpdk_mp_dynamic_tcp_handshake_primary.cpp
/// Primary peer of `dpdk_mp_dynamic_tcp_handshake_e2e` — acceptance gate
/// for the RSS-aware connect fix (reshape/rss-aware-connect).
///
/// What this binary does:
///   1. Spawn a kernel TCP echo mock on NIC_A:port in a background
///      thread. NIC_A is the host's kernel-side NIC; the DPDK side
///      (this same process) talks to it via NIC_B vfio-pci over the
///      VPC fabric — same wire path lat_tcp_dpdk uses.
///   2. `Platform::join_dynamic` with NIC_B's PCI BDF; assert primary.
///   3. ARP-resolve the gateway MAC. Only primary does this — secondary
///      doesn't own queue 0 (where ARP replies typically land under
///      the NIC's default RSS), so we publish gw_mac to a shared file
///      that secondary reads.
///   4. Touch the ready file, then have THIS process also connect via
///      `DpdkTcpStream::create_and_attach` (no `pin_to_queue`) and
///      verify echo round-trip. Pre-fix this hung silently when RSS
///      hashed the SYN-ACK to a queue this process did not own; with
///      the fix it always succeeds because src_port is engineered
///      RSS-aware before connect.
///   5. Hold for HOLD_SECONDS so secondary has a window to complete
///      its own connect against the same mock port.
///
/// All env-var / config plumbing matches `dpdk_mp_dynamic_e2e.sh`. IPs
/// come from `bench.conf` so the wire layout is identical to
/// lat_tcp_dpdk's.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <rte_ethdev.h>
#include <spdlog/spdlog.h>

#include "../../../benchmarks/latency/core/bench_conf.hpp"
#include "eph/dpdk/arp.hpp"
#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/platform.hpp"
#include "eph/codec/raw_stream_codec.hpp"
#include "eph/net/dpdk/poller.hpp"
#include "eph/net/dpdk/tcp_stream.hpp"

#include "echo_mocks.hpp"

namespace ed = eph::net::dpdk;
namespace ecd = eph::codec;

namespace {

const char* env_or_null(const char* k) {
    const char* v = std::getenv(k);
    return (v && *v) ? v : nullptr;
}

std::string env_or(const char* k, const char* fallback) {
    const char* v = env_or_null(k);
    return v ? v : fallback;
}

uint32_t parse_ip_host_order(const std::string& s) {
    in_addr addr{};
    if (inet_pton(AF_INET, s.c_str(), &addr) != 1) {
        return 0;
    }
    return ntohl(addr.s_addr);
}

std::string mac_to_hex(const rte_ether_addr& m) {
    char buf[32];
    std::snprintf(buf, sizeof(buf),
                  "%02x:%02x:%02x:%02x:%02x:%02x",
                  m.addr_bytes[0], m.addr_bytes[1], m.addr_bytes[2],
                  m.addr_bytes[3], m.addr_bytes[4], m.addr_bytes[5]);
    return buf;
}

// Acceptance-test port: chosen above the 19000-block used by the
// dpdk_e2e suite mocks and below the bench-reserved 20000-29999 range.
constexpr uint16_t kHandshakePort = 21000;

} // namespace

TEST(DpdkMpDynamicTcpHandshakePrimary, ConnectsAndEchoes) {
    // ── Env / config ───────────────────────────────────────────────────
    const char* pci        = env_or_null("EPH_MP_ALLOWED_DEV");
    const char* ready_file = env_or_null("EPH_MP_READY_FILE");
    const char* gw_mac_file = env_or_null("EPH_MP_GW_MAC_FILE");
    if (!pci || !ready_file || !gw_mac_file) {
        GTEST_SKIP() << "missing env — run via "
                        "tests/integration/dpdk_mp_dynamic_tcp_handshake_e2e.sh";
    }

    const std::string bench_conf_path = env_or(
        "BENCH_CONFIG", "benchmarks/latency/config.toml");
    auto cfg_r = bench::load_bench_conf(bench_conf_path);
    ASSERT_TRUE(cfg_r) << "load_bench_conf(" << bench_conf_path << "): "
                       << bench::format_error(cfg_r.error());
    const auto& bcfg = *cfg_r;

    const std::string nb_rx_queues_s = env_or("EPH_MP_NB_RX_QUEUES", "4");
    const std::string lcores         = env_or("EPH_MP_LCORES",       "0");
    const std::string hold_s         = env_or("EPH_MP_HOLD_SECONDS", "20");
    const uint16_t nb_rx_queues =
        static_cast<uint16_t>(std::stoul(nb_rx_queues_s));
    const int hold_seconds = std::stoi(hold_s);
    ASSERT_GE(nb_rx_queues, 2u);

    // ── 1. Kernel TCP echo mock on NIC_A ───────────────────────────────
    std::atomic<bool> mock_running{true};
    std::thread mock_thread([&] {
        eph::dpdk::test_e2e::tcp_echo_mock_thread(
            bcfg.networking.server_ip, kHandshakePort, mock_running);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    auto cleanup_mock = [&] {
        mock_running.store(false, std::memory_order_release);
        // accept_one wakes on this flag; if a thread is parked in a
        // detached worker waiting for the flag's other half, the join
        // here only synchronises the listener thread itself.
        if (mock_thread.joinable()) mock_thread.join();
    };

    // ── 2. Platform::join_dynamic (v3) ─────────────────────────────────
    eph::dpdk::JoinDynamicConfigV3 jd{};
    jd.pci                            = pci;
    jd.primary_config.nb_rx_queues    = nb_rx_queues;
    // Match TX queues to RX so secondary's RSS-aware tx_queue_id
    // (= its owned target_qid) maps to a real TX ring. Default
    // nb_tx_queues=1 would leave secondary TX-ing into a nonexistent
    // queue 1 → SYN never leaves the NIC.
    jd.primary_config.nb_tx_queues    = nb_rx_queues;
    jd.lcores                         = {lcores};

    auto plat_r = eph::dpdk::Platform::join_dynamic(std::move(jd));
    if (!plat_r) {
        cleanup_mock();
        FAIL() << "join_dynamic: " << plat_r.error();
    }
    auto platform = std::move(*plat_r);
    ASSERT_FALSE(platform.is_secondary()) << "first peer must be primary";
    ASSERT_TRUE(platform.has_mp_topology());

    const uint16_t port_id = platform.port_id();
    rte_mempool* const pool = platform.mempool();
    ASSERT_NE(pool, nullptr);

    // ── 3. Local MAC + IPs ─────────────────────────────────────────────
    rte_ether_addr src_mac{};
    ASSERT_EQ(rte_eth_macaddr_get(port_id, &src_mac), 0);
    SPDLOG_INFO("primary: local MAC {}", mac_to_hex(src_mac));

    const uint32_t src_ip = parse_ip_host_order(bcfg.networking.client_ip);
    const uint32_t dst_ip = parse_ip_host_order(bcfg.networking.server_ip);
    const uint32_t gw_ip  = parse_ip_host_order(bcfg.networking.gateway_ip);
    ASSERT_NE(src_ip, 0u);
    ASSERT_NE(dst_ip, 0u);
    ASSERT_NE(gw_ip,  0u);

    // ── 4. ARP resolve gateway (primary owns queue 0 where reply lands) ─
    auto gw_mac_r = eph::dpdk::arp::resolve(
        port_id, pool, src_mac, src_ip, gw_ip,
        std::chrono::seconds{3});
    if (!gw_mac_r) {
        cleanup_mock();
        FAIL() << "arp::resolve: " << gw_mac_r.error();
    }
    rte_ether_addr gw_mac = *gw_mac_r;
    SPDLOG_INFO("primary: gateway MAC {}", mac_to_hex(gw_mac));

    // Publish gw_mac for secondary (it can't ARP-resolve from queue 1).
    {
        std::ofstream f(gw_mac_file);
        ASSERT_TRUE(f.is_open());
        f << mac_to_hex(gw_mac) << "\n";
    }

    // ── 5. Poller + register on owned queue range ──────────────────────
    auto [qlo, qhi] = platform.effective_rx_queue_range();
    ASSERT_LT(qlo, qhi);

    ed::PollerConfig pcfg{};
    pcfg.port_id     = port_id;
    pcfg.rx_queue_id = qlo;
    auto poller_r = ed::DpdkPoller<>::create(pcfg);
    if (!poller_r) {
        cleanup_mock();
        FAIL() << "DpdkPoller::create: " << poller_r.error().detail;
    }
    auto poller = std::move(*poller_r);

    for (uint16_t q = qlo; q < qhi; ++q) {
        auto rr = platform.register_poller(q, poller.get());
        ASSERT_TRUE(rr) << "register_poller(" << q << "): "
                        << rr.error().detail;
    }

    // ── 6. Stream::create_and_attach (no pin_to_queue) ────────────────
    // The whole point: in RssPartitioned mode without pin_to_queue,
    // the library MUST engineer src_port so SYN-ACK lands on a queue
    // this process owns. Pre-fix: hung silently.
    ed::StreamConfig scfg{};
    scfg.dpdk.tcp_low_level.tuple   = {src_ip, dst_ip, /*sp=*/0, kHandshakePort};
    scfg.dpdk.tcp_low_level.src_mac = src_mac;
    scfg.dpdk.tcp_low_level.dst_mac = gw_mac;
    scfg.dpdk.tcp_low_level.port_id = port_id;
    scfg.dpdk.pool                  = pool;
    scfg.connect_timeout            = std::chrono::milliseconds{5000};

    using Stream = ed::DpdkTcpStream<ecd::RawStreamCodec, /*EnableTls=*/false>;
    auto stream_r = Stream::create_and_attach(std::move(scfg), platform);
    if (!stream_r) {
        cleanup_mock();
        FAIL() << "create_and_attach: " << stream_r.error().detail;
    }
    auto stream = std::move(*stream_r);

    // ── 7. Touch ready file so secondary can join + connect ────────────
    {
        std::ofstream f(ready_file);
        f << "primary ready " << std::time(nullptr) << "\n";
    }

    // ── 8. Send 16 bytes, poll for echo ───────────────────────────────
    constexpr uint8_t kPayload[16] = {
        'P','R','I','M','-','H','S','-','T','E','S','T','-','0','0','1'
    };
    std::size_t rx_bytes = 0;
    stream->on_message = [&](std::span<const uint8_t> frame) {
        rx_bytes += frame.size();
    };
    auto send_r = stream->send(std::span<const uint8_t>(kPayload, sizeof(kPayload)));
    ASSERT_TRUE(send_r) << "stream->send: " << send_r.error().detail;

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{5};
    while (rx_bytes < sizeof(kPayload) &&
           std::chrono::steady_clock::now() < deadline) {
        poller->poll();
    }
    EXPECT_GE(rx_bytes, sizeof(kPayload))
        << "primary did not receive echo within 5s — RSS-blind connect "
           "regression?";

    SPDLOG_INFO("primary: handshake + echo OK ({} bytes)", rx_bytes);

    // ── 9. Hold so secondary can finish its own connect ────────────────
    std::this_thread::sleep_for(std::chrono::seconds(hold_seconds));

    // Drop stream (releases poller registration via FlowRule RAII).
    stream.reset();
    cleanup_mock();
    // ~Platform owns EAL: destructor releases DPDK + eal_cleanup.
}
