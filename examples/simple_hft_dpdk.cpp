/// @file simple_hft_dpdk.cpp
///
/// DPDK kernel-bypass example: one DpdkPoller<> drives multiple Pollables
/// on a single lcore loop.
///
/// Scope of this demo: plain TCP (`DpdkTcpStream<C, /*EnableTls=*/false>`)
/// with a hand-built `StreamConfig::dpdk::tcp_low_level` tuple and the strict
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
/// EAL bring-up: `--pin lcore_id=cpu_id[:role]` (repeatable) is the typed
/// entry point, going through `EalGuard::init_with_pins` (validates each
/// pin and registers cpus into the process-wide pin registry BEFORE
/// `rte_eal_init` fires). `--lcores '<raw EAL spec>'` (e.g. `(0-3)@(4-7)`)
/// is the escape hatch for syntax `LcorePin` cannot express; mutually
/// exclusive with `--pin` in one call. See `simple_hft_dpdk_mp.cpp` for
/// the same pattern with primary/secondary.
///
/// Usage:
///   sudo ./simple_hft_dpdk --
///        --pci 0000:28:00.0 --pin 0=4 --pin 1=5
///        --dst-ip 10.0.0.20 --dst-port 30000
///
///   # raw escape hatch (range / set-of-sets):
///   sudo ./simple_hft_dpdk --
///        --pci 0000:28:00.0 --lcores '4-7'
///        --dst-port 30000

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

#include <rte_ether.h>
#include <rte_mempool.h>

#include "eph/codec/raw_stream_codec.hpp"
#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/lcore_pin.hpp"   // LcorePin / EalGuard::init_with_pins
#include "eph/dpdk/packet_core.hpp"  // net::kDefaultMss
#include "eph/net/dpdk/poller.hpp"
#include "eph/net/dpdk/tcp_stream.hpp"
#include "eph/utils/cpu.hpp"        // CpuPinPolicy

namespace edpdk = eph::net::dpdk;
namespace ed    = eph::dpdk;
namespace ec    = eph::codec;
using namespace std::chrono_literals;

static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running.store(false, std::memory_order_release); }

// Helper: split argv at the first "--" marker so EAL/app args are kept
// separate. Everything before "--" is currently unused (the EAL config is
// driven entirely from the post-`--` flags) but the marker is preserved so
// existing wrapper scripts keep working.
static int split_eal(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--") return i;
    }
    return argc;
}

// Parse a single `--pin` token: `lcore=cpu` or `lcore=cpu:role`.
// Mirrors `simple_hft_dpdk_mp.cpp::parse_pin_spec`.
static std::expected<ed::LcorePin, std::string>
parse_pin_spec(std::string_view s) {
    auto eq = s.find('=');
    if (eq == std::string_view::npos || eq == 0 || eq + 1 == s.size()) {
        return std::unexpected(std::string{
            "--pin: expected 'lcore=cpu[:role]', got '"} + std::string{s} + "'");
    }
    auto col = s.find(':', eq + 1);
    auto cpu_end = (col == std::string_view::npos) ? s.size() : col;
    auto lcore_str = std::string(s.substr(0, eq));
    auto cpu_str   = std::string(s.substr(eq + 1, cpu_end - eq - 1));
    int  lcore     = std::atoi(lcore_str.c_str());
    int  cpu       = std::atoi(cpu_str.c_str());
    if (lcore < 0 || cpu < 0) {
        return std::unexpected(std::string{
            "--pin: lcore_id and cpu_id must be non-negative, got '"}
            + std::string{s} + "'");
    }
    std::string role = (col == std::string_view::npos)
                           ? std::string{}
                           : std::string(s.substr(col + 1));
    return ed::LcorePin{static_cast<uint16_t>(lcore), cpu, std::move(role)};
}

int main(int argc, char** argv) {
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    spdlog::set_level(spdlog::level::info);

    // ── 1) Parse app args (everything after "--") ─────────────────────────
    const int eal_end = split_eal(argc, argv);
    int      app_argc = argc - (eal_end == argc ? eal_end : eal_end + 1);
    char**   app_argv = argv + (eal_end == argc ? eal_end : eal_end + 1);

    uint16_t dpdk_port_id = 0;
    uint32_t src_ip       = 0x0A000010;  // 10.0.0.16
    uint32_t dst_ip       = 0x0A000020;  // 10.0.0.32
    uint16_t src_port     = 32768;
    uint16_t dst_port     = 30000;
    std::string              pci_bdf;
    std::vector<ed::LcorePin> pins;
    std::string              lcores_raw;

    for (int i = 0; i < app_argc; ++i) {
        std::string_view a = app_argv[i];
        if      (a == "--dpdk-port" && i + 1 < app_argc) dpdk_port_id = static_cast<uint16_t>(std::atoi(app_argv[++i]));
        else if (a == "--src-port"  && i + 1 < app_argc) src_port     = static_cast<uint16_t>(std::atoi(app_argv[++i]));
        else if (a == "--dst-port"  && i + 1 < app_argc) dst_port     = static_cast<uint16_t>(std::atoi(app_argv[++i]));
        else if (a == "--pci"       && i + 1 < app_argc) pci_bdf      = app_argv[++i];
        else if (a == "--lcores"    && i + 1 < app_argc) lcores_raw   = app_argv[++i];
        else if (a == "--pin"       && i + 1 < app_argc) {
            auto p = parse_pin_spec(app_argv[++i]);
            if (!p) {
                spdlog::error("simple_hft_dpdk: {}", p.error());
                return 1;
            }
            pins.push_back(std::move(*p));
        }
    }

    // ── 2) EAL init via lcore_pin.hpp typed path (or raw escape hatch) ────
    const bool typed_pins = !pins.empty();
    const bool raw_lcores = !lcores_raw.empty();
    if (typed_pins && raw_lcores) {
        spdlog::error("simple_hft_dpdk: --pin and --lcores are mutually "
                      "exclusive (typed vs raw EAL paths); pick one");
        return 1;
    }
    if (!typed_pins && !raw_lcores) {
        spdlog::error("simple_hft_dpdk: provide either --pin lcore=cpu[:role] "
                      "(preferred typed path) or --lcores '<raw EAL spec>'");
        return 1;
    }

    ed::EalConfig eal_cfg{};
    eal_cfg.program_name = "simple_hft_dpdk";
    if (!pci_bdf.empty()) eal_cfg.allowed_devs = {pci_bdf};
    if (raw_lcores)       eal_cfg.lcores       = {lcores_raw};

    std::expected<ed::EalGuard, std::string> eal = std::unexpected(std::string{});
    if (typed_pins) {
        spdlog::info("simple_hft_dpdk: EAL init via init_with_pins ({} pin(s))",
                     pins.size());
        eal = ed::EalGuard::init_with_pins(eal_cfg, pins,
                                           eph::utils::CpuPinPolicy{});
    } else {
        spdlog::info("simple_hft_dpdk: EAL init via raw lcores='{}' "
                     "(legacy path; consider --pin for typed validation)",
                     lcores_raw);
        auto argv_owned = ed::build_eal_argv(eal_cfg);
        std::vector<char*> argv_ptrs;
        argv_ptrs.reserve(argv_owned.size());
        for (auto& s : argv_owned) argv_ptrs.push_back(s.data());
        eal = ed::EalGuard::init(static_cast<int>(argv_ptrs.size()),
                                 argv_ptrs.data());
    }
    if (!eal) {
        spdlog::error("simple_hft_dpdk: EAL init failed: {}", eal.error());
        return 2;
    }

    // ── 3) Build the Poller (one lcore burst loop for the app) ────────────
    // NOTE: DpdkPoller does not spawn its own thread — the user's main
    // loop drives `poll()`. The application thread is *not* an EAL lcore
    // and is not pinned by `init_with_pins`; if you want it pinned, call
    // `eph::utils::pin_thread()` separately (see binance_latency.cpp).
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
