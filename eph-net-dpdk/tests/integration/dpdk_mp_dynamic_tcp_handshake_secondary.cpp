/// @file dpdk_mp_dynamic_tcp_handshake_secondary.cpp
/// Secondary peer of `dpdk_mp_dynamic_tcp_handshake_e2e` — the actual
/// fix-target case. Pre-reshape this scenario hung silently because
/// secondary owns queue 1 (not queue 0) under autojoin, yet
/// `DpdkTcpStream::create_and_attach` did not engineer src_port to
/// land SYN-ACK on a queue secondary owned. Post-fix: SYN-ACK is
/// guaranteed to Toeplitz-hash onto queue 1 → handshake succeeds.
///
/// Reads gw_mac from the file primary published (secondary cannot
/// ARP-resolve directly because reply lands on queue 0 ≠ secondary's).

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
#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/platform.hpp"
#include "eph/codec/raw_stream_codec.hpp"
#include "eph/net/dpdk/poller.hpp"
#include "eph/net/dpdk/tcp_stream.hpp"

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
    if (inet_pton(AF_INET, s.c_str(), &addr) != 1) return 0;
    return ntohl(addr.s_addr);
}

bool parse_mac(const std::string& s, rte_ether_addr& out) {
    unsigned int b[6];
    if (std::sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x",
                    &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        if (b[i] > 0xff) return false;
        out.addr_bytes[i] = static_cast<uint8_t>(b[i]);
    }
    return true;
}

constexpr uint16_t kHandshakePort = 21000;

} // namespace

TEST(DpdkMpDynamicTcpHandshakeSecondary, ConnectsAndEchoes) {
    const char* pci         = env_or_null("EPH_MP_ALLOWED_DEV");
    const char* gw_mac_file = env_or_null("EPH_MP_GW_MAC_FILE");
    if (!pci || !gw_mac_file) {
        GTEST_SKIP() << "missing env — run via "
                        "tests/integration/dpdk_mp_dynamic_tcp_handshake_e2e.sh";
    }

    const std::string bench_conf_path = env_or(
        "BENCH_CONFIG", "benchmarks/latency/config.toml");
    auto cfg_r = bench::load_bench_conf(bench_conf_path);
    ASSERT_TRUE(cfg_r) << bench::format_error(cfg_r.error());
    const auto& bcfg = *cfg_r;

    const std::string nb_rx_queues_s = env_or("EPH_MP_NB_RX_QUEUES", "4");
    const std::string lcores         = env_or("EPH_MP_LCORES_SEC",   "1");
    const uint16_t nb_rx_queues =
        static_cast<uint16_t>(std::stoul(nb_rx_queues_s));
    ASSERT_GE(nb_rx_queues, 2u);

    // ── 1. Platform::join_dynamic ──────────────────────────────────────
    eph::dpdk::JoinDynamicConfig jd{};
    jd.pci                        = pci;
    jd.pcfg_template.nb_rx_queues = nb_rx_queues;
    jd.pcfg_template.nb_tx_queues = nb_rx_queues;  // match primary
    jd.lcores                     = {lcores};

    auto plat_r = eph::dpdk::Platform::join_dynamic(std::move(jd));
    ASSERT_TRUE(plat_r) << "join_dynamic: " << plat_r.error();
    auto platform = std::move(*plat_r);
    ASSERT_TRUE(platform.is_secondary()) << "second peer must be secondary";

    auto [qlo, qhi] = platform.effective_rx_queue_range();
    ASSERT_GT(qlo, 0u) << "secondary's queue range must skip primary's";

    const uint16_t port_id = platform.port_id();
    rte_mempool* const pool = platform.mempool();
    ASSERT_NE(pool, nullptr);

    // ── 2. Local MAC ───────────────────────────────────────────────────
    rte_ether_addr src_mac{};
    ASSERT_EQ(rte_eth_macaddr_get(port_id, &src_mac), 0);

    const uint32_t src_ip = parse_ip_host_order(bcfg.networking.client_ip);
    const uint32_t dst_ip = parse_ip_host_order(bcfg.networking.server_ip);
    ASSERT_NE(src_ip, 0u);
    ASSERT_NE(dst_ip, 0u);

    // ── 3. Read gw_mac from primary's published file ───────────────────
    rte_ether_addr gw_mac{};
    {
        std::ifstream f(gw_mac_file);
        ASSERT_TRUE(f.is_open()) << "could not read " << gw_mac_file;
        std::string line;
        std::getline(f, line);
        ASSERT_TRUE(parse_mac(line, gw_mac))
            << "could not parse gw_mac from '" << line << "'";
    }
    SPDLOG_INFO("secondary: queue range [{},{}), using gw_mac from primary",
                 qlo, qhi);

    // ── 4. Poller on secondary's owned queue ──────────────────────────
    ed::PollerConfig pcfg{};
    pcfg.port_id     = port_id;
    pcfg.rx_queue_id = qlo;
    auto poller_r = ed::DpdkPoller<>::create(pcfg);
    ASSERT_TRUE(poller_r) << poller_r.error().detail;
    auto poller = std::move(*poller_r);

    for (uint16_t q = qlo; q < qhi; ++q) {
        auto rr = platform.register_poller(q, poller.get());
        ASSERT_TRUE(rr) << rr.error().detail;
    }

    // ── 5. Stream::create_and_attach (no pin_to_queue — the fix path) ──
    ed::StreamConfig scfg{};
    scfg.dpdk.tcp_low_level.tuple   = {src_ip, dst_ip, /*sp=*/0, kHandshakePort};
    scfg.dpdk.tcp_low_level.src_mac = src_mac;
    scfg.dpdk.tcp_low_level.dst_mac = gw_mac;
    scfg.dpdk.tcp_low_level.port_id = port_id;
    scfg.dpdk.pool                  = pool;
    scfg.connect_timeout            = std::chrono::milliseconds{5000};

    using Stream = ed::DpdkTcpStream<ecd::RawStreamCodec, /*EnableTls=*/false>;
    auto stream_r = Stream::create_and_attach(std::move(scfg), platform);
    ASSERT_TRUE(stream_r) << "create_and_attach: " << stream_r.error().detail;
    auto stream = std::move(*stream_r);

    // ── 6. Send 16 bytes, poll 5s for echo ────────────────────────────
    constexpr uint8_t kPayload[16] = {
        'S','E','C','-','-','H','S','-','T','E','S','T','-','0','0','2'
    };
    std::size_t rx_bytes = 0;
    stream->on_message = [&](std::span<const uint8_t> frame) {
        rx_bytes += frame.size();
    };
    auto send_r = stream->send(std::span<const uint8_t>(kPayload, sizeof(kPayload)));
    ASSERT_TRUE(send_r) << send_r.error().detail;

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{5};
    while (rx_bytes < sizeof(kPayload) &&
           std::chrono::steady_clock::now() < deadline) {
        poller->poll();
    }
    EXPECT_GE(rx_bytes, sizeof(kPayload))
        << "secondary did not receive echo within 5s — this IS the "
           "RSS-blind connect bug; check tcp_stream.hpp::create_and_attach";

    SPDLOG_INFO("secondary: handshake + echo OK ({} bytes)", rx_bytes);

    stream.reset();
}
