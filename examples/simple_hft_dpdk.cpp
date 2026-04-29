/// @file simple_hft_dpdk.cpp
///
/// DPDK kernel-bypass example: one DpdkPoller<> drives multiple Pollables
/// on a single lcore loop.
///
/// Scope of this demo: plain TCP (`DpdkTcpStream<C, /*EnableTls=*/false>`)
/// with a hand-built `StreamConfig::legacy` tuple and the strict
/// `DpdkTcpStream::create(cfg)` factory. It is intentionally a *skeleton* —
/// `scfg.dpdk.pool` is left null so `create` fails fast with `InvalidConfig` on
/// a smoke-boot without a real mempool, and the rest of the code
/// demonstrates the error surface only.
///
/// For a turnkey production factory that owns queue selection, src-port
/// allocation, TLS 1.3 handshake (aws-lc), WS upgrade, Poller attach,
/// FlowDirector rule install and ICMP path-MTU registration in a single
/// call, see `DpdkTcpStream<C, true>::create_and_attach(cfg, platform)` as
/// used in `examples/binance_latency.cpp`.
///
/// For single-NIC multi-process (primary + secondary on one card via
/// shared hugepage), see `examples/simple_hft_dpdk_mp.cpp` and
/// `eph-net-dpdk/docs/dpdk-multiprocess.md`.
///
/// System libdpdk is linked directly (the previous vcpkg-openssl ↔ aws-lc
/// TU clash no longer applies — see CLAUDE.md "Build"), so
/// `DpdkTcpStream<C, true>` is fully supported; this file stays plain-TCP
/// only to keep the example minimal.
///
/// Usage:
///   sudo ./simple_hft_dpdk -a 0000:28:00.0 -l 4-7 --
///        --dst-ip 10.0.0.20 --dst-port 30000

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

#include <rte_ether.h>
#include <rte_mempool.h>

#include "eph/codec/raw_stream_codec.hpp"
#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/packet_core.hpp"  // net::kDefaultMss
#include "eph/net/dpdk/poller.hpp"
#include "eph/net/dpdk/tcp_stream.hpp"

namespace edpdk = eph::net::dpdk;
namespace ec    = eph::codec;
using namespace std::chrono_literals;

static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running.store(false, std::memory_order_release); }

// Helper: split argv at the first "--" marker so EAL gets the EAL args.
static int split_eal(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--") return i;
    }
    return argc;
}

int main(int argc, char** argv) {
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    spdlog::set_level(spdlog::level::info);

    // ── 1) EAL init ───────────────────────────────────────────────────────
    const int eal_end = split_eal(argc, argv);
    auto eal = eph::dpdk::EalGuard::init(eal_end, argv);
    if (!eal) {
        spdlog::error("simple_hft_dpdk_v3: EAL init failed: {}", eal.error());
        return 1;
    }
    int      app_argc = argc - (eal_end == argc ? eal_end : eal_end + 1);
    char**   app_argv = argv + (eal_end == argc ? eal_end : eal_end + 1);

    // ── 2) Parse app args ─────────────────────────────────────────────────
    uint16_t dpdk_port_id = 0;
    uint32_t src_ip       = 0x0A000010;  // 10.0.0.16
    uint32_t dst_ip       = 0x0A000020;  // 10.0.0.32
    uint16_t src_port     = 32768;
    uint16_t dst_port     = 30000;
    int      lcore_hint   = -1;

    for (int i = 0; i < app_argc; ++i) {
        std::string_view a = app_argv[i];
        if      (a == "--dpdk-port" && i + 1 < app_argc) dpdk_port_id = static_cast<uint16_t>(std::atoi(app_argv[++i]));
        else if (a == "--src-port"  && i + 1 < app_argc) src_port     = static_cast<uint16_t>(std::atoi(app_argv[++i]));
        else if (a == "--dst-port"  && i + 1 < app_argc) dst_port     = static_cast<uint16_t>(std::atoi(app_argv[++i]));
        else if (a == "--lcore"     && i + 1 < app_argc) lcore_hint   = std::atoi(app_argv[++i]);
    }

    // ── 3) Build the Poller (one lcore burst loop for the app) ────────────
    // NOTE: DpdkPoller does not spawn its own thread — the user's main
    // loop drives `poll()`. Thread pinning (e.g. to `lcore_hint`) is the
    // caller's responsibility; see eph::utils::pin_thread().
    (void)lcore_hint;
    edpdk::PollerConfig pcfg{};
    pcfg.port_id     = dpdk_port_id;
    pcfg.rx_queue_id = 0;

    auto poller_r = edpdk::DpdkPoller<>::create(pcfg);
    if (!poller_r) {
        spdlog::error("DpdkPoller::create failed: {}", poller_r.error().detail);
        return 2;
    }
    auto poller = std::move(*poller_r);

    // ── 4) Build the TCP stream (PLAIN — see file header re: TLS) ────────
    rte_mempool* pool = nullptr;  // a real app would create one with
                                  // rte_pktmbuf_pool_create() before this
                                  // call. We pass nullptr for the smoke
                                  // boot — DpdkTcpStream::create will reject
                                  // it with InvalidConfig and the rest of
                                  // the demo demonstrates the error
                                  // surface only.

    edpdk::StreamConfig scfg{};
    scfg.dpdk.tcp_low_level.tuple.src_ip   = src_ip;
    scfg.dpdk.tcp_low_level.tuple.dst_ip   = dst_ip;
    scfg.dpdk.tcp_low_level.tuple.src_port = src_port;
    scfg.dpdk.tcp_low_level.tuple.dst_port = dst_port;
    scfg.dpdk.tcp_low_level.port_id        = dpdk_port_id;
    scfg.dpdk.tcp_low_level.tx_queue_id    = 0;
    scfg.dpdk.tcp_low_level.rx_queue_id    = 0;
    scfg.dpdk.tcp_low_level.mss            = eph::dpdk::net::kDefaultMss;
    scfg.dpdk.tcp_low_level.recv_window    = 65535;
    scfg.dpdk.pool                  = pool;
    scfg.connect_timeout       = 3s;

    using Stream = edpdk::DpdkTcpStream<ec::RawStreamCodec, /*Tls=*/false>;
    auto sr = Stream::create(std::move(scfg));
    if (!sr) {
        spdlog::warn(
            "simple_hft_dpdk_v3: DpdkTcpStream::create failed (expected on a "
            "smoke-boot without a real mempool): {}", sr.error().detail);
        spdlog::info("simple_hft_dpdk: populate `scfg.dpdk.pool` with a real "
                     "rte_pktmbuf_pool_create() output to drive the loop.");
        return 0;
    }
    auto stream = std::move(*sr);

    stream->on_message = [](std::span<const uint8_t> app_frame) {
        spdlog::info("[dpdk] rx {} bytes", app_frame.size());
    };

    if (auto r = poller->add(stream.get()); !r) {
        spdlog::error("poller->add failed: {}", r.error().detail);
        return 3;
    }

    // ── 5) Drive the burst loop ───────────────────────────────────────────
    spdlog::info("simple_hft_dpdk_v3: entering burst-poll loop");
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (g_running.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        (void)poller->poll();
    }

    spdlog::info("simple_hft_dpdk_v3: exiting");
    return 0;
}
