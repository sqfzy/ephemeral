/// @file async_dns_multi_resolve.cpp
///
/// **Parallel DNS resolution** via `eph::dpdk::dns::AsyncDnsResolver`,
/// driven from a single `DpdkPoller<>` burst loop.
///
/// Why: in crypto HFT, multi-venue reconnect storms hit DNS as the first
/// step (Binance / OKX / Bybit / Coinbase endpoints are A-record-rotated
/// behind ELB-style frontends — IPs cannot be cached past the TTL). The
/// blocking `eph::dpdk::dns::resolve()` serializes those queries:
/// N venues × ~30ms RTT = N × 30ms wall time. `AsyncDnsResolverT`
/// satisfies `DpdkPollable` so all N queries issue inside the same
/// poll cycle and complete in ~max(individual RTT) wall time.
///
/// What this example shows:
///   1. Construct one `AsyncDnsResolver` per hostname.
///   2. `start()` each (allocates ephemeral src_port + sends query #1).
///   3. `poller->add()` each (registers the 5-tuple for reply routing).
///   4. Drive `poller->poll()` until every resolver reaches a terminal
///      state (`Ready` / `TimedOut` / `Error`).
///   5. Print results and tear down.
///
/// What this example deliberately does NOT show:
///   - DNS-result caching across reconnects: the design assumption is
///     that crypto endpoints rotate IPs, so re-resolving on every
///     reconnect is the correct policy. See `binance_latency.cpp` for
///     the opposite case (single endpoint, resolve-once-at-startup).
///   - ARP resolution of the gateway MAC: the user is expected to call
///     `eph::dpdk::arp::resolve()` once at startup before any DNS query.
///     For the smoke-boot path (no real DPDK env), we pass a synthetic
///     gateway MAC and the resolvers will fail-fast with a clear error.
///   - Reconnect orchestration on top: that is T1.3
///     (`eph::net::ReconnectOrchestrator`) and is a separate concern.
///
/// Smoke-boot vs real-NIC behaviour: this example is structured like
/// `simple_hft_dpdk.cpp` — when `--smoke` is set (or when the mempool /
/// gateway-MAC params are missing), each `start()` returns a clear
/// `ErrorInfo` and the program exits 0 having demonstrated the API
/// surface only. With a real NIC + ARP'd gateway, the same flow drives
/// real queries.
///
/// Usage:
///   sudo ./async_dns_multi_resolve -a 0000:28:00.0 -l 4-7 --
///        --local-ip 10.0.0.16 --gateway-mac aa:bb:cc:dd:ee:ff
///        --nameserver 8.8.8.8
///        [--host stream.binance.com] [--host ws.okx.com]
///        [--host stream.bybit.com]   [--host ws-feed.exchange.coinbase.com]
///        [--smoke]
///
/// `--host` may be passed multiple times. Default set is the four
/// canonical crypto venue WS endpoints. `--smoke` forces the no-NIC
/// path so this example can be run as a doc-test on any host.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <rte_ether.h>
#include <rte_mempool.h>

#include <spdlog/spdlog.h>

#include "eph/dpdk/dns.hpp"
#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/packet_core.hpp"  // net::format_ipv4, parse_ipv4
#include "eph/net/dpdk/poller.hpp"
#include "eph/utils/time.hpp"        // TSC::init() — the resolver's deadline
                                     // logic relies on a calibrated TSC.

namespace edpdk = eph::net::dpdk;
namespace edns  = eph::dpdk::dns;
using namespace std::chrono_literals;

static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running.store(false, std::memory_order_release); }

// Helper — split argv at the first "--" so EAL gets the EAL args.
static int split_eal(int argc, char** argv) noexcept {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--") return i;
    }
    return argc;
}

// Parse "aa:bb:cc:dd:ee:ff" — returns false on any malformed input rather
// than exposing the user to undefined behaviour. Kept inline; reused
// by --gateway-mac parsing only.
static bool parse_mac(std::string_view s, rte_ether_addr& out) noexcept {
    unsigned int b[6];
    if (std::sscanf(s.data(), "%x:%x:%x:%x:%x:%x",
                    &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; ++i) out.addr_bytes[i] = static_cast<uint8_t>(b[i]);
    return true;
}

// Print one terminal-state row in a format that is easy to grep / diff
// across runs. `status` is the user-facing tag; `detail` is either the
// resolved IP (Ready) or the ErrorInfo detail string (TimedOut/Error).
// `duration_tsc == 0` (e.g. for pre-flight Error) is rendered as "-".
static void log_result(const std::string& host,
                       std::string_view status,
                       std::string_view detail,
                       uint64_t duration_tsc) noexcept {
    if (duration_tsc == 0) {
        spdlog::info("dns[{:>32}] {:<9} {:<20} -", host, status, detail);
        return;
    }
    const auto ns = eph::utils::TSC::to_ns(duration_tsc);
    if (ns) {
        spdlog::info("dns[{:>32}] {:<9} {:<20} {:.0f} ns",
                     host, status, detail, *ns);
    } else {
        spdlog::info("dns[{:>32}] {:<9} {:<20} {} cycles (TSC uncalibrated)",
                     host, status, detail, duration_tsc);
    }
}

int main(int argc, char** argv) {
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    spdlog::set_level(spdlog::level::info);

    // ── 1) EAL init ──────────────────────────────────────────────────────
    const int eal_end = split_eal(argc, argv);
    auto eal = eph::dpdk::EalGuard::init(eal_end, argv);
    if (!eal) {
        spdlog::error("async_dns_multi_resolve: EAL init failed: {}", eal.error());
        return 1;
    }
    int    app_argc = argc - (eal_end == argc ? eal_end : eal_end + 1);
    char** app_argv = argv + (eal_end == argc ? eal_end : eal_end + 1);

    // ── 2) Parse app args ────────────────────────────────────────────────
    std::vector<std::string> hosts;
    uint16_t dpdk_port_id = 0;
    uint32_t local_ip     = 0x0A000010;        // 10.0.0.16
    uint32_t nameserver   = 0x08080808;        // 8.8.8.8
    rte_ether_addr gw_mac{};                   // zero — must be set via flag
    bool smoke = false;

    for (int i = 0; i < app_argc; ++i) {
        std::string_view a = app_argv[i];
        if      (a == "--dpdk-port"  && i + 1 < app_argc) dpdk_port_id = static_cast<uint16_t>(std::atoi(app_argv[++i]));
        else if (a == "--host"       && i + 1 < app_argc) hosts.emplace_back(app_argv[++i]);
        else if (a == "--local-ip"   && i + 1 < app_argc) {
            // parse_ipv4 returns 0 on parse failure; only overwrite on success.
            if (auto ip = eph::dpdk::net::parse_ipv4(app_argv[++i]); ip != 0) local_ip = ip;
        }
        else if (a == "--nameserver" && i + 1 < app_argc) {
            if (auto ip = eph::dpdk::net::parse_ipv4(app_argv[++i]); ip != 0) nameserver = ip;
        }
        else if (a == "--gateway-mac"&& i + 1 < app_argc) (void)parse_mac(app_argv[++i], gw_mac);
        else if (a == "--smoke")                          smoke = true;
    }
    if (hosts.empty()) {
        hosts = {"stream.binance.com",
                 "ws.okx.com",
                 "stream.bybit.com",
                 "ws-feed.exchange.coinbase.com"};
    }

    // The resolver's deadline arithmetic uses TSC (per project convention).
    // Calibrate once before any start() call. On failure, the resolver
    // falls back to a 3 GHz estimate (see dns.hpp comment), which is
    // fine for a doc-test but worth a warning.
    if (!eph::utils::TSC::init()) {
        spdlog::warn("async_dns_multi_resolve: TSC::init() failed; "
                     "deadline arithmetic will use a hardcoded estimate");
    }

    // ── 3) Build the Poller ──────────────────────────────────────────────
    edpdk::PollerConfig pcfg{};
    pcfg.port_id     = dpdk_port_id;
    pcfg.rx_queue_id = 0;

    auto poller_r = edpdk::DpdkPoller<>::create(pcfg);
    if (!poller_r) {
        spdlog::error("DpdkPoller::create failed: {}", poller_r.error().detail);
        return 2;
    }
    auto poller = std::move(*poller_r);

    // ── 4) NIC params ────────────────────────────────────────────────────
    // A real run needs a populated mempool + ARP-resolved gateway MAC + the
    // NIC's own MAC. The smoke path leaves these synthetic so start()
    // surfaces the misconfiguration cleanly without sending packets.
    rte_mempool*   pool = nullptr;
    rte_ether_addr src_mac{};   // would come from rte_eth_macaddr_get(port)
    if (smoke || pool == nullptr) {
        spdlog::warn("async_dns_multi_resolve: smoke-boot path — no real "
                     "mempool / ARP-resolved gateway MAC. Demonstrating API "
                     "surface only. Pass --smoke explicitly or wire up a "
                     "real mempool + gateway MAC for live resolution.");
    }

    // ── 5) Construct N resolvers, one per hostname ───────────────────────
    edns::DnsConfig dns_cfg{};
    dns_cfg.nameserver_ip = nameserver;
    dns_cfg.timeout       = 2s;     // generous for a doc-test; production
                                    // typically uses 250-500ms in colo

    // Hostname is supplied to start() per resolver — not the constructor —
    // so the construction loop just sizes the vector.
    std::vector<std::unique_ptr<edns::AsyncDnsResolver>> resolvers;
    resolvers.reserve(hosts.size());
    for (size_t i = 0; i < hosts.size(); ++i) {
        resolvers.emplace_back(std::make_unique<edns::AsyncDnsResolver>(
            dpdk_port_id, /*queue_id=*/0, pool, src_mac, gw_mac,
            local_ip, dns_cfg));
    }

    // ── 6) start() each, then add() to the poller ───────────────────────
    // Order matters: start() allocates the ephemeral src_port that
    // tuple_for_poller_() reports. Adding before start() would register
    // src_port=0 which is unroutable. See dns.hpp tuple_for_poller_ docs.
    size_t live = 0;
    for (size_t i = 0; i < resolvers.size(); ++i) {
        auto sr = resolvers[i]->start(hosts[i]);
        if (!sr) {
            log_result(hosts[i], "Error", sr.error().detail, /*duration_tsc=*/0);
            continue;
        }
        if (auto ar = poller->add(resolvers[i].get()); !ar) {
            log_result(hosts[i], "Error", ar.error().detail, /*duration_tsc=*/0);
            continue;
        }
        ++live;
    }
    if (live == 0) {
        spdlog::info("async_dns_multi_resolve: no resolvers in flight "
                     "(smoke / config error). Exiting cleanly.");
        return 0;
    }

    // ── 7) Drive the burst loop until all terminal or wall deadline ─────
    spdlog::info("async_dns_multi_resolve: {} resolvers in flight, polling…",
                 live);
    const auto wall_deadline =
        std::chrono::steady_clock::now() + dns_cfg.timeout + 500ms;
    while (g_running.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < wall_deadline) {
        (void)poller->poll();
        // Early-exit: every attached resolver in a terminal state.
        bool all_done = true;
        for (const auto& r : resolvers) {
            if (r->is_attached() && !r->is_done()) { all_done = false; break; }
        }
        if (all_done) break;
    }

    // ── 8) Report ───────────────────────────────────────────────────────
    for (size_t i = 0; i < resolvers.size(); ++i) {
        if (!resolvers[i]->is_attached()) continue;     // never started
        const auto duration = resolvers[i]->resolve_duration_tsc();
        const auto res      = resolvers[i]->result();
        if (res) {
            // Format the resolved IPv4 for display.
            char buf[16];
            const auto fmt = eph::dpdk::net::format_ipv4(*res);
            std::snprintf(buf, sizeof(buf), "%.*s",
                          static_cast<int>(fmt.size()), fmt.data());
            log_result(hosts[i], "Ready", buf, duration);
        } else {
            log_result(hosts[i],
                       resolvers[i]->status() == edns::ResolveStatus::TimedOut
                           ? "TimedOut" : "Error",
                       res.error().detail, duration);
        }
        (void)poller->remove(resolvers[i].get());
    }

    spdlog::info("async_dns_multi_resolve: done");
    return 0;
}
