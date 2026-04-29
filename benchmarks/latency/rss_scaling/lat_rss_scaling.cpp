/// @file lat_rss_scaling.cpp
/// Multi-connection one-way RX latency bench.
///
/// Reproduces the observed AWS Nitro NIC behaviour: even with multi-
/// lcore parallel polling, DPDK RX latency on the client side grows
/// (slope ≈ 0.6 µs/connection on c7g) as the number of concurrent
/// connections rises through 5 → 20 → 100. The kernel comparison
/// (single-thread epoll with the same N sockets) shows a flat-to-mild
/// curve; the asymmetry is the deliverable.
///
/// Topology (matches `test_dpdk_rss_fanout` `NStreamsDistributedAcrossQueues`):
///   - DPDK: nb_rx_queues=4, RSS engaged. One DpdkPoller per queue,
///     registered with Platform. N sockets distributed round-robin
///     across queues — `pin_to_queue` rebinds each src_port via
///     `find_src_port_for_queue` so packets actually land on the
///     intended queue. Worker thread per queue, pinned to a dedicated
///     CPU.
///   - Kernel: 1 thread on `cpu_client`, single KernelPoller, N
///     KernelUdpSocket bound to ephemeral src_ports.
///
/// One-way protocol (mockex `rss_scaling_push_run`):
///   - Client sends a single zero-filled subscribe datagram per socket
///     (mock keys subscriptions on (client_ip, src_port)).
///   - After 100 ms grace, mock pushes payload-sized datagrams at
///     `push_rate_pps_per_conn` per peer with `mock_send_ns` written
///     to bytes [16:24].
///   - Each push received → record `now_ns - mock_send_ns` into the
///     per-connection Recorder. No echo / no RTT — pure RX.
///
/// Output: per-connection P50/P99/P99.9 distributions + aggregate
/// merged across connections. Run thrice (BENCH_CONN_COUNT=5/20/100)
/// to chart the scaling slope; `scripts/run_rss_scaling.sh` does the
/// sweep and tabulates.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/codec/raw_datagram_codec.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/utils/cpu.hpp"
#include "eph/utils/recorder.hpp"

#if defined(EPH_USE_DPDK)
#  include "eph/net/dpdk/poller.hpp"
#  include "eph/net/dpdk/udp_socket.hpp"
#  include "core/dpdk_env.hpp"
#else
#  include "eph/net/kernel/config.hpp"
#  include "eph/net/kernel/poller.hpp"
#  include "eph/net/kernel/udp_socket.hpp"
#endif

#include "core/measurement.hpp"
#include "core/pin_client.hpp"
#include "core/timestamp_proto.hpp"

namespace {

namespace ec = eph::codec;
namespace eu = eph::utils;
namespace en = eph::net;

#if defined(EPH_USE_DPDK)
namespace ed = eph::net::dpdk;
using Socket = ed::DpdkUdpSocket<ec::RawDatagramCodec>;
using Poller = ed::DpdkPoller<>;
constexpr const char* kBackend = "dpdk";
#else
namespace ek = eph::net::kernel;
using Socket = ek::KernelUdpSocket<ec::RawDatagramCodec>;
using Poller = ek::KernelPoller;
constexpr const char* kBackend = "kernel";
#endif

constexpr const char* kDefaultConfigPath = "benchmarks/latency/config.toml";
constexpr const char* kSection           = "lat_rss_scaling";

[[nodiscard]] const char* parse_config_path(int argc, char** argv) noexcept {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0) return argv[i + 1];
    }
    if (const char* env = std::getenv("BENCH_CONFIG"); env && *env) return env;
    return kDefaultConfigPath;
}

/// Parse a CSV "0,1,2,3" of cpu indices into a vector. Empty/garbage
/// tokens are skipped silently — the caller is expected to have already
/// validated the count.
[[nodiscard, maybe_unused]] std::vector<int>
parse_csv_ints(std::string_view csv) noexcept {
    std::vector<int> out;
    out.reserve(8);
    std::size_t pos = 0;
    while (pos <= csv.size()) {
        std::size_t comma = csv.find(',', pos);
        std::string_view tok = (comma == std::string_view::npos)
                                   ? csv.substr(pos) : csv.substr(pos, comma - pos);
        while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t'))
            tok.remove_prefix(1);
        while (!tok.empty() && (tok.back()  == ' ' || tok.back()  == '\t'))
            tok.remove_suffix(1);
        if (!tok.empty()) {
            char buf[16] = {};
            const std::size_t n = std::min(tok.size(), sizeof(buf) - 1);
            std::memcpy(buf, tok.data(), n);
            const int v = std::atoi(buf);
            if (v >= 0) out.push_back(v);
        }
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
    return out;
}

/// Result of summarising N per-connection P-percentile values.
struct ConnDistro {
    uint64_t min{};
    uint64_t p50{};
    uint64_t p90{};
    uint64_t max{};
};

[[nodiscard]] ConnDistro summarise(std::vector<uint64_t> v) noexcept {
    ConnDistro d{};
    if (v.empty()) return d;
    std::sort(v.begin(), v.end());
    d.min = v.front();
    d.max = v.back();
    auto pick = [&](double q) {
        const auto idx = std::min<std::size_t>(
            v.size() - 1,
            static_cast<std::size_t>(static_cast<double>(v.size()) * q));
        return v[idx];
    };
    d.p50 = pick(0.50);
    d.p90 = pick(0.90);
    return d;
}

void print_distro(const char* label, const ConnDistro& d) noexcept {
    std::printf("  %-22s min=%-9llu p50=%-9llu p90=%-9llu max=%-9llu\n",
                label,
                static_cast<unsigned long long>(d.min),
                static_cast<unsigned long long>(d.p50),
                static_cast<unsigned long long>(d.p90),
                static_cast<unsigned long long>(d.max));
}

/// Per-connection container — one Recorder per socket. Boxed so the
/// `on_datagram` lambda can capture a stable pointer that survives
/// vector growth.
struct Conn {
    std::unique_ptr<Socket> sock;
    eu::Recorder            rec;
    uint64_t                samples = 0;
    uint16_t                queue   = 0;       ///< DPDK only; 0 on kernel

    explicit Conn(std::string name) : rec(std::move(name)) {}
};

} // namespace

int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::info);

    const char* conf_path = parse_config_path(argc, argv);

    auto cfg_r = bench::load_bench_conf(conf_path);
    if (!cfg_r) {
        std::fprintf(stderr, "lat_rss_scaling: %s\n",
                     bench::format_error(cfg_r.error()).c_str());
        return 1;
    }
    bench::BenchConfig cfg = std::move(*cfg_r);

    const bench::Scenario* sc_ptr = cfg.scenario(kSection);
    if (sc_ptr == nullptr) {
        std::fprintf(stderr,
                     "lat_rss_scaling: [scenarios.%s] not found in %s\n",
                     kSection, conf_path);
        return 1;
    }
    const bench::Scenario& sc = *sc_ptr;

    auto port_r = sc.get<uint16_t>("port");
    if (!port_r) {
        std::fprintf(stderr, "lat_rss_scaling: %s\n",
                     bench::format_error(port_r.error()).c_str());
        return 1;
    }
    const uint16_t port = *port_r;

    const std::size_t payload_size =
        sc.get_or<uint32_t>("payload_size", 256);
    if (payload_size < bench::kTimestampBlockSize) {
        std::fprintf(stderr,
                     "lat_rss_scaling: payload_size=%zu < %zu\n",
                     payload_size, bench::kTimestampBlockSize);
        return 1;
    }

    const uint32_t duration_s =
        sc.get_or<uint32_t>("duration_seconds", 30);
    const uint32_t warmup_s =
        sc.get_or<uint32_t>("warmup_seconds", 2);
    const uint32_t push_rate =
        sc.get_or<uint32_t>("push_rate_pps_per_conn", 5000);

    uint16_t conn_count = sc.get_or<uint16_t>("conn_count", 5);
    if (const char* env = std::getenv("BENCH_CONN_COUNT"); env && *env) {
        const int v = std::atoi(env);
        if (v > 0 && v <= 10000) conn_count = static_cast<uint16_t>(v);
    }
    if (conn_count == 0) {
        std::fprintf(stderr, "lat_rss_scaling: conn_count must be > 0\n");
        return 1;
    }

#if defined(EPH_USE_DPDK)
    const uint16_t nb_rx_queues =
        sc.get_or<uint16_t>("nb_rx_queues_override", 4);
    const uint16_t worker_threads =
        sc.get_or<uint16_t>("worker_threads", nb_rx_queues);
    const std::string eal_override =
        sc.get_or<std::string>("eal_cores_override", std::string{});
    const std::string worker_cpus_csv =
        sc.get_or<std::string>("worker_cpus", std::string{"0,1,2,3"});

    if (worker_threads != nb_rx_queues) {
        std::fprintf(stderr,
                     "lat_rss_scaling: worker_threads (%u) must equal "
                     "nb_rx_queues_override (%u) — one polling thread "
                     "per queue\n",
                     worker_threads, nb_rx_queues);
        return 1;
    }

    auto worker_cpus = parse_csv_ints(worker_cpus_csv);
    if (worker_cpus.size() < worker_threads) {
        std::fprintf(stderr,
                     "lat_rss_scaling: worker_cpus has %zu entries, need %u\n",
                     worker_cpus.size(), worker_threads);
        return 1;
    }
    worker_cpus.resize(worker_threads);

    if (!eal_override.empty()) cfg.cpu.eal_cores = eal_override;
    cfg.dpdk.nb_rx_queues = nb_rx_queues;
#endif

    const std::string& mock_ip_str = cfg.networking.server_ip.empty()
                                         ? std::string{"127.0.0.1"}
                                         : cfg.networking.server_ip;
    auto ip_r = en::Ipv4Addr::parse(mock_ip_str);
    if (!ip_r) {
        std::fprintf(stderr, "lat_rss_scaling: invalid mock_ip '%s': %s\n",
                     mock_ip_str.c_str(), ip_r.error().detail);
        return 1;
    }
    const en::SocketAddr remote{ip_r.value(), port};

    const std::string& client_ip_str = cfg.networking.client_ip.empty()
                                           ? std::string{"0.0.0.0"}
                                           : cfg.networking.client_ip;
    auto client_ip_r = en::Ipv4Addr::parse(client_ip_str);
    if (!client_ip_r) {
        std::fprintf(stderr, "lat_rss_scaling: invalid client_ip '%s': %s\n",
                     client_ip_str.c_str(), client_ip_r.error().detail);
        return 1;
    }

    std::printf("=== lat_rss_scaling (%s) ===\n", kBackend);
    std::printf("config: mock=%s client_bind=%s port=%u payload=%zu "
                "duration=%us warmup=%us push_rate_per_conn=%u pps "
                "conn_count=%u\n",
                mock_ip_str.c_str(), client_ip_str.c_str(),
                static_cast<unsigned>(port),
                payload_size, duration_s, warmup_s, push_rate,
                static_cast<unsigned>(conn_count));
#if defined(EPH_USE_DPDK)
    std::printf("dpdk_topo: nb_rx_queues=%u workers=%u worker_cpus=%s\n",
                nb_rx_queues, worker_threads, worker_cpus_csv.c_str());
#endif
    std::fflush(stdout);

    bench::install_signal_handler();

#if defined(EPH_USE_DPDK)
    // DPDK bring-up — Platform with nb_rx_queues, ARP resolved, EAL up.
    auto env_r = bench::load_dpdk_env(cfg, /*port_id=*/0);
    if (!env_r) {
        std::fprintf(stderr, "lat_rss_scaling: %s\n", env_r.error().c_str());
        return 1;
    }
    auto env = std::move(*env_r);
    bench::print_dpdk_config_echo(env);

    if (env.platform.nb_rx_queues() < nb_rx_queues) {
        std::fprintf(stderr,
                     "lat_rss_scaling: Platform reports %u RX queues, "
                     "requested %u — NIC firmware likely capped\n",
                     env.platform.nb_rx_queues(), nb_rx_queues);
        return 2;
    }

    // Per-queue Pollers, registered with the Platform's queue→poller
    // map. Each push lands on the queue the RSS hash dictates and is
    // dispatched to that queue's Poller.
    std::vector<std::unique_ptr<Poller>> pollers;
    pollers.reserve(nb_rx_queues);
    for (uint16_t q = 0; q < nb_rx_queues; ++q) {
        ed::PollerConfig pcfg{};
        pcfg.port_id     = env.port_id;
        pcfg.rx_queue_id = q;
        // Opt in to the Poller's hard cap (kMaxConnHard=64) so a 100-conn
        // sweep with hash imbalance fits in any single queue's table.
        // Default is kMaxConn=16 — too small once conn_count > 4*16=64.
        pcfg.max_connections = ed::DpdkPoller<void>::kMaxConnHard;
        auto p_r = Poller::create(pcfg);
        if (!p_r) {
            std::fprintf(stderr,
                         "lat_rss_scaling: Poller::create(q=%u): %s\n",
                         q, p_r.error().detail);
            return 2;
        }
        pollers.push_back(std::move(*p_r));
        if (auto rr = env.platform.register_poller(q, pollers[q].get()); !rr) {
            std::fprintf(stderr,
                         "lat_rss_scaling: register_poller(q=%u): %s\n",
                         q, rr.error().detail);
            return 2;
        }
    }
#else
    auto poller_r = Poller::create({});
    if (!poller_r) {
        std::fprintf(stderr, "lat_rss_scaling: Poller::create: %s\n",
                     poller_r.error().detail);
        return 2;
    }
    auto poller = std::move(*poller_r);
#endif

    // Per-connection state — boxed so the on_datagram lambda's captured
    // raw pointer is stable through the rest of setup.
    std::vector<std::unique_ptr<Conn>> conns;
    conns.reserve(conn_count);

    // The "no samples before this ns" gate. Updated to the actual
    // measurement-window start right after the subscribe pass and the
    // mock's 100 ms grace lapses. Atomic because the workers read it
    // (relaxed) on every callback.
    std::atomic<uint64_t> measurement_start_ns{UINT64_MAX};

    auto make_callback = [&measurement_start_ns](Conn* cp) {
        return [cp, &measurement_start_ns](
                   std::span<const uint8_t> data,
                   const en::SocketAddr& /*src*/) noexcept {
            if (data.size() < bench::kTimestampBlockSize) return;
            const uint64_t t1 = bench::monotonic_raw_ns();
            const uint64_t start = measurement_start_ns.load(
                std::memory_order_relaxed);
            if (t1 < start) return;  // warmup-discard

            uint64_t mock_send = 0;
            std::memcpy(&mock_send, data.data() + 16, sizeof(mock_send));
            if (mock_send == 0 || mock_send > t1) return;  // sanity

            cp->rec.record_ns(t1 - mock_send);
            ++cp->samples;
        };
    };

    for (uint16_t i = 0; i < conn_count; ++i) {
        auto c = std::make_unique<Conn>(
            std::string{"rss_scaling_"} + kBackend + "_conn_" +
            std::to_string(i));

#if defined(EPH_USE_DPDK)
        const uint16_t target_q = i % nb_rx_queues;
        ed::UdpConfig scfg{};
        scfg.legacy.src_ip       = env.src_ip;
        scfg.legacy.dst_ip       = env.dst_ip;
        scfg.legacy.src_port     = 0;  // overridden by find_src_port_for_queue
        scfg.legacy.dst_port     = port;
        scfg.legacy.src_mac      = env.src_mac;
        scfg.legacy.dst_mac      = env.gw_mac;
        scfg.legacy.port_id      = env.port_id;
        scfg.legacy.tx_queue_id  = target_q;
        scfg.legacy.pool         = env.pool;
        scfg.pin_to_queue        = target_q;

        auto sock_r = Socket::create_and_attach(std::move(scfg), env.platform);
        if (!sock_r) {
            std::fprintf(stderr,
                         "lat_rss_scaling: create_and_attach(conn=%u, q=%u): %s\n",
                         i, target_q, sock_r.error().detail);
            return 3;
        }
        c->sock  = std::move(*sock_r);
        c->queue = target_q;
#else
        ek::UdpConfig scfg{};
        scfg.bind = en::SocketAddr{client_ip_r.value(), 0};  // ephemeral
        auto sock_r = Socket::create(scfg);
        if (!sock_r) {
            std::fprintf(stderr,
                         "lat_rss_scaling: KernelUdpSocket::create(conn=%u): %s\n",
                         i, sock_r.error().detail);
            return 3;
        }
        c->sock = std::move(*sock_r);
        if (auto rr = poller->add(c->sock.get()); !rr) {
            std::fprintf(stderr,
                         "lat_rss_scaling: poller->add(conn=%u): %s\n",
                         i, rr.error().detail);
            return 3;
        }
#endif

        c->sock->on_datagram = make_callback(c.get());
        conns.push_back(std::move(c));
    }

    // Subscribe pass — single zero-filled datagram per socket.
    {
        std::array<uint8_t, bench::kTimestampBlockSize> zeros{};
        for (uint16_t i = 0; i < conn_count; ++i) {
            auto sr = conns[i]->sock->send_to(
                std::span<const uint8_t>{zeros.data(), zeros.size()},
                remote);
            if (!sr) {
                std::fprintf(stderr,
                             "lat_rss_scaling: subscribe(conn=%u): %s\n",
                             i, sr.error().detail);
                return 4;
            }
        }
    }
    SPDLOG_INFO("lat_rss_scaling: {} subscribes sent", conn_count);

    // Drive the mock's 100 ms grace + scenario warmup before flipping
    // the recording gate. After the gate flips, callbacks start
    // counting samples toward the per-conn Recorder.
    const uint64_t now_ns       = bench::monotonic_raw_ns();
    const uint64_t mock_grace   = 150ull * 1'000'000ull;          // a hair past mock's 100 ms
    const uint64_t warmup_ns    = static_cast<uint64_t>(warmup_s) * 1'000'000'000ull;
    const uint64_t measure_at   = now_ns + mock_grace + warmup_ns;
    const uint64_t deadline_ns  = measure_at +
        static_cast<uint64_t>(duration_s) * 1'000'000'000ull;
    measurement_start_ns.store(measure_at, std::memory_order_relaxed);

#if defined(EPH_USE_DPDK)
    // Worker threads — one per queue, each spinning its own Poller
    // until the deadline. Pinned to the configured worker_cpus so the
    // OS scheduler can't migrate them onto each other.
    std::atomic<bool> stop{false};
    std::vector<std::jthread> workers;
    workers.reserve(nb_rx_queues);
    for (uint16_t q = 0; q < nb_rx_queues; ++q) {
        const int cpu = worker_cpus[q];
        Poller* p = pollers[q].get();
        workers.emplace_back([cpu, p, &stop]() {
            eph::utils::CpuPinPolicy policy{
                .require_isolcpus            = false,
                .require_no_sibling_conflict = false,
                .require_same_numa           = false,
                .warn_irq_overlap            = false,
            };
            char tname[16];
            std::snprintf(tname, sizeof(tname), "rss-w-%d", cpu);
            auto pin = eph::utils::pin_thread(cpu, tname, policy);
            if (pin) pin->release();
            else SPDLOG_WARN("worker pin to cpu {} failed: {}",
                             cpu, pin.error());

            while (!stop.load(std::memory_order_relaxed)) {
                (void)p->poll();
            }
        });
    }

    // Main thread idles until deadline (or SIGINT).
    while (bench::monotonic_raw_ns() < deadline_ns &&
           !bench::shutdown_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    stop.store(true, std::memory_order_relaxed);
    workers.clear();   // jthread join on destruction
#else
    bench::pin_client_from_cfg(cfg, "lat_rss_scaling");

    while (bench::monotonic_raw_ns() < deadline_ns &&
           !bench::shutdown_requested()) {
        (void)poller->poll();
    }
#endif

    // ── Reporting ────────────────────────────────────────────────────
    std::printf("\n=== lat_rss_scaling (%s) conn=%u ===\n",
                kBackend, static_cast<unsigned>(conn_count));

    // Per-connection percentiles → distributions across N conns.
    std::vector<uint64_t> p50s, p99s, p999s, sample_counts;
    p50s.reserve(conn_count);
    p99s.reserve(conn_count);
    p999s.reserve(conn_count);
    sample_counts.reserve(conn_count);

    eu::Recorder agg{std::string{"rss_scaling_"} + kBackend + "_aggregate"};

    for (auto& c : conns) {
        if (auto s_opt = c->rec.compute_stats()) {
            p50s.push_back(s_opt->p50_ns);
            p99s.push_back(s_opt->p99_ns);
            p999s.push_back(s_opt->p999_ns);
            sample_counts.push_back(s_opt->count);
        } else {
            sample_counts.push_back(0);
        }
        (void)agg.merge(c->rec);
    }

    uint64_t total_samples = 0;
    uint64_t min_per_conn  = UINT64_MAX;
    uint64_t max_per_conn  = 0;
    for (auto n : sample_counts) {
        total_samples += n;
        if (n < min_per_conn) min_per_conn = n;
        if (n > max_per_conn) max_per_conn = n;
    }
    if (sample_counts.empty()) min_per_conn = 0;

    std::printf("samples_total=%llu  per_conn_min=%llu  per_conn_max=%llu  "
                "(window=%us)\n",
                static_cast<unsigned long long>(total_samples),
                static_cast<unsigned long long>(min_per_conn),
                static_cast<unsigned long long>(max_per_conn),
                duration_s);

    std::printf("\nper-connection percentile distributions (one-way RX_ns):\n");
    print_distro("P50 across conns",   summarise(p50s));
    print_distro("P99 across conns",   summarise(p99s));
    print_distro("P99.9 across conns", summarise(p999s));

    std::printf("\naggregate (all samples merged):\n");
    if (auto agg_s = agg.compute_stats()) {
        bench::print_stats_block("RX_one_way_ns", *agg_s);
    } else {
        bench::print_stats_block_empty("RX_one_way_ns");
    }
    std::fflush(stdout);

    // Cleanup — ensure pollers/sockets tear down cleanly. Order matters
    // on DPDK: unregister sockets via Platform first, then unregister
    // queue pollers, then drop pollers and env in reverse setup order.
#if defined(EPH_USE_DPDK)
    for (auto& c : conns) c->sock.reset();
    for (uint16_t q = 0; q < nb_rx_queues; ++q) {
        env.platform.unregister_poller(q);
    }
    pollers.clear();
#else
    for (auto& c : conns) {
        if (c->sock) (void)poller->remove(c->sock.get());
        c->sock.reset();
    }
    poller.reset();
#endif

    return bench::shutdown_requested() ? 130 : 0;
}
