/// @file tools/dpdk_rss_queue_probe.cpp
///
/// DPDK 侧 src_port→RX队列 **经验探测器**(finder)。
///
/// kernel 侧对应工具是 `tools/kernel_rss_queue_probe.py`(eBPF 读 skb->queue_mapping)。
/// 两者不可互换:NIC 绑 vfio-pci 时内核看不到包、eBPF 挂不上;且两套驱动各自编程进
/// 硬件的 RSS(key/reta)不同,kernel 测出的表不保证迁移到 DPDK——故 DPDK 必须在 EAL
/// 内用真实 rx_burst 自探。
///
/// 向 VPC DNS 反射器发不同 src_port 的 DNS 查询,收回包后按 dst_port 认出
/// src_port,记录它**实际落的 RX 队列**(rx_burst 的队列号),机器可读输出
/// `FINDERMAP <src_port> <queue>`(每队列至多 M 个)。
///
/// 纯经验、不信 RSS key——对 ENA 的占位 key 天然免疫(RSS 队列落点在 ENA 上
/// 无法用 Toeplitz 预测,只能实测;见 docs/cpu-no-cross-core.md)。把输出的
/// src_port 喂给 cfg.dpdk.wire.tuple.src_port + cfg.dpdk.pin_to_queue。
///
/// 用 VPC DNS 当反射器,绕开同实例 ENI 不通的限制(DNS 解析器不在本实例上)。
///
/// 用法(NIC 已绑 vfio-pci、hugepages 已分配;本机 DPDK 只用 ens5):
///   sudo dpdk_rss_queue_probe -l 0-1 -n 4 -a <bdf> --file-prefix=p -- [app-args]
/// app-args(均可选,默认本会话 ens5/VPC-DNS 值):
///   --local-ip A.B.C.D     本机 IP(发包源)
///   --dst-ip A.B.C.D       反射器 IP(VPC DNS,在子网内可达)
///   --dst-port N           反射器端口(53)
///   --local-mac aa:..:ff   本机 MAC
///   --dst-mac aa:..:ff     反射器 MAC(子网内 = 目标自身 MAC;先用 kernel ARP 取)
///   --nb-rxq N             RX 队列数(默认 4)
///   --port-lo / --port-hi  候选 src_port 范围(默认 40001..40512)
///   --per-queue M          每队列至多收集 M 个 src_port(默认 1)
///   --finder               accepted no-op(经验 finder 是唯一模式)

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <arpa/inet.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_udp.h>

#include <spdlog/spdlog.h>

namespace {

// ── 目标参数(默认 = 本会话 ens5 / VPC DNS 实测值;可被 argv 覆盖)──
uint32_t LOCAL_IP = 0xAC1F023E;  // 172.31.2.62 (ens5)
uint32_t DNS_IP   = 0xAC1F0002;  // 172.31.0.2  (VPC resolver)
uint16_t DNS_PORT = 53;
rte_ether_addr LOCAL_MAC{{0x0a, 0x4c, 0x71, 0xa7, 0x29, 0xff}};  // ens5
rte_ether_addr DST_MAC{{0x0a, 0x8a, 0xc5, 0x41, 0x9c, 0xc9}};    // 172.31.0.2 的 MAC

uint16_t NB_RXQ   = 4;
uint16_t PORT_LO  = 40001;
uint16_t PORT_HI  = 40512;
uint16_t PER_QUEUE = 1;

constexpr uint16_t NB_TXQ = 1;

bool parse_ipv4(const char* s, uint32_t& out) {
    unsigned a, b, c, d;
    if (std::sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    out = (a << 24) | (b << 16) | (c << 8) | d;  // host order (MSB = first octet)
    return true;
}

bool parse_mac(const char* s, rte_ether_addr& out) {
    unsigned x[6];
    if (std::sscanf(s, "%x:%x:%x:%x:%x:%x", &x[0], &x[1], &x[2], &x[3], &x[4], &x[5]) != 6)
        return false;
    for (int i = 0; i < 6; ++i) out.addr_bytes[i] = uint8_t(x[i]);
    return true;
}

void parse_app_args(int argc, char** argv) {
    auto need = [&](int& i) -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        const char* v = nullptr;
        if (a == "--finder") { /* accepted no-op: empirical finder is the only mode */ }
        else if (a == "--local-ip"  && (v = need(i))) parse_ipv4(v, LOCAL_IP);
        else if (a == "--dst-ip"    && (v = need(i))) parse_ipv4(v, DNS_IP);
        else if (a == "--dst-port"  && (v = need(i))) DNS_PORT = uint16_t(std::atoi(v));
        else if (a == "--local-mac" && (v = need(i))) parse_mac(v, LOCAL_MAC);
        else if (a == "--dst-mac"   && (v = need(i))) parse_mac(v, DST_MAC);
        else if (a == "--nb-rxq"    && (v = need(i))) NB_RXQ = uint16_t(std::atoi(v));
        else if (a == "--port-lo"   && (v = need(i))) PORT_LO = uint16_t(std::atoi(v));
        else if (a == "--port-hi"   && (v = need(i))) PORT_HI = uint16_t(std::atoi(v));
        else if (a == "--per-queue" && (v = need(i))) PER_QUEUE = uint16_t(std::atoi(v));
    }
}

uint16_t ipv4_csum(const void* hdr, size_t len) {
    const uint16_t* p = static_cast<const uint16_t*>(hdr);
    uint32_t sum = 0;
    for (size_t i = 0; i < len / 2; ++i) sum += p[i];
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return static_cast<uint16_t>(~sum);
}

size_t build_dns_query(uint8_t* p, uint16_t id) {
    size_t n = 0;
    auto put16 = [&](uint16_t v) { p[n++] = v >> 8; p[n++] = v & 0xff; };
    put16(id); put16(0x0100);
    put16(1); put16(0); put16(0); put16(0);
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
    p[n++] = 0;
    put16(1); put16(1);
    return n;
}

bool craft(rte_mbuf* m, uint16_t src_port) {
    uint8_t dns[64];
    size_t dns_len = build_dns_query(dns, src_port);
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
    udp->dgram_cksum = 0;

    std::memcpy(d + sizeof(rte_ether_hdr) + ip_len - udp_len + sizeof(rte_udp_hdr),
                dns, dns_len);

    m->data_len = uint16_t(tot);
    m->pkt_len  = uint32_t(tot);
    return true;
}

// Human-readable progress goes to stderr (spdlog); machine-readable
// FINDERMAP/FINDERDONE go to stdout — keep them on separate streams so
// callers can parse stdout cleanly.
void log_info(const std::string& s) { spdlog::info("{}", s); }

}  // namespace

int main(int argc, char** argv) {
    int consumed = rte_eal_init(argc, argv);
    if (consumed < 0) { spdlog::error("rte_eal_init failed"); return 2; }
    parse_app_args(argc - consumed, argv + consumed);

    const uint16_t port = 0;
    if (rte_eth_dev_count_avail() < 1) { spdlog::error("no DPDK port"); return 2; }

    auto* pool = rte_pktmbuf_pool_create("PROBE_POOL", 8192, 256, 0,
                                         RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (!pool) { spdlog::error("mbuf pool create failed"); return 2; }

    rte_eth_dev_info dev_info{};
    if (rte_eth_dev_info_get(port, &dev_info) != 0) { spdlog::error("dev_info_get"); return 2; }

    rte_eth_conf pc{};
    pc.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
    pc.rx_adv_conf.rss_conf.rss_key = nullptr;
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

    // ── 发候选 src_port 的 DNS 查询(范围扫描)──
    std::vector<uint16_t> ports;
    for (uint32_t p = PORT_LO; p <= PORT_HI; ++p) ports.push_back(uint16_t(p));
    for (uint16_t sp : ports) {
        rte_mbuf* m = rte_pktmbuf_alloc(pool);
        if (!m) continue;
        craft(m, sp);
        if (rte_eth_tx_burst(port, 0, &m, 1) < 1) rte_pktmbuf_free(m);
    }
    log_info(std::format("sent {} DNS queries to {}.{}.{}.{}:{}, polling…",
             ports.size(), (DNS_IP>>24)&0xff,(DNS_IP>>16)&0xff,(DNS_IP>>8)&0xff,DNS_IP&0xff, DNS_PORT));

    // ── 收回包,按 dst_port 认出 src_port,记录实际落的队列 ──
    struct Obs { bool got=false; uint32_t hash=0; bool flag=false; uint16_t qid=0; };
    std::vector<Obs> obs(65536);
    const uint64_t hz = rte_get_timer_hz();
    const uint64_t deadline = rte_get_timer_cycles() + hz * 4;
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

    {
        // ── 经验探测:每队列至多 PER_QUEUE 个 src_port,机器可读输出 ──
        // 直接看回包实际落的 RX 队列(obs[sp].qid),不做任何 Toeplitz 预测。
        std::vector<uint16_t> per_q_count(NB_RXQ, 0);
        for (uint16_t sp : ports) {
            if (!obs[sp].got) continue;
            uint16_t q = obs[sp].qid;
            if (q < NB_RXQ && per_q_count[q] < PER_QUEUE) {
                std::printf("FINDERMAP %u %u\n", unsigned(sp), unsigned(q));
                ++per_q_count[q];
            }
        }
        int filled = 0;
        for (uint16_t q = 0; q < NB_RXQ; ++q) if (per_q_count[q] >= PER_QUEUE) ++filled;
        std::printf("FINDERDONE filled=%d/%u per_queue=%u\n", filled, unsigned(NB_RXQ), unsigned(PER_QUEUE));
        std::fflush(stdout);
    }

    rte_eth_dev_stop(port);
    rte_eth_dev_close(port);
    rte_eal_cleanup();
    return 0;
}
