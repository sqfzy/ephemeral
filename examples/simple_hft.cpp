/// @file simple_hft.cpp
///
/// Canonical "simple HFT client" — DPDK datapath, single-lcore burst loop,
/// `WsCodec` + TLS 1.3 over a real WebSocket endpoint. Demonstrates the
/// shortest path from `Platform` attach to streaming application frames
/// using the turnkey `DpdkTcpStream::create_and_attach` factory:
///
///   1. `Platform::create(PlatformConfig{.pci, .queues, ...})` — secondary
///      attach to a daemon-managed NIC (the `eph-nicd` binary must already
///      be running for this PCI).
///   2. `arp::resolve` + `dns::resolve` — DPDK-native L2/L3 control plane
///   3. `DpdkPoller<>::create`          — single-queue burst-poll loop driver
///   4. `DpdkTcpStream<WsCodec, true>::create_and_attach`
///        — TCP handshake, TLS 1.3 (aws-lc), RFC 6455 Upgrade,
///          src_port allocation, FlowDirector / RSS-pin queue selection,
///          Poller::add and ICMP registration ALL in one call.
///   5. `on_message` callback, `poller->poll()` until duration / SIGINT.
///
/// For the production-grade variant with reconnect, JSON parse, and a
/// latency histogram, see `examples/binance_latency.cpp`. For the DPDK
/// control-plane mechanics (multi-process, RSS multi-queue), see
/// `examples/dpdk_mp_demo.cpp` and `examples/dpdk_rss_demo.cpp`.
///
/// Required environment:
///   - NIC bound to vfio-pci, ≥ 256 hugepages free
///     (see `eph-net-dpdk/scripts/dpdk-setup.sh`)
///   - `eph-nicd@<pci>.service` running for the target PCI BDF (the daemon
///     owns the NIC primary; this example attaches as secondary). Until
///     the eph-nicd binary lands (S4 of the daemon reshape), operators can
///     bring up the daemon equivalent in a sibling process via
///     `Platform::serve_nic(NicServiceConfig{.pci=...})`. EAL forbids
///     primary+secondary in the same process, so this single-binary demo
///     is application-only.
///
/// Usage:
///   sudo ./simple_hft --
///        --pci 0000:28:00.0 --pin 0=2:ws
///        --local-ip 10.0.0.16 --gateway-ip 10.0.0.1
///        --host stream.binance.com [--dst-ip 52.193.255.60]
///        [--port 9443] [--path /ws/btcusdt@aggTrade]
///        [--nameserver 8.8.8.8] [--duration 30]
///
/// `--dst-ip` skips the DPDK-native DNS path. When absent, the example
/// resolves `--host` through `--nameserver` (default 8.8.8.8) over the
/// ARP-resolved gateway.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_mempool.h>

#include <spdlog/spdlog.h>

#include "eph/codec/ws_codec.hpp"

#include "eph/dpdk/arp.hpp"
#include "eph/dpdk/cli.hpp"
#include "eph/dpdk/dns.hpp"
#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/lcore_pin.hpp"
#include "eph/dpdk/packet_core.hpp"   // net::parse_ipv4 / format_ipv4 / kDefaultMss
#include "eph/dpdk/platform.hpp"

#include "eph/net/dpdk/poller.hpp"
#include "eph/net/dpdk/tcp_stream.hpp"
#include "eph/net/stream_metrics.hpp"

#include "eph/utils/cpu.hpp"

namespace edpdk = eph::net::dpdk;
namespace ed    = eph::dpdk;
namespace ec    = eph::codec;
using namespace std::chrono_literals;

static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running.store(false, std::memory_order_release); }

namespace {

struct AppConfig {
    ed::cli::EalCliConfig eal{};        // --pci / --pin / --lcores / --port-id / --dpdk-port

    std::string  host = "stream.binance.com";
    std::string  ws_path = "/ws/btcusdt@aggTrade";
    uint16_t     port = 9443;
    uint32_t     local_ip = 0;     // required
    uint32_t     gateway_ip = 0;   // required
    uint32_t     dst_ip = 0;       // 0 ⇒ resolve via DPDK-native DNS
    uint32_t     nameserver_ip = 0x08080808;
    int          duration_s = 30;
};

int split_at_dash_dash(int argc, char** argv) noexcept {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view{argv[i]} == "--") return i;
    }
    return argc;
}

std::optional<AppConfig> parse_args(int argc, char** argv) {
    AppConfig cfg{};
    auto parse_ip = [](const char* s, uint32_t& out) -> bool {
        const uint32_t v = ed::net::parse_ipv4(s);
        if (v == 0) return false;
        out = v;
        return true;
    };

    for (int i = 0; i < argc; ++i) {
        const std::string_view a = argv[i];
        const char* next = (i + 1 < argc) ? argv[i + 1] : nullptr;

        // EAL flags first — try_consume returns 2 if it ate the token + value.
        auto consumed = ed::cli::consume_one(cfg.eal, a, next);
        if (!consumed) {
            spdlog::error("{}", consumed.error());
            return std::nullopt;
        }
        if (*consumed > 0) { i += *consumed - 1; continue; }

        // App-specific flags. need() advances i on success.
        auto need = [&](const char* flag) {
            if (!next) {
                spdlog::error("{} requires an argument", flag);
                return false;
            }
            ++i;
            return true;
        };
        if      (a == "--host" && need("--host"))         cfg.host    = next;
        else if (a == "--path" && need("--path"))         cfg.ws_path = next;
        else if (a == "--port" && need("--port"))
            cfg.port = static_cast<uint16_t>(std::atoi(next));
        else if (a == "--duration" && need("--duration")) cfg.duration_s = std::atoi(next);
        else if (a == "--local-ip" && need("--local-ip")) {
            if (!parse_ip(next, cfg.local_ip)) {
                spdlog::error("invalid --local-ip: {}", next);
                return std::nullopt;
            }
        }
        else if (a == "--gateway-ip" && need("--gateway-ip")) {
            if (!parse_ip(next, cfg.gateway_ip)) {
                spdlog::error("invalid --gateway-ip: {}", next);
                return std::nullopt;
            }
        }
        else if (a == "--dst-ip" && need("--dst-ip")) {
            if (!parse_ip(next, cfg.dst_ip)) {
                spdlog::error("invalid --dst-ip: {}", next);
                return std::nullopt;
            }
        }
        else if (a == "--nameserver" && need("--nameserver")) {
            if (!parse_ip(next, cfg.nameserver_ip)) {
                spdlog::error("invalid --nameserver: {}", next);
                return std::nullopt;
            }
        }
        else spdlog::warn("ignoring unknown argument: {}", a);
    }

    if (cfg.local_ip == 0) {
        spdlog::error("--local-ip is required (DPDK has no kernel to ask)");
        return std::nullopt;
    }
    if (cfg.gateway_ip == 0) {
        spdlog::error("--gateway-ip is required (L2 next-hop for ARP)");
        return std::nullopt;
    }
    return cfg;
}

std::string mac_to_string(const rte_ether_addr& m) {
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
                  m.addr_bytes[0], m.addr_bytes[1], m.addr_bytes[2],
                  m.addr_bytes[3], m.addr_bytes[4], m.addr_bytes[5]);
    return buf;
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    spdlog::set_level(spdlog::level::info);

    // ── 1) Argv split: pre-`--` is unused, post-`--` is the app config ───
    const int eal_end = split_at_dash_dash(argc, argv);
    const int app_start = (eal_end == argc) ? eal_end : eal_end + 1;

    auto cfg_opt = parse_args(argc - app_start, argv + app_start);
    if (!cfg_opt) return 1;
    AppConfig cfg = std::move(*cfg_opt);

    // ── 2) --pin / --lcores exclusivity (+ require at least one) ──────────
    if (auto v = ed::cli::validate(cfg.eal); !v) {
        spdlog::error("simple_hft: {}", v.error());
        return 1;
    }
    if (cfg.eal.pins.empty() && cfg.eal.lcores_raw.empty()) {
        spdlog::error("simple_hft: provide either --pin lcore=cpu[:role] "
                      "(typed) or --lcores '<raw EAL spec>' (escape hatch)");
        return 1;
    }

    // ── 3) Platform::create — application secondary attach ───────
    //
    // The daemon-led model (post-2026-05-02) requires `eph-nicd` to already
    // own the NIC primary for `cfg.eal.pci[0]` before we attach as secondary.
    // PlatformConfig now carries only `pci` + `queues` + EAL knobs; the NIC
    // physical state (descriptor depth, RSS key, mempool size, promiscuous,
    // …) lives in `NicServiceConfig` and is the daemon's job.
    //
    // TODO(daemon-reshape): once `eph-nicd` ships (S4) and `eph::dpdk::dev::
    // ensure_local_daemon(pci)` lands (S6), this example can call the dev
    // helper to fork the daemon when running ad-hoc on a dev box. Today
    // operators must `sudo systemctl start eph-nicd@<pci>.service` (or run
    // the daemon binary by hand in another terminal) before invoking this.
    if (cfg.eal.pci.empty()) {
        spdlog::error("simple_hft: --pci is required (the daemon model "
                      "needs an explicit BDF until default-NIC selection "
                      "from /etc/eph/*.toml lands in S6)");
        return 1;
    }
    ed::PlatformConfig pcfg{};
    pcfg.pci            = cfg.eal.pci.front();
    pcfg.queues         = 1;
    pcfg.pins           = std::span<ed::LcorePin const>{cfg.eal.pins};
    pcfg.pin_policy     = eph::utils::CpuPinPolicy{};
    pcfg.lcores         = cfg.eal.lcores_raw.empty()
                              ? std::vector<std::string>{}
                              : std::vector<std::string>{cfg.eal.lcores_raw};
    pcfg.program_name   = "simple_hft";

    auto plat_r = ed::Platform::create(std::move(pcfg));
    if (!plat_r) {
        spdlog::error("Platform::create failed: {} "
                      "(is `eph-nicd@{}.service` running?)",
                      plat_r.error(), cfg.eal.pci.front());
        return 2;
    }
    auto platform = std::move(*plat_r);
    const uint16_t port_id = platform.port_id();
    rte_mempool* const pool = platform.mempool();

    // ── 4) Local MAC + ARP-resolve gateway + (optional) DNS-resolve host ──
    rte_ether_addr src_mac{};
    if (int rc = rte_eth_macaddr_get(port_id, &src_mac); rc != 0) {
        spdlog::error("rte_eth_macaddr_get failed: rc={}", rc);
        return 3;
    }
    spdlog::info("port_id={} src_mac={} local_ip={}",
                 port_id, mac_to_string(src_mac),
                 ed::net::format_ipv4(cfg.local_ip).data());

    auto gw_r = ed::arp::resolve(port_id, pool, src_mac, cfg.local_ip,
                                 cfg.gateway_ip, 3s);
    if (!gw_r) {
        spdlog::error("ARP resolve gateway {} failed: {}",
                      ed::net::format_ipv4(cfg.gateway_ip).data(),
                      gw_r.error());
        return 4;
    }
    const rte_ether_addr gw_mac = *gw_r;
    spdlog::info("gateway {} -> mac {}",
                 ed::net::format_ipv4(cfg.gateway_ip).data(),
                 mac_to_string(gw_mac));

    uint32_t dst_ip = cfg.dst_ip;
    if (dst_ip == 0) {
        ed::dns::DnsConfig dns_cfg{};
        dns_cfg.nameserver_ip = cfg.nameserver_ip;
        dns_cfg.timeout = 3s;
        auto dns_r = ed::dns::resolve(port_id, /*queue=*/0, pool, src_mac,
                                      gw_mac, cfg.local_ip, cfg.host, dns_cfg);
        if (!dns_r) {
            spdlog::error("DPDK DNS resolve '{}' via {} failed: {}",
                          cfg.host,
                          ed::net::format_ipv4(cfg.nameserver_ip).data(),
                          dns_r.error());
            return 5;
        }
        dst_ip = *dns_r;
    }
    spdlog::info("destination {} -> {}", cfg.host,
                 ed::net::format_ipv4(dst_ip).data());

    // ── 5) Poller — one queue, registered with Platform ───────────────────
    edpdk::PollerConfig poller_cfg{};
    poller_cfg.port_id     = port_id;
    poller_cfg.rx_queue_id = 0;
    auto poller_r = edpdk::DpdkPoller<>::create(poller_cfg);
    if (!poller_r) {
        spdlog::error("DpdkPoller::create failed: {}",
                      poller_r.error().detail);
        return 6;
    }
    auto poller = std::move(*poller_r);
    if (auto r = platform.register_poller(0, poller.get()); !r) {
        spdlog::error("register_poller failed: {}", r.error().detail);
        return 7;
    }

    // ── 6) DpdkTcpStream<WsCodec, TLS> via the turnkey factory ────────────
    // create_and_attach folds: src_port allocation, queue selection
    // (Software / RSS-pinned / FlowDirector — picked by Platform's
    // dispatch_mode), TCP+TLS+WS handshakes, Poller::add, FlowDir rule
    // install, ICMP registration. One call replaces ~50 lines of glue.
    using Stream = edpdk::DpdkTcpStream<ec::WsCodec, /*EnableTls=*/true>;

    edpdk::StreamConfig scfg{};
    scfg.dpdk.wire.tuple.src_ip = cfg.local_ip;
    scfg.dpdk.wire.tuple.dst_ip = dst_ip;
    scfg.dpdk.wire.tuple.dst_port = cfg.port;
    // src_port left 0 — create_and_attach allocates an ephemeral one.
    scfg.dpdk.wire.src_mac     = src_mac;
    scfg.dpdk.wire.dst_mac     = gw_mac;
    scfg.dpdk.wire.port_id     = port_id;
    scfg.dpdk.wire.tx_queue_id = 0;
    scfg.dpdk.wire.rx_queue_id = 0;
    scfg.dpdk.wire.mss         = ed::net::kDefaultMss;
    scfg.dpdk.wire.recv_window = 65535;
    scfg.dpdk.pool       = pool;
    scfg.connect_timeout = 5s;
    scfg.tls.hostname    = cfg.host;     // SNI
    scfg.ws.path         = cfg.ws_path;  // RFC 6455 Upgrade target
    scfg.ws.host         = cfg.host;
    scfg.ws.timeout      = 10s;
    scfg.reasm_capacity  = 256 * 1024;

    auto sr = Stream::create_and_attach(std::move(scfg), platform);
    if (!sr) {
        spdlog::error("DpdkTcpStream::create_and_attach failed: {}",
                      sr.error().detail);
        return 8;
    }
    auto stream = std::move(*sr);

    uint64_t frames = 0;
    stream->on_message = [&frames](std::span<const uint8_t> app_frame) {
        ++frames;
        if ((frames & 0xFF) == 1) {
            std::string_view view{
                reinterpret_cast<const char*>(app_frame.data()),
                app_frame.size()};
            spdlog::info("[#{:>6}] {:.80}", frames, view);
        }
    };

    // ── 7) Burst-poll loop until duration / signal ────────────────────────
    spdlog::info("simple_hft: connected, polling for {}s", cfg.duration_s);
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::seconds{cfg.duration_s};
    while (g_running.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline
           && stream->state() == eph::net::TcpState::Established) {
        (void)poller->poll();
    }

    // ── 8) Final metrics snapshot — direct read at exit ───────────────────
    spdlog::info("simple_hft: frames={} bytes_sent={} bytes_recv={} "
                 "frames_decoded={} reasm_overflows={} codec_errors={}",
                 frames,
                 stream->metric(eph::net::StreamMetric::kBytesSent),
                 stream->metric(eph::net::StreamMetric::kBytesRecv),
                 stream->metric(eph::net::StreamMetric::kFramesDecoded),
                 stream->metric(eph::net::StreamMetric::kReasmOverflows),
                 stream->metric(eph::net::StreamMetric::kCodecErrors));

    // RAII: ~Stream → ~DpdkPoller → ~Platform → eal_cleanup.
    return 0;
}
