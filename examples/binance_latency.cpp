/// @file binance_latency.cpp
///
/// Binance WebSocket recv-to-event latency probe, **DPDK datapath**.
///
///   recv_ns (CLOCK_REALTIME on message arrival) − T_ms * 1'000'000
///
/// Bypasses the kernel TCP/IP stack entirely: uses `eph::net::dpdk::DpdkTcpStream`
/// driven from a single-lcore `DpdkPoller` burst loop. Handles TLS 1.3 via
/// aws-lc and the WebSocket upgrade through the same StreamConfig knobs as
/// the kernel variant. All DPDK wiring (EAL, port, mempool, src/gw MAC,
/// destination IP) is resolved on startup via the helpers in
/// `eph-net-dpdk/include/eph/dpdk/{eal,platform,arp,dns}.hpp`.
///
/// Full JSON payloads are emitted at spdlog DEBUG. The event-loop thread
/// is the EAL main lcore (lcore 0); its CPU affinity is set by
/// `rte_eal_init` based on `--pin 0=<cpu>`, and the cpu is registered into
/// the process-wide pin registry by typed `EalGuard::init` BEFORE EAL runs (so
/// any later `eph::utils::pin_thread` collision surfaces loudly). No
/// separate application-thread pin is needed.
///
/// EAL bring-up uses `lcore_pin.hpp`: `--pin lcore_id=cpu_id[:role]`
/// (repeatable) goes through typed `EalGuard::init`, which validates
/// each pin against `CpuPinPolicy` (warn_irq_overlap is enabled here so
/// IRQ-affinity overlap on the WS lcore is logged). `--lcores '<raw EAL
/// spec>'` is the escape hatch for syntax `LcorePin` cannot express
/// (set-of-sets / coremask); mutually exclusive with `--pin` in one call.
///
/// Reconnect behaviour: the stream lifecycle is wrapped in an
/// `eph::utils::ExponentialBackoff` exponential-backoff loop with ±25% jitter.
/// DNS / ARP / mempool / src MAC are resolved ONCE on startup and reused
/// across reconnects — re-resolving DNS on every drop would add noise to a
/// latency probe and the L2 gateway is stable on a single-segment LAN.
/// Latency samples and frame counters persist across reconnects so the
/// final report reflects the whole `--duration` window. Tuning knobs:
/// `--reconnect-initial-ms` / `--reconnect-max-ms` / `--reconnect-max-attempts`.
///
/// Usage:
///   sudo ./binance_latency --
///        --pci 0000:28:00.0 --pin 0=2:ws-latency
///        --local-ip 10.0.0.16 --gateway-ip 10.0.0.1
///        --host stream.binance.com [--dst-ip 52.193.255.60]
///        [--nameserver 8.8.8.8] [--dpdk-port 0]
///        [--src-port 40001] [--port 9443]
///        [--path /ws/btcusdt@aggTrade] [--duration 30]
///        [--log-level info]
///        [--reconnect-initial-ms 250] [--reconnect-max-ms 10000]
///        [--reconnect-max-attempts 0]
///
///   # raw escape hatch (range / set-of-sets):
///   sudo ./binance_latency --
///        --pci 0000:28:00.0 --lcores '2-3'
///        --local-ip 10.0.0.16 --gateway-ip 10.0.0.1 ...
///
/// `--dst-ip` skips the DPDK-native DNS path (useful to pin a known
/// TLS-1.3-capable Binance backend). When absent, `eph::dpdk::dns::resolve`
/// is used to look up `--host` through the configured nameserver, which
/// itself is reached via the ARP-resolved gateway MAC.
///
/// `--reconnect-max-attempts 0` means unlimited retries; the loop is
/// bounded by `--duration` and SIGINT / SIGTERM instead.

#include <pthread.h>
#include <time.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_mempool.h>

#include <spdlog/spdlog.h>

#include "eph/codec/ws_codec.hpp"
#include "eph/json/parser.hpp"

// DPDK-native helpers: EAL lifetime, port/mempool bring-up, ARP & DNS.
// (eph/dpdk/tcp.hpp is pulled in transitively by eph/net/dpdk/tcp_stream.hpp
//  — no need to include it directly here.)
#include "eph/dpdk/arp.hpp"
#include "eph/dpdk/cli.hpp"
#include "eph/dpdk/dns.hpp"
#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/lcore_pin.hpp"   // LcorePin / typed EalGuard::init
#include "eph/dpdk/packet_core.hpp"  // net::parse_ipv4, net::format_ipv4, kDefaultMss
#include "eph/dpdk/platform.hpp"

// DPDK stream / poller.
#include "eph/net/dpdk/poller.hpp"
#include "eph/net/dpdk/tcp_stream.hpp"
#include "eph/utils/backoff.hpp"

#include "eph/utils/cpu.hpp"
#include "eph/utils/recorder.hpp"
#include "eph/utils/time.hpp"

namespace edpdk = eph::net::dpdk;
namespace ed    = eph::dpdk;
namespace ec = eph::codec;
using namespace std::chrono_literals;

// Defaults target Binance's spot WebSocket endpoint. aggTrade carries a
// trade-time `T` field (ms since epoch) which is what we subtract from
// the receive timestamp to get arrival latency.
static constexpr const char* kDefaultHost = "stream.binance.com";
static constexpr std::uint16_t kDefaultPort = 9443;
static constexpr const char* kDefaultPath = "/ws/btcusdt@aggTrade";

static std::atomic<bool> g_running{true};
static void on_signal(int) {
    g_running.store(false, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Split argv at the first "--" marker so EAL args are cleanly separated
// from the application's own flags.
// ---------------------------------------------------------------------------
static int split_at_dash_dash(int argc, char** argv) noexcept {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view{argv[i]} == "--") return i;
    }
    return argc;
}

// Pretty-print an rte_ether_addr (used for logging only).
[[nodiscard]] static std::string mac_to_string(const rte_ether_addr& m) {
    char buf[18];
    std::snprintf(buf, sizeof(buf),
                  "%02x:%02x:%02x:%02x:%02x:%02x",
                  m.addr_bytes[0], m.addr_bytes[1], m.addr_bytes[2],
                  m.addr_bytes[3], m.addr_bytes[4], m.addr_bytes[5]);
    return buf;
}

// ---------------------------------------------------------------------------
// App configuration extracted from argv (the slice *after* the `--` marker).
// Uses host-byte-order for all IP fields to match `ConnectionTuple`.
// ---------------------------------------------------------------------------
struct AppConfig {
    std::string host = kDefaultHost;
    std::string ws_path = kDefaultPath;
    std::uint16_t port = kDefaultPort;
    std::uint16_t src_port = 0;  // 0 = ask DpdkPoller::pick_src_port
    int duration_s = 30;
    spdlog::level::level_enum log_level = spdlog::level::info;

    std::uint32_t local_ip = 0;        // required
    std::uint32_t gateway_ip = 0;      // required
    std::uint32_t dst_ip = 0;          // optional — 0 means "use DPDK DNS"
    std::uint32_t nameserver_ip = 0x08080808;  // 8.8.8.8 default

    // ExponentialBackoff knobs. Sane defaults for exchange-grade endpoints:
    // fast initial retry, capped at 10s, unlimited attempts (outer loop
    // is bounded by --duration and SIGINT).
    int reconnect_initial_ms = 250;
    int reconnect_max_ms     = 10000;
    std::uint32_t reconnect_max_attempts = 0;  // 0 = unlimited

    // EAL bring-up — --pci / --pin / --lcores / --dpdk-port (or --port-id
    // alias) routed through the shared cli helper.
    ed::cli::EalCliConfig eal{};
};

[[nodiscard]] static std::optional<AppConfig>
parse_app_args(int argc, char** argv) {
    AppConfig cfg{};
    auto parse_ip = [](const char* s, std::uint32_t& out) -> bool {
        std::uint32_t v = eph::dpdk::net::parse_ipv4(s);
        if (v == 0) return false;
        out = v;
        return true;
    };

    for (int i = 0; i < argc; ++i) {
        const std::string_view a = argv[i];
        const char* next = (i + 1 < argc) ? argv[i + 1] : nullptr;

        // EAL flags first (--pci / --pin / --lcores / --dpdk-port / --port-id).
        auto consumed = ed::cli::consume_one(cfg.eal, a, next);
        if (!consumed) {
            spdlog::error("{}", consumed.error());
            return std::nullopt;
        }
        if (*consumed > 0) { i += *consumed - 1; continue; }

        auto need = [&](const char* flag) {
            if (!next) {
                spdlog::error("{} requires an argument", flag);
                return false;
            }
            ++i;
            return true;
        };
        if (a == "--host" && need("--host")) {
            cfg.host = next;
        } else if (a == "--port" && need("--port")) {
            cfg.port = static_cast<std::uint16_t>(std::atoi(next));
        } else if (a == "--path" && need("--path")) {
            cfg.ws_path = next;
        } else if (a == "--duration" && need("--duration")) {
            cfg.duration_s = std::atoi(next);
        } else if (a == "--log-level" && need("--log-level")) {
            cfg.log_level = spdlog::level::from_str(next);
        } else if (a == "--local-ip" && need("--local-ip")) {
            if (!parse_ip(next, cfg.local_ip)) {
                spdlog::error("invalid --local-ip: {}", next);
                return std::nullopt;
            }
        } else if (a == "--gateway-ip" && need("--gateway-ip")) {
            if (!parse_ip(next, cfg.gateway_ip)) {
                spdlog::error("invalid --gateway-ip: {}", next);
                return std::nullopt;
            }
        } else if (a == "--dst-ip" && need("--dst-ip")) {
            if (!parse_ip(next, cfg.dst_ip)) {
                spdlog::error("invalid --dst-ip: {}", next);
                return std::nullopt;
            }
        } else if (a == "--nameserver" && need("--nameserver")) {
            if (!parse_ip(next, cfg.nameserver_ip)) {
                spdlog::error("invalid --nameserver: {}", next);
                return std::nullopt;
            }
        } else if (a == "--src-port" && need("--src-port")) {
            cfg.src_port = static_cast<std::uint16_t>(std::atoi(next));
        } else if (a == "--reconnect-initial-ms" && need("--reconnect-initial-ms")) {
            cfg.reconnect_initial_ms = std::atoi(next);
        } else if (a == "--reconnect-max-ms" && need("--reconnect-max-ms")) {
            cfg.reconnect_max_ms = std::atoi(next);
        } else if (a == "--reconnect-max-attempts" && need("--reconnect-max-attempts")) {
            cfg.reconnect_max_attempts = static_cast<std::uint32_t>(std::atoi(next));
        } else {
            spdlog::warn("ignoring unknown app argument: {}", a);
        }
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

// ---------------------------------------------------------------------------
// Latency-sample bookkeeping passed into the on_message callback. Keeping
// Recorder + counters in their own struct lets the callback stay small and
// makes reconnect-style refactors cheap.
// ---------------------------------------------------------------------------
struct SessionStats {
    eph::utils::Recorder recorder;
    std::uint64_t frames = 0;
    std::uint64_t parsed_ok = 0;
    std::uint64_t no_T_field = 0;
    std::uint64_t clock_skew_skipped = 0;

    explicit SessionStats(std::string name) : recorder(std::move(name)) {}
};

int main(int argc, char** argv) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

    // ── 1) Split argv at the "--" marker ───────────────────────────────────
    const int eal_end = split_at_dash_dash(argc, argv);
    const int app_start = (eal_end == argc) ? eal_end : eal_end + 1;
    const int app_argc = argc - app_start;
    char** const app_argv = argv + app_start;

    // Runtime log level stays info by default but becomes whatever the
    // caller passes via --log-level (compile-time level is DEBUG so the
    // payload dump macros stay live).
    auto app_cfg_opt = parse_app_args(app_argc, app_argv);
    if (!app_cfg_opt) return 1;
    AppConfig app_cfg = *app_cfg_opt;
    spdlog::set_level(app_cfg.log_level);

    // ── 2) EAL bring-up — validate exclusivity, require at least one path.
    if (auto v = ed::cli::validate(app_cfg.eal); !v) {
        spdlog::error("binance_latency: {}", v.error());
        return 1;
    }
    if (app_cfg.eal.pins.empty() && app_cfg.eal.lcores_raw.empty()) {
        spdlog::error("binance_latency: provide either --pin lcore=cpu[:role] "
                      "(preferred typed path) or --lcores '<raw EAL spec>'");
        return 1;
    }

    // Relaxed everything except IRQ overlap — preserves the warning
    // surfaced by the previous standalone `pin_thread(... warn_irq_overlap=
    // true)` call. validate_pin_policy runs against every LcorePin inside
    // pin_lcores (called by launch) so the warning fires on the
    // WS event-loop lcore.
    eph::utils::CpuPinPolicy pin_policy{
        .require_isolcpus            = false,
        .require_no_sibling_conflict = false,
        .require_same_numa           = false,
        .warn_irq_overlap            = true,
    };

    // ── 2) Platform::create — application secondary attach ────────
    // Operator must already have started the daemon for this NIC:
    //
    //   sudo systemctl start eph-nicd@<pci>.service
    //
    // The mbuf pool size lives on the daemon's NicServiceConfig now;
    // application-side PlatformConfig only carries `pci`, `queues`, and
    // EAL knobs. ~Platform releases the secondary attach atomically.
    //
    // Dev-mode shortcut: `dev::ensure_local_daemon(pci)` from
    // `eph/dpdk/dev_helpers.hpp` will fork+exec a daemon if absent —
    // intentionally not invoked here so this example exercises the
    // production-style "operator already started the daemon" flow. Add
    // one include + one call site if you want auto-spawn in dev runs.
    if (app_cfg.eal.pci.empty()) {
        spdlog::error("binance_latency: --pci is required");
        return 1;
    }
    eph::dpdk::PlatformConfig pcfg{};
    pcfg.pci          = app_cfg.eal.pci.front();
    pcfg.queues       = 1;
    pcfg.pins         = std::span<ed::LcorePin const>{app_cfg.eal.pins};
    pcfg.pin_policy   = pin_policy;
    pcfg.lcores       = app_cfg.eal.lcores_raw.empty()
                            ? std::vector<std::string>{}
                            : std::vector<std::string>{app_cfg.eal.lcores_raw};
    pcfg.program_name = "binance_latency";

    if (!app_cfg.eal.pins.empty()) {
        spdlog::info("binance_latency: Platform::create "
                     "(typed pins, {} pin(s))", app_cfg.eal.pins.size());
    } else {
        spdlog::info("binance_latency: Platform::create "
                     "(raw lcores='{}')", app_cfg.eal.lcores_raw);
    }

    auto plat = eph::dpdk::Platform::create(std::move(pcfg));
    if (!plat) {
        spdlog::error("Platform::create failed: {} "
                      "(is `eph-nicd@{}.service` running?)",
                      plat.error(), app_cfg.eal.pci.front());
        return 2;
    }

    // ── 3) Thread name + TSC bring-up ─────────────────────────────────────
    // The main thread's CPU affinity is set by `rte_eal_init` to the EAL
    // main lcore's cpu (via the typed-pin path inside Platform::create).
    // No separate pin call needed.
    pthread_setname_np(pthread_self(), "ws-latency");
    if (!eph::utils::TSC::is_initialized() && !eph::utils::TSC::init()) {
        spdlog::error("TSC::init failed — recorder would be unusable");
        return 3;
    }

    const uint16_t port_id = plat->port_id();
    rte_mempool* const pool = plat->mempool();

    // ── 5) Local MAC (from the NIC itself) ────────────────────────────────
    rte_ether_addr src_mac{};
    if (int rc = rte_eth_macaddr_get(port_id, &src_mac); rc != 0) {
        spdlog::error("rte_eth_macaddr_get failed: rc={}", rc);
        return 5;
    }
    spdlog::info("local NIC port_id={} src_mac={} src_ip={}", port_id,
                 mac_to_string(src_mac),
                 eph::dpdk::net::format_ipv4(app_cfg.local_ip).data());

    // ── 6) ARP-resolve the gateway MAC ─────────────────────────────────────
    auto gw_mac_r = eph::dpdk::arp::resolve(
        port_id, pool, src_mac, app_cfg.local_ip,
        app_cfg.gateway_ip, std::chrono::seconds{3});
    if (!gw_mac_r) {
        spdlog::error("ARP resolve gateway {} failed: {}",
                      eph::dpdk::net::format_ipv4(app_cfg.gateway_ip).data(),
                      gw_mac_r.error());
        return 6;
    }
    const rte_ether_addr gw_mac = *gw_mac_r;
    spdlog::info("gateway {} -> mac {}",
                 eph::dpdk::net::format_ipv4(app_cfg.gateway_ip).data(),
                 mac_to_string(gw_mac));

    // ── 7) Destination IP: DPDK-native DNS if not pinned via --dst-ip ─────
    //
    // eph::dpdk::dns::resolve builds the DNS query packet on top of the
    // same L2 path (src_mac -> gw_mac) and talks to the configured
    // nameserver via UDP/53, parsing the first A record from the reply.
    std::uint32_t dst_ip = app_cfg.dst_ip;
    if (dst_ip == 0) {
        eph::dpdk::dns::DnsConfig dns_cfg{};
        dns_cfg.nameserver_ip = app_cfg.nameserver_ip;
        dns_cfg.timeout = 3s;
        auto dns_r = eph::dpdk::dns::resolve(
            port_id, /*queue=*/0, pool, src_mac, gw_mac, app_cfg.local_ip,
            app_cfg.host, dns_cfg);
        if (!dns_r) {
            spdlog::error("DPDK DNS resolve '{}' via {} failed: {}",
                          app_cfg.host,
                          eph::dpdk::net::format_ipv4(app_cfg.nameserver_ip).data(),
                          dns_r.error());
            return 7;
        }
        dst_ip = *dns_r;
        spdlog::info("DPDK DNS: {} -> {}", app_cfg.host,
                     eph::dpdk::net::format_ipv4(dst_ip).data());
    } else {
        spdlog::info("--dst-ip pinned: {}",
                     eph::dpdk::net::format_ipv4(dst_ip).data());
    }

    // ── 8) Build the DpdkPoller (burst-poll loop driver) ───────────────────
    edpdk::PollerConfig poller_cfg{};
    poller_cfg.port_id = port_id;
    poller_cfg.rx_queue_id = 0;
    auto poller_r = edpdk::DpdkPoller<>::create(poller_cfg);
    if (!poller_r) {
        spdlog::error("DpdkPoller::create failed: {}",
                      poller_r.error().detail);
        return 8;
    }
    auto poller = std::move(*poller_r);

    // ── 9) Reconnect policy + stats (persist across sessions) ────────────
    //
    // ExponentialBackoff is just exponential-backoff math — no sleep, no
    // connect. The outer loop below drives the actual connect / drop
    // cycle. On a clean drop (FIN, close handshake, TLS alert) the
    // stream leaves Established, the inner poll loop exits, we destroy
    // the stream, and the outer loop picks a fresh ephemeral src port
    // and tries again. `policy.reset()` is called after each successful
    // connect so a later drop starts backoff fresh instead of inheriting
    // the previous chain's delay. (3 distinct failure points each drive
    // their own backoff, so this stays a direct backoff loop rather than a
    // single eph::utils::retry call.)
    eph::utils::ExponentialBackoff policy{eph::utils::ExponentialBackoff::Config{
        .initial_backoff = std::chrono::milliseconds{app_cfg.reconnect_initial_ms},
        .max_backoff     = std::chrono::milliseconds{app_cfg.reconnect_max_ms},
        .multiplier      = 2.0,
        .jitter_factor   = 0.25,
        .max_attempts    = app_cfg.reconnect_max_attempts,  // 0 = unlimited
    }};

    using Stream = edpdk::DpdkTcpStream<ec::WsCodec, /*EnableTls=*/true>;
    SessionStats stats{"binance_recv_minus_T_ns"};

    // on_message captures `stats` by reference so counters and the
    // Recorder accumulate across reconnects — the final report reflects
    // the whole --duration window.
    auto on_message_handler = [&stats](std::span<const std::uint8_t> app_frame) {
        // Wall-clock receive timestamp (matches Binance's T which is
        // wall-clock ms since epoch). Mixing TSC with realtime would
        // produce garbage diffs.
        timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);
        const std::uint64_t recv_ns =
            static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
            static_cast<std::uint64_t>(ts.tv_nsec);

        ++stats.frames;
        const auto* data = app_frame.data();
        const auto  len  = app_frame.size();

        // Full payload dump for offline debugging. Compile-time gated by
        // SPDLOG_ACTIVE_LEVEL on this target — build with
        // SPDLOG_LEVEL_DEBUG (default) to keep the call live.
        SPDLOG_DEBUG(
            "rx frame #{} len={} payload={}", stats.frames, len,
            std::string_view{reinterpret_cast<const char*>(data), len});

        auto jv = eph::json::parse(data, len);
        if (!jv) [[unlikely]] return;

        auto T_ms = jv->get_int("T");
        if (!T_ms) [[unlikely]] {
            ++stats.no_T_field;
            return;
        }
        ++stats.parsed_ok;

        const std::uint64_t T_ns =
            static_cast<std::uint64_t>(*T_ms) * 1'000'000ULL;
        if (recv_ns <= T_ns) [[unlikely]] {
            // Cross-machine wall-clock skew: this host's clock is
            // behind the exchange, so the subtraction would underflow.
            ++stats.clock_skew_skipped;
            return;
        }
        stats.recorder.record_ns(recv_ns - T_ns);
    };

    // ── 10) Reconnect loop ────────────────────────────────────────────────
    //
    // Deadline is wall-clock from startup: time spent in backoff / TLS
    // handshake on reconnect eats into the measurement budget by design
    // (failed connects ARE a latency event the operator should see in
    // the final report).
    //
    // DNS / ARP / mempool / src MAC are resolved above and reused — we
    // do NOT re-resolve them on reconnect: DNS is slow and would add
    // noise to a latency probe, and the L2 gateway is stable.
    spdlog::info(
        "binance_latency (dpdk): host={} -> dst={}:{} path={} "
        "duration={}s reconnect[initial={}ms max={}ms attempts={}]",
        app_cfg.host,
        eph::dpdk::net::format_ipv4(dst_ip).data(), app_cfg.port,
        app_cfg.ws_path, app_cfg.duration_s,
        app_cfg.reconnect_initial_ms, app_cfg.reconnect_max_ms,
        app_cfg.reconnect_max_attempts);

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{app_cfg.duration_s};
    std::uint64_t session_count = 0;
    bool warned_src_port_override = false;

    while (g_running.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {

        // ── 10a) Fresh ephemeral src port per attempt ───────────────────
        // DPDK has no kernel ephemeral-port allocator, so DpdkPoller
        // does the [32768, 60999] random scan itself. `--src-port N`
        // is a soft preference; on conflict the scan picks something
        // else and we warn once.
        auto src_port_r = poller->pick_src_port(
            app_cfg.local_ip, dst_ip, app_cfg.port,
            /*range_begin=*/32768, /*range_end=*/60999,
            /*preferred=*/app_cfg.src_port);
        if (!src_port_r) {
            const auto delay = policy.next_delay();
            if (!delay) break;  // backoff budget exhausted
            spdlog::warn("pick_src_port failed: {} — retry in {}ms (attempt {})",
                         src_port_r.error().detail, delay->count(),
                         policy.attempts());
            std::this_thread::sleep_for(*delay);
            continue;
        }
        const std::uint16_t src_port = *src_port_r;
        if (!warned_src_port_override && app_cfg.src_port != 0 &&
            src_port != app_cfg.src_port) {
            spdlog::warn("--src-port {} rejected; allocated {} instead",
                         app_cfg.src_port, src_port);
            warned_src_port_override = true;
        }

        // ── 10b) Build StreamConfig and run create() — this does the
        //        TCP handshake, TLS 1.3 handshake, and WS Upgrade all in
        //        one call. Failure here is what the reconnect loop is
        //        here for.
        edpdk::StreamConfig scfg{};
        scfg.dpdk.wire.tuple.src_ip = app_cfg.local_ip;
        scfg.dpdk.wire.tuple.dst_ip = dst_ip;
        scfg.dpdk.wire.tuple.src_port = src_port;
        scfg.dpdk.wire.tuple.dst_port = app_cfg.port;
        scfg.dpdk.wire.src_mac = src_mac;
        scfg.dpdk.wire.dst_mac = gw_mac;
        scfg.dpdk.wire.port_id = port_id;
        scfg.dpdk.wire.tx_queue_id = 0;
        scfg.dpdk.wire.rx_queue_id = 0;
        scfg.dpdk.wire.mss = eph::dpdk::net::kDefaultMss;
        scfg.dpdk.wire.recv_window = 65535;
        scfg.dpdk.pool = pool;
        scfg.connect_timeout = 5s;
        scfg.tls.hostname = app_cfg.host;  // SNI
        scfg.ws.path = app_cfg.ws_path;
        scfg.ws.host = app_cfg.host;
        scfg.ws.timeout = 10s;
        scfg.reasm_capacity = 256 * 1024;

        auto sr = Stream::create(std::move(scfg));
        if (!sr) {
            const auto delay = policy.next_delay();
            if (!delay) break;  // backoff budget exhausted
            spdlog::warn("DpdkTcpStream::create failed: {} — retry in {}ms (attempt {})",
                         sr.error().detail, delay->count(), policy.attempts());
            std::this_thread::sleep_for(*delay);
            continue;
        }
        auto stream = std::move(*sr);
        stream->on_message = on_message_handler;

        if (auto r = poller->add(stream.get()); !r) {
            const auto delay = policy.next_delay();
            if (!delay) break;  // backoff budget exhausted
            spdlog::warn("poller->add failed: {} — retry in {}ms (attempt {})",
                         r.error().detail, delay->count(), policy.attempts());
            std::this_thread::sleep_for(*delay);
            continue;
        }

        // Connected — reset backoff chain.
        policy.reset();
        ++session_count;
        spdlog::info("session #{} connected src={}:{} dst={}:{}",
                     session_count,
                     eph::dpdk::net::format_ipv4(app_cfg.local_ip).data(),
                     src_port,
                     eph::dpdk::net::format_ipv4(dst_ip).data(),
                     app_cfg.port);

        // ── 10c) Burst-poll loop until clean drop, SIGINT, or deadline.
        // `state() == Established` flips to Closing / Closed on any FIN,
        // RST, TLS alert, or poller-side disconnect.
        while (g_running.load(std::memory_order_acquire)
               && std::chrono::steady_clock::now() < deadline
               && stream->state() == eph::net::TcpState::Established) {
            (void)poller->poll();
        }

        (void)poller->remove(stream.get());
        spdlog::info("session #{} ended state={} frames_so_far={}",
                     session_count,
                     eph::net::tcp_state_name(stream->state()),
                     stats.frames);
        // `stream` destroyed at end of scope; outer loop decides what
        // to do next.
    }

    spdlog::info(
        "binance_latency: stopping. sessions={} frames={} parsed_ok={} no_T={} skew_skip={}",
        session_count, stats.frames, stats.parsed_ok, stats.no_T_field,
        stats.clock_skew_skipped);
    stats.recorder.print_report();
    return 0;
}
