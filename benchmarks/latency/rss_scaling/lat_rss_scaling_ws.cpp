/// @file lat_rss_scaling_ws.cpp
/// WS-over-TLS variant of `lat_rss_scaling`.
///
/// One-process internal sweep: reads `conn_count_list` from
/// `[scenarios.lat_rss_scaling_ws]` and runs each conn count as a
/// "cell" sequentially against a single mockex instance. Per-cell:
///   1. Open N TLS+WS streams (sequential connect+handshake).
///   2. After mock-grace + warmup_seconds, record per-connection
///      one-way RX latency (`t_recv − mock_send_ns` from frame
///      [16:24]) for `duration_seconds`.
///   3. close_gracefully + drain pollers + drop all streams.
///   4. Print per-cell report; collect (n, agg p50, agg p99.9) for
///      the final summary table.
///
/// Topology matches `test_dpdk_rss_fanout` `NStreamsDistributedAcrossQueues`:
///   - DPDK: nb_rx_queues=4 RSS, 4 EAL **worker lcores** each polling one
///     DpdkPoller via `rte_eal_remote_launch` (canonical pattern, mirrors
///     `examples/dpdk_rss_demo.cpp`). Setaffinity is done by EAL
///     itself from `--lcores=N@cpu` (driven by `cpu.eal_cores` →
///     `LcorePin` → `init_with_pins`). Stream i pinned to queue
///     (i % nb_q). The previous `std::jthread` + `pin_thread` path is
///     gone — polling DPDK from a non-lcore thread fights EAL for
///     setaffinity and bypasses RTE_PER_LCORE state.
///   - Kernel: single thread, single KernelPoller, N streams.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/codec/ws_codec.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/utils/cpu.hpp"
#include "eph/utils/recorder.hpp"

#if defined(EPH_USE_DPDK)
#  include <rte_launch.h>
#  include <rte_lcore.h>

#  include "eph/net/dpdk/poller.hpp"
#  include "eph/net/dpdk/tcp_stream.hpp"
#  include "core/dpdk_env.hpp"
#else
#  include "eph/net/kernel/config.hpp"
#  include "eph/net/kernel/poller.hpp"
#  include "eph/net/kernel/tcp_stream.hpp"
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
using Stream = ed::DpdkTcpStream<ec::WsCodec, /*EnableTls=*/true>;
using Poller = ed::DpdkPoller<>;
constexpr const char* kBackend = "dpdk_tls";
#else
namespace ek = eph::net::kernel;
using Stream = ek::KernelTcpStream<ec::WsCodec, /*EnableTls=*/true>;
using Poller = ek::KernelPoller;
constexpr const char* kBackend = "kernel_tls";
#endif

constexpr const char* kDefaultConfigPath = "benchmarks/latency/config.toml";
constexpr const char* kSection           = "lat_rss_scaling_ws";

[[nodiscard]] const char* parse_config_path(int argc, char** argv) noexcept {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0) return argv[i + 1];
    }
    if (const char* env = std::getenv("BENCH_CONFIG"); env && *env) return env;
    return kDefaultConfigPath;
}

struct ConnDistro {
    uint64_t min{}, p50{}, p90{}, max{};
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

struct Conn {
    std::unique_ptr<Stream> stream;
    eu::Recorder            rec;
    uint64_t                samples = 0;
    uint16_t                queue   = 0;
    explicit Conn(std::string name) : rec(std::move(name)) {}
};

/// Result tuple for the final summary table — populated per cell.
struct CellResult {
    uint16_t                  conn_count{};
    uint64_t                  total_samples{};
    std::optional<eu::Stats>  agg;
    ConnDistro                p50_distro;
    ConnDistro                p99_distro;
    ConnDistro                p999_distro;
};

#if defined(EPH_USE_DPDK)
/// Per-worker context handed to `rte_eal_remote_launch`. Lifetime is
/// bounded by `run_cell`: every launch is matched by a
/// `rte_eal_wait_lcore` before the cell returns.
struct WorkerCtx {
    Poller*            poller;
    std::atomic<bool>* stop;
};

/// EAL worker entry point. EAL has already pinned this thread to its
/// declared cpu inside `rte_eal_init` (driven by `cpu.eal_cores` →
/// `LcorePin` → `init_with_pins` in `load_dpdk_env`). Returning 0
/// hands the lcore back to `eal_thread_loop` so the next cell can
/// re-launch it.
int worker_main(void* arg) noexcept {
    auto* ctx = static_cast<WorkerCtx*>(arg);
    while (!ctx->stop->load(std::memory_order_relaxed)) {
        (void)ctx->poller->poll();
    }
    return 0;
}
#endif

} // namespace

int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::info);

    const char* conf_path = parse_config_path(argc, argv);

    auto cfg_r = bench::load_bench_conf(conf_path);
    if (!cfg_r) {
        std::fprintf(stderr, "lat_rss_scaling_ws: %s\n",
                     bench::format_error(cfg_r.error()).c_str());
        return 1;
    }
    bench::BenchConfig cfg = std::move(*cfg_r);

    const bench::Scenario* sc_ptr = cfg.scenario(kSection);
    if (sc_ptr == nullptr) {
        std::fprintf(stderr,
                     "lat_rss_scaling_ws: [scenarios.%s] not found in %s\n",
                     kSection, conf_path);
        return 1;
    }
    const bench::Scenario& sc = *sc_ptr;

    auto port_r = sc.get<uint16_t>("port");
    if (!port_r) {
        std::fprintf(stderr, "lat_rss_scaling_ws: %s\n",
                     bench::format_error(port_r.error()).c_str());
        return 1;
    }
    const uint16_t port = *port_r;

    const std::size_t payload_size =
        sc.get_or<uint32_t>("payload_size", 256);
    if (payload_size < bench::kTimestampBlockSize) {
        std::fprintf(stderr,
                     "lat_rss_scaling_ws: payload_size=%zu < %zu\n",
                     payload_size, bench::kTimestampBlockSize);
        return 1;
    }

    const std::string ws_path =
        sc.get_or<std::string>("ws_path", std::string{"/rss"});
    const uint32_t duration_s =
        sc.get_or<uint32_t>("duration_seconds", 30);
    const uint32_t warmup_s =
        sc.get_or<uint32_t>("warmup_seconds", 2);

    // conn_count_list (TOML int array) drives the sweep. Singleton
    // `conn_count` falls back if the list is absent — mostly for
    // hand-running with a single fixed N.
    std::vector<uint16_t> conn_counts =
        sc.get_or<std::vector<uint16_t>>("conn_count_list",
                                          std::vector<uint16_t>{});
    if (conn_counts.empty()) {
        const uint16_t single = sc.get_or<uint16_t>("conn_count", 5);
        conn_counts.push_back(single);
    }
    for (auto n : conn_counts) {
        if (n == 0) {
            std::fprintf(stderr,
                         "lat_rss_scaling_ws: conn_count_list contains 0\n");
            return 1;
        }
    }
    const uint16_t max_conn_count =
        *std::max_element(conn_counts.begin(), conn_counts.end());

#if defined(EPH_USE_DPDK)
    const uint16_t nb_rx_queues =
        sc.get_or<uint16_t>("nb_rx_queues_override", 4);
    const std::string eal_override =
        sc.get_or<std::string>("eal_cores_override", std::string{});

    // `cpu.eal_cores` (or its `eal_cores_override`) must list main +
    // every worker cpu. `load_dpdk_env` parses the CSV into LcorePin
    // entries that EAL itself pins; `rte_eal_remote_launch` then
    // dispatches the per-queue poll loops onto the worker lcores. The
    // count check happens after EAL init via `rte_lcore_count()`.
    if (!eal_override.empty()) cfg.cpu.eal_cores = eal_override;
    cfg.dpdk.nb_rx_queues = nb_rx_queues;
#endif

    const std::string& mock_ip_str = cfg.networking.server_ip.empty()
                                         ? std::string{"127.0.0.1"}
                                         : cfg.networking.server_ip;
    auto ip_r = en::Ipv4Addr::parse(mock_ip_str);
    if (!ip_r) {
        std::fprintf(stderr, "lat_rss_scaling_ws: invalid mock_ip '%s': %s\n",
                     mock_ip_str.c_str(), ip_r.error().detail);
        return 1;
    }
    const en::SocketAddr remote{ip_r.value(), port};

    std::printf("=== lat_rss_scaling_ws (%s) ===\n", kBackend);
    std::printf("config: mock=%s port=%u ws_path=%s payload=%zu "
                "duration=%us warmup=%us conn_count_list=[",
                mock_ip_str.c_str(), static_cast<unsigned>(port),
                ws_path.c_str(), payload_size, duration_s, warmup_s);
    for (size_t i = 0; i < conn_counts.size(); ++i) {
        std::printf("%s%u", i == 0 ? "" : ",",
                    static_cast<unsigned>(conn_counts[i]));
    }
    std::printf("]\n");
#if defined(EPH_USE_DPDK)
    std::printf("dpdk_topo: nb_rx_queues=%u eal_cores=%s "
                "(main lcore + %u worker lcore(s) via rte_eal_remote_launch)\n",
                nb_rx_queues, cfg.cpu.eal_cores.c_str(), nb_rx_queues);
#endif
    std::fflush(stdout);

    bench::install_signal_handler();

    // ── One-time setup: DPDK env + workers OR kernel poller ──────────
#if defined(EPH_USE_DPDK)
    auto env_r = bench::load_dpdk_env(cfg, /*port_id=*/0);
    if (!env_r) {
        std::fprintf(stderr, "lat_rss_scaling_ws: %s\n", env_r.error().c_str());
        return 1;
    }
    auto env = std::move(*env_r);
    bench::print_dpdk_config_echo(env);

    if (env.platform.nb_rx_queues() < nb_rx_queues) {
        std::fprintf(stderr,
                     "lat_rss_scaling_ws: Platform reports %u RX queues, "
                     "requested %u\n",
                     env.platform.nb_rx_queues(), nb_rx_queues);
        return 2;
    }

    // EAL must have at least nb_rx_queues worker lcores (i.e. lcore
    // count - 1 main). `cpu.eal_cores` carries the cpu list — extend it
    // in config.toml if this trips.
    const unsigned avail_workers =
        rte_lcore_count() > 0 ? rte_lcore_count() - 1u : 0u;
    if (avail_workers < nb_rx_queues) {
        std::fprintf(stderr,
                     "lat_rss_scaling_ws: EAL has %u worker lcore(s), need %u "
                     "(set cpu.eal_cores or eal_cores_override to "
                     "main+nb_rx_queues cpus, e.g. \"0,8,9,10,11\" for "
                     "nb_rx_queues=4)\n",
                     avail_workers, nb_rx_queues);
        return 2;
    }

    std::vector<std::unique_ptr<Poller>> pollers;
    pollers.reserve(nb_rx_queues);
    for (uint16_t q = 0; q < nb_rx_queues; ++q) {
        ed::PollerConfig pcfg{};
        pcfg.port_id         = env.port_id;
        pcfg.rx_queue_id     = q;
        pcfg.max_connections = ed::DpdkPoller<void>::kMaxConnHard;
        auto p_r = Poller::create(pcfg);
        if (!p_r) {
            std::fprintf(stderr,
                         "lat_rss_scaling_ws: Poller::create(q=%u): %s\n",
                         q, p_r.error().detail);
            return 2;
        }
        pollers.push_back(std::move(*p_r));
        if (auto rr = env.platform.register_poller(q, pollers[q].get()); !rr) {
            std::fprintf(stderr,
                         "lat_rss_scaling_ws: register_poller(q=%u): %s\n",
                         q, rr.error().detail);
            return 2;
        }
    }
    // Workers are spawned PER CELL — see run_cell. They cannot persist
    // across cells because `Stream::create_and_attach` polls the per-
    // queue Poller from the main thread during the synchronous TCP /
    // TLS / WS handshake, and `DpdkPoller::poll()` is not safe under
    // concurrent callers. Per-cell spawn is cheap (~5 ms × 4 threads).
#else
    auto poller_r = Poller::create({});
    if (!poller_r) {
        std::fprintf(stderr, "lat_rss_scaling_ws: Poller::create: %s\n",
                     poller_r.error().detail);
        return 2;
    }
    auto poller = std::move(*poller_r);
    bench::pin_client_from_cfg(cfg, "lat_rss_scaling_ws");
#endif
    (void)max_conn_count;

    // ── Per-cell measurement ─────────────────────────────────────────

    // Gate atomic — reset per cell so each cell discards its own
    // mock-grace + warmup. Workers read it relaxed in the on_message
    // callback.
    std::atomic<uint64_t> measurement_start_ns{UINT64_MAX};

    auto make_callback = [&measurement_start_ns](Conn* cp) {
        return [cp, &measurement_start_ns](
                   std::span<const uint8_t> app_frame) noexcept {
            if (app_frame.size() < bench::kTimestampBlockSize) return;
            const uint64_t t1 = bench::monotonic_raw_ns();
            const uint64_t start =
                measurement_start_ns.load(std::memory_order_relaxed);
            if (t1 < start) return;

            uint64_t mock_send = 0;
            std::memcpy(&mock_send, app_frame.data() + 16, sizeof(mock_send));
            if (mock_send == 0 || mock_send > t1) return;

            cp->rec.record_ns(t1 - mock_send);
            ++cp->samples;
        };
    };

    auto run_cell = [&](uint16_t n) -> CellResult {
        SPDLOG_INFO("==== cell start: conn_count={} ====", n);

        std::vector<std::unique_ptr<Conn>> conns;
        conns.reserve(n);

        // Disarm gate while we set up — any pre-warmup arrivals get
        // discarded by the t1<start check.
        measurement_start_ns.store(UINT64_MAX, std::memory_order_relaxed);

        for (uint16_t i = 0; i < n; ++i) {
            auto c = std::make_unique<Conn>(
                std::string{"rss_scaling_ws_"} + kBackend +
                "_n" + std::to_string(n) + "_conn_" + std::to_string(i));

#if defined(EPH_USE_DPDK)
            const uint16_t target_q = i % nb_rx_queues;
            ed::StreamConfig scfg{};
            scfg.dpdk.tcp_low_level = env.make_tcp_config(
                bench::random_src_port(), port);
            scfg.dpdk.pool         = env.pool;
            scfg.dpdk.pin_to_queue = target_q;
            scfg.connect_timeout   = std::chrono::milliseconds{5000};
            scfg.ws.path           = ws_path;
            scfg.ws.timeout        = std::chrono::seconds{10};
            scfg.tls.hostname      = mock_ip_str;
            scfg.tls.verify_peer   = false;

            auto stream_r = Stream::create_and_attach(std::move(scfg),
                                                       env.platform);
            if (!stream_r) {
                SPDLOG_ERROR("create_and_attach(conn={}, q={}): {}",
                             i, target_q, stream_r.error().detail);
                return CellResult{n, 0, std::nullopt, {}, {}, {}};
            }
            c->stream = std::move(*stream_r);
            c->queue  = target_q;
#else
            ek::StreamConfig scfg{};
            scfg.remote          = remote;
            scfg.reasm_capacity  = std::max<std::size_t>(64 * 1024,
                                                          payload_size * 4);
            scfg.connect_timeout = std::chrono::milliseconds{5000};
            scfg.ws.path         = ws_path;
            scfg.ws.timeout      = std::chrono::seconds{10};
            scfg.tls.hostname    = mock_ip_str;
            scfg.tls.verify_peer = false;

            auto stream_r = Stream::create(scfg);
            if (!stream_r) {
                SPDLOG_ERROR("KernelTcpStream::create(conn={}): {}",
                             i, stream_r.error().detail);
                return CellResult{n, 0, std::nullopt, {}, {}, {}};
            }
            c->stream = std::move(*stream_r);
            if (auto rr = poller->add(c->stream.get()); !rr) {
                SPDLOG_ERROR("poller->add(conn={}): {}", i, rr.error().detail);
                return CellResult{n, 0, std::nullopt, {}, {}, {}};
            }
#endif
            c->stream->on_message = make_callback(c.get());
            conns.push_back(std::move(c));
        }
        SPDLOG_INFO("cell n={}: {} streams established", n, conns.size());

        // Open the gate and run.
        // Subscribe protocol: mock waits for one WS frame from each
        // client before pushing to that subscriber. We send the
        // subscribe AFTER workers are spawned so the RX path (PMD →
        // mbuf → poller → on_message) is ready to drain pushes from
        // the moment they start. Without this gate, mock pushes
        // during the setup loop accumulate as unread mbufs and
        // exhaust the pool around conn≈8 of cell 2, dropping the
        // SYN-ACK for subsequent connects.
        const uint64_t now_ns      = bench::monotonic_raw_ns();
        const uint64_t mock_grace  = 150ull * 1'000'000ull;
        const uint64_t warmup_ns   = static_cast<uint64_t>(warmup_s) *
                                     1'000'000'000ull;
        const uint64_t measure_at  = now_ns + mock_grace + warmup_ns;
        const uint64_t deadline_ns = measure_at +
            static_cast<uint64_t>(duration_s) * 1'000'000'000ull;
        measurement_start_ns.store(measure_at, std::memory_order_relaxed);

#if defined(EPH_USE_DPDK)
        // Dispatch poll loops onto EAL worker lcores AFTER all
        // create_and_attach handshakes are done. Main thread did the
        // handshake polls; workers now take over the per-queue Pollers.
        // EAL has already pinned every worker thread to its declared
        // cpu inside `rte_eal_init` — no `pin_thread`, no user-spawned
        // `std::jthread` (that path bypasses EAL setaffinity /
        // RTE_PER_LCORE state and collides with the lcore EAL has
        // parked on the same cpu). Safe because no other thread is
        // calling poll() at this point.
        std::atomic<bool> stop_workers{false};
        std::vector<WorkerCtx> ctxs;
        ctxs.reserve(nb_rx_queues);
        for (uint16_t q = 0; q < nb_rx_queues; ++q) {
            ctxs.push_back(WorkerCtx{pollers[q].get(), &stop_workers});
        }

        bool launch_ok = true;
        uint16_t worker_idx = 0;
        unsigned lcore_id;
        RTE_LCORE_FOREACH_WORKER(lcore_id) {
            if (worker_idx >= nb_rx_queues) break;
            if (int rc = rte_eal_remote_launch(worker_main,
                                               &ctxs[worker_idx],
                                               lcore_id);
                rc != 0) {
                SPDLOG_ERROR("rte_eal_remote_launch(lcore={},queue={}): "
                             "rc={}", lcore_id, worker_idx, rc);
                launch_ok = false;
                break;
            }
            SPDLOG_DEBUG("cell n={}: launched worker lcore={} on queue={}",
                         n, lcore_id, worker_idx);
            ++worker_idx;
        }
        if (launch_ok && worker_idx < nb_rx_queues) {
            SPDLOG_ERROR("cell n={}: only {} worker lcore(s) launched, "
                         "need {}", n, worker_idx, nb_rx_queues);
            launch_ok = false;
        }

        // Subscribe pass: send one tiny WS frame per stream. Mock
        // gates push activation on this frame's arrival.
        if (launch_ok) {
            const std::array<uint8_t, 1> hello{0x73};  // 's'
            for (uint16_t i = 0; i < n; ++i) {
                auto sr = conns[i]->stream->send(
                    std::span<const uint8_t>{hello.data(), hello.size()});
                if (!sr) {
                    SPDLOG_ERROR("subscribe(conn={}): {}",
                                 i, sr.error().detail);
                }
            }

            while (bench::monotonic_raw_ns() < deadline_ns &&
                   !bench::shutdown_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds{50});
            }
        }

        // Stop and join workers BEFORE close_gracefully — the close
        // drain below polls each Poller from the main thread, and
        // worker lcores must not race that. `rte_eal_wait_lcore`
        // returns the lcore to WAIT state, ready for the next cell's
        // `rte_eal_remote_launch`.
        stop_workers.store(true, std::memory_order_relaxed);
        RTE_LCORE_FOREACH_WORKER(lcore_id) {
            (void)rte_eal_wait_lcore(lcore_id);
        }
        if (!launch_ok) return CellResult{n, 0, std::nullopt, {}, {}, {}};
#else
        // Kernel: send subscribe frames now, then spin the poller for
        // the measurement window.
        {
            const std::array<uint8_t, 1> hello{0x73};
            for (uint16_t i = 0; i < n; ++i) {
                auto sr = conns[i]->stream->send(
                    std::span<const uint8_t>{hello.data(), hello.size()});
                if (!sr) {
                    SPDLOG_ERROR("subscribe(conn={}): {}",
                                 i, sr.error().detail);
                }
            }
        }
        while (bench::monotonic_raw_ns() < deadline_ns &&
               !bench::shutdown_requested()) {
            (void)poller->poll();
        }
#endif

        // Snapshot stats BEFORE close — close_gracefully + drain may
        // race with late callbacks.
        std::vector<uint64_t> p50s, p99s, p999s;
        eu::Recorder agg{std::string{"rss_scaling_ws_"} + kBackend +
                         "_n" + std::to_string(n) + "_aggregate"};
        uint64_t total_samples = 0;
        for (auto& c : conns) {
            if (auto s_opt = c->rec.compute_stats()) {
                p50s.push_back(s_opt->p50_ns);
                p99s.push_back(s_opt->p99_ns);
                p999s.push_back(s_opt->p999_ns);
                total_samples += s_opt->count;
            }
            (void)agg.merge(c->rec);
        }
        auto agg_stats = agg.compute_stats();

        // Close all streams. Drain pollers briefly to let FIN-ACKs land
        // before destructors unregister the flow table entries (avoids
        // ASan UAF on the DPDK dispatch path; on kernel it's just
        // cooperative).
        for (auto& c : conns) {
            if (c->stream) (void)c->stream->close_gracefully();
        }
#if defined(EPH_USE_DPDK)
        // Main thread drives the queue pollers to push the FIN/ACK/RST
        // packets through (workers already stopped above). 1.5 s is
        // generous: enough time for every cell-N stream's FIN to land,
        // mock to mark each fd dead via a failed write_all, and any
        // residual mock pushes already in flight to drain into the
        // closing PMD queue. Without this, the next cell's setup
        // races with mock's still-in-progress catch-up state and
        // drops SYN-ACKs around conn≈10.
        const uint64_t close_until =
            bench::monotonic_raw_ns() + 1500ull * 1'000'000ull;
        while (bench::monotonic_raw_ns() < close_until) {
            for (auto& p : pollers) (void)p->poll();
        }
        for (auto& c : conns) c->stream.reset();
        // Brief settle after streams unregister from Platform's flow
        // table so the next cell's create_and_attach starts with a
        // clean route_table on each Poller.
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
#else
        const uint64_t drain_until =
            bench::monotonic_raw_ns() + 300ull * 1'000'000ull;
        while (bench::monotonic_raw_ns() < drain_until) {
            (void)poller->poll();
        }
        for (auto& c : conns) {
            if (c->stream) (void)poller->remove(c->stream.get());
            c->stream.reset();
        }
#endif

        CellResult r{};
        r.conn_count    = n;
        r.total_samples = total_samples;
        r.agg           = agg_stats;
        r.p50_distro    = summarise(std::move(p50s));
        r.p99_distro    = summarise(std::move(p99s));
        r.p999_distro   = summarise(std::move(p999s));

        std::printf("\n--- cell n=%u ---\n", static_cast<unsigned>(n));
        std::printf("samples_total=%llu (window=%us)\n",
                    static_cast<unsigned long long>(r.total_samples),
                    duration_s);
        std::printf("per-connection percentile distributions (one-way RX_ns):\n");
        print_distro("P50 across conns",   r.p50_distro);
        print_distro("P99 across conns",   r.p99_distro);
        print_distro("P99.9 across conns", r.p999_distro);
        std::printf("aggregate (all samples merged):\n");
        if (r.agg) bench::print_stats_block("RX_one_way_ns", *r.agg);
        else       bench::print_stats_block_empty("RX_one_way_ns");
        std::fflush(stdout);

        SPDLOG_INFO("==== cell done: conn_count={} samples={} ====",
                    n, r.total_samples);
        return r;
    };

    // ── Sweep ────────────────────────────────────────────────────────
    std::vector<CellResult> results;
    results.reserve(conn_counts.size());
    for (auto n : conn_counts) {
        if (bench::shutdown_requested()) break;
        results.push_back(run_cell(n));
    }

    // ── Final summary table ──────────────────────────────────────────
    std::printf("\n═══════════════════════════════════════════════════════════════════\n");
    std::printf("  lat_rss_scaling_ws (%s) — sweep summary\n", kBackend);
    std::printf("═══════════════════════════════════════════════════════════════════\n");
    std::printf("\n%-6s | %-10s | %-10s | %-10s | %-10s | %-10s\n",
                "conns", "agg p50", "agg p90", "agg p99", "agg p99.9", "agg max");
    std::printf("%-6s-+-%-10s-+-%-10s-+-%-10s-+-%-10s-+-%-10s\n",
                "------", "----------", "----------", "----------",
                "----------", "----------");
    for (const auto& r : results) {
        if (r.agg) {
            std::printf("%-6u | %-10llu | %-10llu | %-10llu | %-10llu | %-10llu\n",
                        static_cast<unsigned>(r.conn_count),
                        static_cast<unsigned long long>(r.agg->p50_ns),
                        static_cast<unsigned long long>(r.agg->p90_ns),
                        static_cast<unsigned long long>(r.agg->p99_ns),
                        static_cast<unsigned long long>(r.agg->p999_ns),
                        static_cast<unsigned long long>(r.agg->max_ns));
        } else {
            std::printf("%-6u | (no samples)\n",
                        static_cast<unsigned>(r.conn_count));
        }
    }
    if (results.size() >= 2 && results.front().agg && results.back().agg) {
        const double dn = static_cast<double>(results.back().conn_count -
                                              results.front().conn_count);
        if (dn > 0) {
            const double p50_slope =
                (static_cast<double>(results.back().agg->p50_ns) -
                 static_cast<double>(results.front().agg->p50_ns)) / dn;
            const double p999_slope =
                (static_cast<double>(results.back().agg->p999_ns) -
                 static_cast<double>(results.front().agg->p999_ns)) / dn;
            std::printf("\nslope (last − first) / Δconn:  "
                        "agg p50 = %.1f ns/conn   agg p99.9 = %.1f ns/conn\n",
                        p50_slope, p999_slope);
        }
    }
    std::fflush(stdout);

    // ── Teardown ─────────────────────────────────────────────────────
#if defined(EPH_USE_DPDK)
    for (uint16_t q = 0; q < nb_rx_queues; ++q) {
        env.platform.unregister_poller(q);
    }
    pollers.clear();
#else
    poller.reset();
#endif

    return bench::shutdown_requested() ? 130 : 0;
}
