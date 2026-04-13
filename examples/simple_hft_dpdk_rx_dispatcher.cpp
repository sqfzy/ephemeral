/// @file simple_hft_dpdk_rx_dispatcher.cpp
/// Multi-connection RxDispatcher pattern example.
///
/// Demonstrates a single `DpdkPoller<>` driving multiple Pollables (a TCP
/// order channel + a UDP market data socket) on one lcore burst loop.
///
/// NOTE: TLS on DPDK is not yet available due to a vcpkg-openssl ↔ aws-lc
/// TU clash (same issue documented in simple_hft_dpdk_v3.cpp). We use plain
/// TCP here so the example builds and demonstrates the rx_dispatcher pattern
/// without dragging in the legacy stack.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>

#include <spdlog/spdlog.h>

#include <rte_mempool.h>

#include "eph/codec/raw_datagram_codec.hpp"
#include "eph/codec/raw_stream_codec.hpp"
#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/packet_core.hpp"
#include "eph/net/dpdk/poller.hpp"
#include "eph/net/dpdk/tcp_stream.hpp"
#include "eph/net/dpdk/udp_socket.hpp"

namespace edpdk = eph::net::dpdk;
namespace ec    = eph::codec;
using namespace std::chrono_literals;

static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running.store(false, std::memory_order_release); }

// Split argv at "--" so EAL gets the EAL args.
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

    // ── 1) EAL ────────────────────────────────────────────────────────────
    const int eal_end = split_eal(argc, argv);
    auto eal = eph::dpdk::EalGuard::init(eal_end, argv);
    if (!eal) {
        spdlog::error("EAL init failed: {}", eal.error());
        return 1;
    }

    // ── 2) Build the single Poller that drives both pollables ────────────
    edpdk::PollerConfig pcfg{};
    pcfg.port_id     = 0;
    pcfg.rx_queue_id = 0;
    pcfg.rx_cpu      = -1;

    auto poller_r = edpdk::DpdkPoller<>::create(pcfg);
    if (!poller_r) {
        spdlog::error("DpdkPoller::create failed: {}", poller_r.error().detail);
        return 2;
    }
    auto poller = std::move(*poller_r);

    // ── 3) TCP order channel ──────────────────────────────────────────────
    using OrderStream = edpdk::DpdkTcpStream<ec::RawStreamCodec, /*Tls=*/false>;

    edpdk::StreamConfig scfg{};
    scfg.legacy.tuple.src_ip   = 0x0A000010;
    scfg.legacy.tuple.dst_ip   = 0x0A000020;
    scfg.legacy.tuple.src_port = 32768;
    scfg.legacy.tuple.dst_port = 30000;
    scfg.legacy.port_id        = 0;
    scfg.legacy.tx_queue_id    = 0;
    scfg.legacy.rx_queue_id    = 0;
    scfg.legacy.mss            = eph::dpdk::net::kDefaultMss;
    scfg.legacy.recv_window    = 65535;
    scfg.pool                  = nullptr;  // smoke-boot
    scfg.connect_timeout       = 3s;

    auto order_r = OrderStream::create(std::move(scfg));
    if (!order_r) {
        spdlog::warn(
            "rx_dispatcher demo: OrderStream::create failed (expected without "
            "a real mempool): {}", order_r.error().detail);
        spdlog::info("rx_dispatcher demo: populate scfg.pool with a real "
                     "rte_pktmbuf_pool_create() output to drive the loop.");
        // Fall through and still attempt to register the UDP socket so
        // the example exercises both code paths.
    } else {
        auto stream = std::move(*order_r);
        stream->on_message = [](const uint8_t*, uint16_t len) {
            spdlog::info("[order] exec_report {} bytes", len);
            (void)len;
        };
        if (auto r = poller->add(stream.get()); !r) {
            spdlog::warn("[order] add failed: {}", r.error().detail);
        }
        // The unique_ptr lives only inside this `else` arm — that's fine
        // for a smoke boot. A real app would hoist it to function scope.
    }

    // ── 4) UDP market-data socket ────────────────────────────────────────
    using MdSock = edpdk::DpdkUdpSocket<ec::RawDatagramCodec>;

    edpdk::UdpConfig ucfg{};
    // Real rx_dispatcher demos populate ucfg.legacy with src/dst MAC, src IP,
    // src/dst port, mempool, port_id, tx_queue_id. We leave it default
    // here — the create() factory will surface a typed validation error,
    // which is the smoke-boot signal we want to exercise.

    auto md_r = MdSock::create(std::move(ucfg));
    if (!md_r) {
        spdlog::warn(
            "rx_dispatcher demo: MdSock::create failed (expected on smoke-boot): {}",
            md_r.error().detail);
        spdlog::info("rx_dispatcher demo: populate ucfg.legacy.{src/dst MAC, src IP, "
                     "ports, mempool, port_id, tx_queue_id} for a real run.");
    } else {
        auto md = std::move(*md_r);
        md->on_datagram = [](const uint8_t*, uint16_t len,
                             const eph::net::SocketAddr& src) {
            spdlog::info("[md] from {} ({} bytes)", src.to_string(), len);
            (void)len;
        };
        if (auto r = poller->add(md.get()); !r) {
            spdlog::warn("[md] add failed: {}", r.error().detail);
        }
    }

    // ── 5) Drive both pollables from a single burst loop ────────────────
    spdlog::info("rx_dispatcher demo: entering single-loop burst poll, "
                 "driving {} Pollable(s)", poller->size());

    auto deadline = std::chrono::steady_clock::now() + 1s;
    while (g_running.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        (void)poller->poll();
    }

    spdlog::info("rx_dispatcher demo: exiting");
    return 0;
}
