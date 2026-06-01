/// @file examples/dpdk_rsskey_probe.cpp
///
/// 一次性诊断：DPDK ENA 的 probed RSS key 到底是不是硬件真正用来 steering 的 key。
///
/// 裸 DPDK 起本端口(4 RSS 队列)，读 `query_rss_state` 探到的 key，向 VPC DNS
/// (172.31.0.2:53) 发 N 个不同 src_port 的 DNS 查询。DNS 回包(入站 4-tuple =
/// src=DNS:53, dst=本机:src_port)会被 NIC 算 RSS 哈希并写进 `mbuf->hash.rss`。
/// 对比 `mbuf->hash.rss` 与 `toeplitz_hash_ipv4(probed_key, 入站tuple)`：
///   全部相等 → probed key 是真 key（case i），eph 的 predict_rss_queue 在 ENA 上成立。
///   不相等   → probed key 是软件占位（case ii），eph 的 ENA RSS 假设是错的。
///
/// 这是 test_dpdk_rss_key_correctness 的核心断言，但用 VPC DNS 当反射器，绕开了
/// 同实例 ENI 不通的限制（DNS 解析器不在本实例上）。
///
/// 用法（ens5 已绑 vfio-pci，hugepages 已分配）:
///   sudo dpdk_rsskey_probe -l 0-1 -n 4 -a 0000:00:05.0 --file-prefix=rsskeyprobe --

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include <arpa/inet.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_udp.h>

#include <spdlog/spdlog.h>

#include "eph/net/dpdk/flow_steering.hpp"  // query_rss_state, toeplitz_hash_ipv4, RssState

namespace {

// ── 本机 / 目标固定参数（本会话实测值）──
constexpr uint32_t LOCAL_IP = 0xAC1F023E;  // 172.31.2.62 (ens5)
constexpr uint32_t DNS_IP   = 0xAC1F0002;  // 172.31.0.2  (VPC resolver)
constexpr uint16_t DNS_PORT = 53;
const rte_ether_addr LOCAL_MAC{{0x0a, 0x4c, 0x71, 0xa7, 0x29, 0xff}};  // ens5
const rte_ether_addr DST_MAC{{0x0a, 0x8a, 0xc5, 0x41, 0x9c, 0xc9}};    // 172.31.0.2 的 MAC

constexpr uint16_t NB_RXQ = 4;
constexpr uint16_t NB_TXQ = 1;

// 一组候选 src_port（ephemeral 段，覆盖到不同队列）
const std::vector<uint16_t> SRC_PORTS = {
    40001, 40002, 40003, 40004, 40005, 40006, 40007, 40008,
    40010, 40012, 40014, 40016, 40020, 40025, 40030, 40040,
};

uint16_t ipv4_csum(const void* hdr, size_t len) {
    const uint16_t* p = static_cast<const uint16_t*>(hdr);
    uint32_t sum = 0;
    for (size_t i = 0; i < len / 2; ++i) sum += p[i];
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return static_cast<uint16_t>(~sum);
}

// 构造一个最小 DNS A 查询 ("amazonaws.com")
size_t build_dns_query(uint8_t* p, uint16_t id) {
    size_t n = 0;
    auto put16 = [&](uint16_t v) { p[n++] = v >> 8; p[n++] = v & 0xff; };
    put16(id); put16(0x0100);            // id, flags=RD
    put16(1); put16(0); put16(0); put16(0);  // qd=1 an=ns=ar=0
    const char* name = "amazonaws.com";
    const char* seg = name;
    while (true) {
        const char* dot = std::strchr(seg, '.');
        size_t l = dot ? size_t(dot - seg) : std::strlen(seg);
        p[n++] = uint8_t(l);
        std::memcpy(p + n, seg, l); n += l;
        if (!dot) break;
        seg = dot + 1;
    }
    p[n++] = 0;                          // root
    put16(1); put16(1);                  // qtype=A qclass=IN
    return n;
}

// 在 mbuf 里组一个 DNS 查询包 (Eth/IPv4/UDP/DNS)，返回是否成功
bool craft(rte_mbuf* m, uint16_t src_port) {
    uint8_t dns[64];
    size_t dns_len = build_dns_query(dns, src_port);  // 用 src_port 当 DNS id，便于辨识
    size_t udp_len = sizeof(rte_udp_hdr) + dns_len;
    size_t ip_len  = sizeof(rte_ipv4_hdr) + udp_len;
    size_t tot     = sizeof(rte_ether_hdr) + ip_len;

    uint8_t* d = rte_pktmbuf_mtod(m, uint8_t*);
    std::memset(d, 0, tot);

    auto* eth = reinterpret_cast<rte_ether_hdr*>(d);
    eth->dst_addr = DST_MAC;
    eth->src_addr = LOCAL_MAC;
    eth->ether_type = htons(RTE_ETHER_TYPE_IPV4);

    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(d + sizeof(rte_ether_hdr));
    ip->version_ihl = 0x45;
    ip->total_length = htons(uint16_t(ip_len));
    ip->time_to_live = 64;
    ip->next_proto_id = IPPROTO_UDP;
    ip->src_addr = htonl(LOCAL_IP);
    ip->dst_addr = htonl(DNS_IP);
    ip->hdr_checksum = 0;
    ip->hdr_checksum = ipv4_csum(ip, sizeof(rte_ipv4_hdr));

    auto* udp = reinterpret_cast<rte_udp_hdr*>(d + sizeof(rte_ether_hdr) + sizeof(rte_ipv4_hdr));
    udp->src_port = htons(src_port);
    udp->dst_port = htons(DNS_PORT);
    udp->dgram_len = htons(uint16_t(udp_len));
    udp->dgram_cksum = 0;  // IPv4 UDP 校验和可为 0

    std::memcpy(d + sizeof(rte_ether_hdr) + ip_len - udp_len + sizeof(rte_udp_hdr),
                dns, dns_len);

    m->data_len = uint16_t(tot);
    m->pkt_len  = uint32_t(tot);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    int consumed = rte_eal_init(argc, argv);
    if (consumed < 0) { spdlog::error("rte_eal_init failed"); return 2; }

    const uint16_t port = 0;
    if (rte_eth_dev_count_avail() < 1) { spdlog::error("no DPDK port"); return 2; }

    auto* pool = rte_pktmbuf_pool_create("PROBE_POOL", 8192, 256, 0,
                                         RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (!pool) { spdlog::error("mbuf pool create failed"); return 2; }

    rte_eth_dev_info dev_info{};
    rte_eth_dev_info_get(port, &dev_info);

    rte_eth_conf pc{};
    pc.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
    pc.rx_adv_conf.rss_conf.rss_key = nullptr;  // 用 PMD 默认 key（本次 attach 的那把）
    pc.rx_adv_conf.rss_conf.rss_hf =
        (RTE_ETH_RSS_IP | RTE_ETH_RSS_UDP | RTE_ETH_RSS_TCP) &
        dev_info.flow_type_rss_offloads;
    if (dev_info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_RSS_HASH)
        pc.rxmode.offloads |= RTE_ETH_RX_OFFLOAD_RSS_HASH;

    if (rte_eth_dev_configure(port, NB_RXQ, NB_TXQ, &pc) < 0) {
        spdlog::error("dev_configure failed"); return 2;
    }
    for (uint16_t q = 0; q < NB_RXQ; ++q)
        if (rte_eth_rx_queue_setup(port, q, 512, rte_socket_id(), nullptr, pool) < 0) {
            spdlog::error("rx_queue_setup q={} failed", q); return 2;
        }
    if (rte_eth_tx_queue_setup(port, 0, 512, rte_socket_id(), nullptr) < 0) {
        spdlog::error("tx_queue_setup failed"); return 2;
    }
    if (rte_eth_dev_start(port) < 0) { spdlog::error("dev_start failed"); return 2; }

    // ── 探 RSS key ──
    auto st = eph::net::dpdk::query_rss_state(port);
    if (!st) { spdlog::error("query_rss_state: {}", st.error()); return 2; }
    auto key = st->key();
    std::string keyhex;
    for (auto b : key) { char t[3]; std::snprintf(t, 3, "%02x", b); keyhex += t; }
    spdlog::info("probed key_len={} reta_size={} key={}", st->key_len, st->reta_size, keyhex);

    // ── 发 DNS 查询 ──
    for (uint16_t sp : SRC_PORTS) {
        rte_mbuf* m = rte_pktmbuf_alloc(pool);
        if (!m) continue;
        craft(m, sp);
        if (rte_eth_tx_burst(port, 0, &m, 1) < 1) rte_pktmbuf_free(m);
    }
    spdlog::info("sent {} DNS queries to 172.31.0.2:53, polling replies…", SRC_PORTS.size());

    // ── 收回包，按 dst_port 认出对应 src_port，读 mbuf->hash.rss ──
    struct Obs { bool got=false; uint32_t hash=0; bool flag=false; uint16_t qid=0; };
    std::array<Obs, 65536> obs{};
    const uint64_t hz = rte_get_timer_hz();
    const uint64_t deadline = rte_get_timer_cycles() + hz * 4;  // 收 4 秒
    while (rte_get_timer_cycles() < deadline) {
        for (uint16_t q = 0; q < NB_RXQ; ++q) {
            rte_mbuf* burst[32];
            uint16_t n = rte_eth_rx_burst(port, q, burst, 32);
            for (uint16_t i = 0; i < n; ++i) {
                rte_mbuf* m = burst[i];
                auto* eth = rte_pktmbuf_mtod(m, rte_ether_hdr*);
                if (eth->ether_type == htons(RTE_ETHER_TYPE_IPV4)) {
                    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(
                        rte_pktmbuf_mtod(m, uint8_t*) + sizeof(rte_ether_hdr));
                    if (ip->next_proto_id == IPPROTO_UDP &&
                        ip->src_addr == htonl(DNS_IP)) {
                        auto* udp = reinterpret_cast<rte_udp_hdr*>(
                            reinterpret_cast<uint8_t*>(ip) + ((ip->version_ihl & 0xf) * 4));
                        if (ntohs(udp->src_port) == DNS_PORT) {
                            uint16_t dp = ntohs(udp->dst_port);
                            obs[dp].got = true;
                            obs[dp].hash = m->hash.rss;
                            obs[dp].flag = (m->ol_flags & RTE_MBUF_F_RX_RSS_HASH) != 0;
                            obs[dp].qid = q;
                        }
                    }
                }
                rte_pktmbuf_free(m);
            }
        }
    }

    // ── 比对 ──
    // ENA 不把 RSS hash 值写进 mbuf（实测 hash.rss 恒为 0，即便 flag 置位），
    // 所以改用「队列」对比：predict_rss_queue(probed key + 真实 RETA, 入站tuple)
    // vs 包实际落的 RX 队列。入站方向：src=DNS:53, dst=本机:sp（见 queue_for_tuple 注释）。
    int got = 0, qmatch = 0, qmismatch = 0; bool hash_exposed = false;
    spdlog::info("--- per src_port: 预测队列(probed key+RETA) vs 实际落的 RX 队列 ---");
    for (uint16_t sp : SRC_PORTS) {
        if (!obs[sp].got) { spdlog::warn("src_port={} : 无回包", sp); continue; }
        ++got;
        if (obs[sp].hash != 0) hash_exposed = true;
        uint16_t pred_q = eph::net::dpdk::queue_for_tuple(*st, DNS_IP, DNS_PORT, LOCAL_IP, sp);
        uint32_t pred_h = eph::net::dpdk::toeplitz_hash_ipv4(key, DNS_IP, DNS_PORT, LOCAL_IP, sp);
        bool ok = (pred_q == obs[sp].qid);
        if (ok) ++qmatch; else ++qmismatch;
        spdlog::info("src_port={} actual_q={} predicted_q={} (pred_hash=0x{:08x} nic_hash=0x{:08x}) {}",
                     sp, obs[sp].qid, pred_q, pred_h, obs[sp].hash, ok ? "QMATCH" : "QMISMATCH");
    }
    spdlog::info("================ 判定 ================");
    spdlog::info("回包 {}/{}，队列预测 match={} mismatch={}；NIC 是否暴露 hash 值={}",
                 got, SRC_PORTS.size(), qmatch, qmismatch, hash_exposed);
    if (got == 0) {
        spdlog::warn("没收到任何 DNS 回包 —— DPDK 口到 VPC DNS 的路径没通，无法判定");
    } else if (qmismatch == 0) {
        spdlog::info("→ ✅ probed key 是真 key（case i）：predict_rss_queue 与实际落核全一致，"
                     "eph 的 ENA RSS 假设成立");
    } else if (qmatch == 0) {
        spdlog::info("→ ❌ probed key 是软件占位（case ii）：predict_rss_queue 与实际落核全不符，"
                     "eph 的 ENA RSS 假设错误（probed key 不是硬件真正用的 key）");
    } else {
        spdlog::warn("→ ⚠️ 部分一致（{}/{})，看上面逐条", qmatch, got);
    }

    rte_eth_dev_stop(port);
    rte_eth_dev_close(port);
    rte_eal_cleanup();
    return 0;
}
