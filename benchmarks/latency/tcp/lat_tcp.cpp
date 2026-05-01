/// @file lat_tcp.cpp
/// Latency benchmark: raw TCP RTT against the `tcp` scenario served by
/// the unified C++ mock `benchmarks/mockex/mockex`.
///
///   * Single-file scenario binary that reads `[lat_tcp]` from bench.conf
///     (port / payload_size / duration_seconds) plus the lowercase global
///     `mock_ip`, `warmup_samples`.
///   * Uses `KernelTcpStream<RawStreamCodec, ...>` + `KernelPoller` (kernel)
///     or `DpdkTcpStream<RawStreamCodec, ...>` + `DpdkPoller` (DPDK).
///   * Measurement clock is `bench::monotonic_raw_ns()` (CLOCK_MONOTONIC_RAW
///     via vDSO) — not TSC.
///   * Samples feed `eph::utils::Recorder::record_ns(ns)`.
///
/// **Reshape note (parallel-bench v2)**: the inner measurement loop has
/// been extracted to `benchmarks/latency/scenarios/lat_tcp_loop.hpp`'s
/// `run_lat_tcp_loop<EnableTls>(BenchCtx&)`. main() here becomes a thin
/// wrapper that does Platform / Poller bring-up, builds a BenchCtx with
///
/// The binary does NOT manage mocks or NICs — the `lat` wrapper script forks
/// `mockex --scenario tcp` and drives the NIC state transition before
/// exec'ing this binary.
///
/// A second target `lat_tcp_dpdk` is produced by the xmake auto-glob loop
/// with `EPH_USE_DPDK=1`.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <spdlog/spdlog.h>

#include "core/bench_conf.hpp"
#include "core/bench_ctx.hpp"
#include "core/measurement.hpp"
#include "core/pin_client.hpp"
#if defined(EPH_USE_DPDK)
#  include "core/dpdk_env.hpp"
#  include "eph/net/dpdk/poller.hpp"
#else
#  include "eph/net/kernel/poller.hpp"
#endif

#include "scenarios/lat_tcp_loop.hpp"

namespace {

constexpr const char* kDefaultConfigPath = "benchmarks/latency/config.toml";

[[nodiscard, maybe_unused]] const char* parse_config_path(int argc, char** argv) noexcept {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0) {
            return argv[i + 1];
        }
    }
    if (const char* env = std::getenv("BENCH_CONFIG"); env && *env) {
        return env;
    }
    return kDefaultConfigPath;
}

} // namespace

int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::info);

    const char* conf_path = parse_config_path(argc, argv);

    auto cfg_r = bench::load_bench_conf(conf_path);
    if (!cfg_r) {
        std::fprintf(stderr, "lat_tcp: %s\n",
                     bench::format_error(cfg_r.error()).c_str());
        return 1;
    }
    const bench::BenchConfig& bench_cfg = *cfg_r;
    const bench::Scenario* tcp = bench_cfg.scenario("lat_tcp");
    if (tcp == nullptr) {
        std::fprintf(stderr, "lat_tcp: [scenarios.lat_tcp] not found in %s\n",
                     conf_path);
        return 1;
    }

    bench::pin_client_from_cfg(bench_cfg, "lat_tcp");

    const bool use_tls = tcp->get_or<bool>("use_tls", false);
    const std::size_t payload_size = tcp->get_or<uint32_t>("payload_size", 256);
    const uint16_t port = tcp->get_or<uint16_t>("port", 0);
    if (port == 0) {
        std::fprintf(stderr, "lat_tcp: scenarios.lat_tcp.port required\n");
        return 1;
    }

    const std::string mock_ip_str = bench_cfg.networking.server_ip.empty()
                                        ? std::string{"127.0.0.1"}
                                        : bench_cfg.networking.server_ip;
    const uint64_t duration_s = tcp->get_or<uint32_t>("duration_seconds", 10);
    const uint64_t warmup_samples = bench_cfg.measurement.warmup_samples;

    std::printf("=== lat_tcp ===\n");
    std::printf("config: mock=%s port=%u payload_size=%zu duration=%llus "
                "warmup_samples=%llu tls=%s\n",
                mock_ip_str.c_str(),
                static_cast<unsigned>(port),
                payload_size,
                static_cast<unsigned long long>(duration_s),
                static_cast<unsigned long long>(warmup_samples),
                use_tls ? "yes" : "no");
    std::fflush(stdout);

    bench::install_signal_handler();

#if defined(EPH_USE_DPDK)
    auto env_r = bench::load_dpdk_env(bench_cfg, /*port_id=*/0);
    if (!env_r) {
        std::fprintf(stderr, "lat_tcp: %s\n", env_r.error().c_str());
        return 1;
    }
    auto env = std::move(*env_r);
    bench::print_dpdk_config_echo(env);
    bench::DpdkBenchView view = bench::make_view(env);

    // Use the actual queue owned by this process (primary=0, secondary=1, etc.)
    const uint16_t rx_queue = env.platform.effective_rx_queue_range().first;

    eph::net::dpdk::PollerConfig poller_cfg{};
    poller_cfg.port_id     = env.port_id;
    poller_cfg.rx_queue_id = rx_queue;
    auto poller_r = eph::net::dpdk::DpdkPoller<>::create(poller_cfg);
    if (!poller_r) {
        std::fprintf(stderr, "lat_tcp: Poller::create failed: %s\n",
                     poller_r.error().detail);
        return 2;
    }
    auto poller = std::move(poller_r.value());

    if (auto rr = env.platform.register_poller(rx_queue, poller.get()); !rr) {
        std::fprintf(stderr, "lat_tcp: register_poller failed: %s\n",
                     rr.error().detail);
        return 3;
    }
#else
    auto poller_r = eph::net::kernel::KernelPoller::create({});
    if (!poller_r) {
        std::fprintf(stderr, "lat_tcp: Poller::create failed: %s\n",
                     poller_r.error().detail);
        return 2;
    }
    auto poller = std::move(poller_r.value());
    const uint16_t rx_queue = 0;  // kernel: no MP queue concept
#endif

    bench::BenchCtx ctx{};
    ctx.bench_cfg    = &bench_cfg;
    ctx.scenario_cfg = tcp;
#if defined(EPH_USE_DPDK)
    ctx.view         = &view;
#endif
    ctx.poller       = poller.get();
    ctx.queue_id     = rx_queue;

    return use_tls
        ? bench::scenarios::run_lat_tcp_loop<true>(ctx)
        : bench::scenarios::run_lat_tcp_loop<false>(ctx);
}
