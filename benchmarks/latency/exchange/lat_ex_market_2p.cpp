/// @file lat_ex_market_2p.cpp
/// Two-phase multi-symbol bookTicker latency benchmark.
///
/// Compares two processing strategies for multi-symbol WebSocket market data:
///
///   mode=naive     Every on_message does full eph::json::parse() +
///                  BinanceBookTicker::from() before recording the sample.
///                  This is the straightforward approach.
///
///   mode=twophase  Phase 1 (on_message): lightweight symbol_hash() +
///                  scan_json_uint_field("T") + memcpy to per-symbol slot.
///                  Phase 2 (after poll()): full parse only for surviving
///                  slots (one per symbol — the latest frame wins).
///
/// The mock (`mockex --scenario ex_market_2p`) sends bursts of `burst_size`
/// frames per MMPP busy-regime tick, drawing from a rotating pool of real
/// Binance bookTicker frames with the `"T"` field patched in place on each
/// send. In a burst, the same symbol may appear multiple times; two-phase
/// deduplicates these so only the latest is parsed.
///
/// Latency measurement: a single timestamp `t_actionable = monotonic_raw_ns()`
/// stamped at the moment the business object (BookTicker) is parsed and
/// usable. `t_server` is extracted from the JSON `"T"` field.
/// Sample = `t_actionable - t_server`. This single point captures all three
/// effects that distinguish the two modes:
///   - HOL blocking (naive parse-inline stretches burst-tail wakeup time)
///   - Parse cost itself (naive pays per frame; twophase pays per slot)
///   - Dedup savings (in twophase, stale frames are overwritten and never
///     reach Phase 2 — they contribute zero samples, which is the point).
///
/// Config: [lat_ex_market_2p] in bench.conf
///   port, ws_path, push_rate_hz, duration_seconds, burst_size, symbols, mode

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <arpa/inet.h>        // inet_ntop
#include <netdb.h>            // getaddrinfo (Phase 4 real-server mode)
#include <netinet/in.h>
#include <sys/socket.h>

#include <spdlog/spdlog.h>

#include "eph/codec/ws_codec.hpp"
#include "eph/json/adapters/binance.hpp"
#include "eph/json/parser.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/utils/recorder.hpp"

#if defined(EPH_USE_DPDK)
#  include "eph/net/dpdk/poller.hpp"
#  include "eph/net/dpdk/tcp_stream.hpp"
#else
#  include "eph/net/kernel/config.hpp"
#  include "eph/net/kernel/poller.hpp"
#  include "eph/net/kernel/tcp_stream.hpp"
#endif

#include "core/config.hpp"
#include "core/endpoint.hpp"          // Phase 4: resolve_endpoint
#include "core/json_scan.hpp"
#include "core/measurement.hpp"
#if defined(EPH_USE_DPDK)
#  include "core/dpdk_env.hpp"
#endif

namespace {

namespace ec = eph::codec;
namespace ej = eph::json;
namespace eu = eph::utils;
namespace en = eph::net;

#if defined(EPH_USE_DPDK)
namespace ed = eph::net::dpdk;
using Stream    = ed::DpdkTcpStream<ec::WsCodec, /*EnableTls=*/false>;
using StreamTls = Stream;  // DPDK path doesn't support wss://
using Poller    = ed::DpdkPoller<>;
#else
namespace ek = eph::net::kernel;
using Stream    = ek::KernelTcpStream<ec::WsCodec, /*EnableTls=*/false>;
using StreamTls = ek::KernelTcpStream<ec::WsCodec, /*EnableTls=*/true>;
using Poller    = ek::KernelPoller;
#endif

constexpr const char* kDefaultConfigPath = "benchmarks/latency/bench.conf";
constexpr std::size_t kMaxSymbols = 64;
constexpr std::size_t kMaxFrameSize = 512;

// ── Per-symbol slot for two-phase mode ─────────────────────────────────
struct Slot {
    uint32_t hash     = 0;
    bool     active   = false;
    uint16_t len      = 0;
    uint64_t t_server = 0;
    uint8_t  data[kMaxFrameSize]{};
};

enum class Mode { kNaive, kTwophase };

[[nodiscard]] Mode parse_mode(std::string_view s) noexcept {
    if (s == "naive") return Mode::kNaive;
    return Mode::kTwophase;
}

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

[[nodiscard]] std::string_view parse_mode_override(int argc, char** argv) noexcept {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--mode") == 0) {
            return argv[i + 1];
        }
    }
    return {};
}

/// Templated measurement body — shared between the plain-TCP (mock) and
/// TLS (real-server) stream flavours. Takes ownership of the already-
/// connected stream and runs the full poll loop until `duration_s` or
/// SIGTERM, then prints the report and exports JSON.
///
/// All the heavy lifting (naive vs twophase lambda install, per-slot
/// Phase-2 processing after poll()) stays exactly as it was before
/// Phase 4 — only the stream type is abstracted.
template <class StreamT, class PollerT>
int run_measurement(std::unique_ptr<StreamT> stream,
                    std::unique_ptr<PollerT>& poller,
                    eu::Recorder& rec,
                    Mode mode,
                    uint32_t burst_size,
                    uint64_t duration_s,
                    uint64_t warmup_samples,
                    const char* mode_label,
                    const char* backend) noexcept {
    (void)burst_size;  // informational; handled by the mock

    uint64_t sample_idx      = 0;
    uint64_t total_frames    = 0;
    uint64_t parsed_frames   = 0;
    uint64_t malformed       = 0;
    uint64_t clock_skew      = 0;
    uint64_t slot_overflow   = 0;
    uint64_t t_measure_start = 0;

    Slot slots[kMaxSymbols]{};
    std::size_t n_slots = 0;

    if (mode == Mode::kNaive) {
        stream->on_message = [&](std::span<const uint8_t> app_frame) {
            ++total_frames;
            const auto* d = app_frame.data();
            const auto  n = app_frame.size();
            auto json_r = ej::parse(d, n);
            if (!json_r) { ++malformed; return; }
            auto ticker = ej::binance::BookTicker::from(json_r.value());
            if (!ticker) { ++malformed; return; }
            ++parsed_frames;
            auto t_server_opt = bench::scan_json_uint_field(d, n, "T");
            if (!t_server_opt) { ++malformed; return; }
            const uint64_t t_server = *t_server_opt;
            const uint64_t t_actionable = bench::monotonic_raw_ns();
            if (t_actionable <= t_server) { ++clock_skew; return; }
            if (sample_idx == warmup_samples) t_measure_start = t_actionable;
            if (sample_idx >= warmup_samples) rec.record_ns(t_actionable - t_server);
            ++sample_idx;
        };
    } else {
        stream->on_message = [&](std::span<const uint8_t> app_frame) {
            ++total_frames;
            const auto* d = app_frame.data();
            const auto  n = app_frame.size();
            const uint32_t hash = ej::binance::symbol_hash(d, n);
            if (hash == 0) { ++malformed; return; }
            auto t_server_opt = bench::scan_json_uint_field(d, n, "T");
            if (!t_server_opt) { ++malformed; return; }
            Slot* target = nullptr;
            for (std::size_t i = 0; i < n_slots; ++i) {
                if (slots[i].hash == hash) { target = &slots[i]; break; }
            }
            if (!target) {
                if (n_slots >= kMaxSymbols) { ++slot_overflow; return; }
                target = &slots[n_slots++];
                target->hash = hash;
            }
            const uint16_t copy_len =
                static_cast<uint16_t>(std::min<std::size_t>(n, kMaxFrameSize));
            std::memcpy(target->data, d, copy_len);
            target->len      = copy_len;
            target->t_server = *t_server_opt;
            target->active   = true;
        };
    }

    // Stream is pre-attached by the caller — DPDK via Stream::create_and_attach,
    // kernel via an explicit poller->add in main(). We don't re-attach here.

    const uint64_t t_start = bench::monotonic_raw_ns();
    const uint64_t t_deadline =
        t_start + (duration_s + 2) * 1'000'000'000ull;

    while (bench::monotonic_raw_ns() < t_deadline &&
           !bench::shutdown_requested()) {
        poller->poll();
        if (mode == Mode::kTwophase) {
            for (std::size_t i = 0; i < n_slots; ++i) {
                if (!slots[i].active) continue;
                slots[i].active = false;
                auto json_r = ej::parse(slots[i].data, slots[i].len);
                if (!json_r) { ++malformed; continue; }
                auto ticker = ej::binance::BookTicker::from(json_r.value());
                if (!ticker) { ++malformed; continue; }
                ++parsed_frames;
                const uint64_t t_actionable = bench::monotonic_raw_ns();
                if (t_actionable <= slots[i].t_server) { ++clock_skew; continue; }
                if (sample_idx == warmup_samples) t_measure_start = t_actionable;
                if (sample_idx >= warmup_samples)
                    rec.record_ns(t_actionable - slots[i].t_server);
                ++sample_idx;
            }
        }
    }

    if (malformed > 0 || clock_skew > 0 || slot_overflow > 0) {
        std::fprintf(stderr,
            "lat_ex_market_2p: skipped %llu malformed + %llu clock-skew"
            " + %llu slot-overflow samples\n",
            static_cast<unsigned long long>(malformed),
            static_cast<unsigned long long>(clock_skew),
            static_cast<unsigned long long>(slot_overflow));
    }

    const uint64_t wall_time_ns =
        (t_measure_start != 0)
            ? (bench::monotonic_raw_ns() - t_measure_start)
            : 0;
    bench::print_report("lat_ex_market_2p", backend, rec,
                        warmup_samples, wall_time_ns);

    const double skip_ratio =
        (total_frames > 0)
            ? 1.0 - static_cast<double>(parsed_frames) /
                     static_cast<double>(total_frames)
            : 0.0;
    std::printf("\n--- two-phase stats ---\n");
    std::printf("mode:          %s\n", mode_label);
    std::printf("total_frames:  %llu\n",
                static_cast<unsigned long long>(total_frames));
    std::printf("parsed_frames: %llu\n",
                static_cast<unsigned long long>(parsed_frames));
    std::printf("skip_ratio:    %.2f%%\n", skip_ratio * 100.0);
    if (mode == Mode::kTwophase) {
        std::printf("active_slots:  %zu\n", n_slots);
    }
    std::fflush(stdout);

    if (!rec.export_json("benchmarks/latency/outputs")) {
        std::fprintf(stderr,
                     "[WARN] lat_ex_market_2p: export_json to "
                     "'benchmarks/latency/outputs' failed\n");
    }

    (void)stream->close_gracefully();
#if !defined(EPH_USE_DPDK)
    poller->poll(std::chrono::milliseconds{50});
#endif
    (void)poller->remove(stream.get());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::info);

    const char* conf_path = parse_config_path(argc, argv);

    // ── Load config ────────────────────────────────────────────────────
    auto globals_r = bench::ScenarioConfig::load_globals(conf_path);
    if (!globals_r) {
        std::fprintf(stderr, "lat_ex_market_2p: %s\n", globals_r.error().c_str());
        return 1;
    }
    auto scenario_r = bench::ScenarioConfig::load(conf_path, "lat_ex_market_2p");
    if (!scenario_r) {
        std::fprintf(stderr, "lat_ex_market_2p: %s\n", scenario_r.error().c_str());
        return 1;
    }
    const auto& globals  = globals_r.value();
    const auto& scenario = scenario_r.value();

    auto port_r = scenario.get_u32("port");
    if (!port_r) {
        std::fprintf(stderr, "lat_ex_market_2p: %s\n", port_r.error().c_str());
        return 1;
    }
    const uint16_t port = static_cast<uint16_t>(port_r.value());

    const std::string ws_path = scenario.get_string("ws_path", "/ws/bookticker");

    auto duration_r = scenario.get_u32("duration_seconds", 300);
    if (!duration_r) {
        std::fprintf(stderr, "lat_ex_market_2p: %s\n", duration_r.error().c_str());
        return 1;
    }
    const uint64_t duration_s = duration_r.value();

    const uint32_t push_rate_hz = scenario.get_u32("push_rate_hz", 10000)
                                      .value_or(10000);
    const uint32_t burst_size = scenario.get_u32("burst_size", 10)
                                    .value_or(10);

    // Mode: CLI --mode overrides bench.conf.
    auto mode_override = parse_mode_override(argc, argv);
    const std::string mode_str =
        mode_override.empty()
            ? scenario.get_string("mode", "twophase")
            : std::string{mode_override};
    const Mode mode = parse_mode(mode_str);

    auto warmup_r = globals.get_u64("warmup_samples", 1000);
    if (!warmup_r) {
        std::fprintf(stderr, "lat_ex_market_2p: %s\n", warmup_r.error().c_str());
        return 1;
    }
    const uint64_t warmup_samples = warmup_r.value();

    // Phase 4: resolve endpoint (mock or wss://…).
    auto endpoint_r = bench::resolve_endpoint(globals, scenario);
    if (!endpoint_r) {
        std::fprintf(stderr, "lat_ex_market_2p: %s\n", endpoint_r.error().c_str());
        return 1;
    }
    const auto endpoint = std::move(*endpoint_r);

#if defined(EPH_USE_DPDK)
    if (endpoint.is_real_server) {
        std::fprintf(stderr,
            "lat_ex_market_2p: real-server endpoint is not supported on the "
            "DPDK build — the PMD path has no outbound WAN routing.\n");
        return 1;
    }
#endif

    // Resolve host → IP. For "mock" the value is already an IPv4 literal
    // from bench.conf. For wss:// real-server it may be a DNS name — fall
    // back to getaddrinfo then feed the resolved IP to Ipv4Addr::parse.
    const std::string host_str = endpoint.host;
    auto ip_r = en::Ipv4Addr::parse(host_str);
    std::string resolved_ip = host_str;
    if (!ip_r) {
        if (!endpoint.is_real_server) {
            std::fprintf(stderr, "lat_ex_market_2p: invalid mock_ip '%s': %s\n",
                         host_str.c_str(), ip_r.error().detail);
            return 1;
        }
        struct addrinfo hints{};
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* ai = nullptr;
        const int gai = ::getaddrinfo(host_str.c_str(), nullptr, &hints, &ai);
        if (gai != 0 || ai == nullptr) {
            std::fprintf(stderr,
                "lat_ex_market_2p: getaddrinfo('%s') failed: %s\n",
                host_str.c_str(), ::gai_strerror(gai));
            if (ai) ::freeaddrinfo(ai);
            return 1;
        }
        char buf[INET_ADDRSTRLEN];
        const auto* sin = reinterpret_cast<const sockaddr_in*>(ai->ai_addr);
        ::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
        resolved_ip = buf;
        ::freeaddrinfo(ai);
        ip_r = en::Ipv4Addr::parse(resolved_ip);
        if (!ip_r) {
            std::fprintf(stderr,
                "lat_ex_market_2p: failed to re-parse resolved IP '%s'\n",
                resolved_ip.c_str());
            return 1;
        }
        std::printf("lat_ex_market_2p: DNS %s → %s\n",
                    host_str.c_str(), resolved_ip.c_str());
    }
    const en::SocketAddr remote{ip_r.value(), endpoint.port};
    const std::string effective_ws_path =
        endpoint.is_real_server ? endpoint.ws_path : ws_path;

    const char* mode_label = (mode == Mode::kNaive) ? "naive" : "twophase";
    std::printf("=== lat_ex_market_2p ===\n");
    std::printf("config: target=%s:%u ws_path=%s push_rate_hz=%u "
                "burst_size=%u mode=%s duration=%llus warmup=%llu kind=%s\n",
                endpoint.host.c_str(),
                static_cast<unsigned>(endpoint.port),
                effective_ws_path.c_str(),
                static_cast<unsigned>(push_rate_hz),
                static_cast<unsigned>(burst_size),
                mode_label,
                static_cast<unsigned long long>(duration_s),
                static_cast<unsigned long long>(warmup_samples),
                endpoint.is_real_server ? "real-server+tls" : "mock");
    std::fflush(stdout);
    (void)port;  // superseded by endpoint.port

    bench::install_signal_handler();

    // Construct Recorder early so TSC calibration happens before connection.
    const char* backend =
#if defined(EPH_USE_DPDK)
        "dpdk";
#else
        "kernel";
#endif
    eu::Recorder rec{std::string{"lat_ex_market_2p_"} + backend +
                     "_" + mode_label + "_oneway"};

    // ── Transport setup ────────────────────────────────────────────────
#if defined(EPH_USE_DPDK)
    auto env_r = bench::load_dpdk_env(globals, /*port_id=*/0);
    if (!env_r) {
        std::fprintf(stderr, "lat_ex_market_2p: %s\n", env_r.error().c_str());
        return 1;
    }
    auto env = std::move(*env_r);
    bench::print_dpdk_config_echo(env);

    ed::PollerConfig poller_cfg{};
    poller_cfg.port_id     = env.port_id;
    poller_cfg.rx_queue_id = 0;
    auto poller_r = Poller::create(poller_cfg);
#else
    auto poller_r = ek::KernelPoller::create({});
#endif
    if (!poller_r) {
        std::fprintf(stderr, "lat_ex_market_2p: Poller::create failed: %s\n",
                     poller_r.error().detail);
        return 2;
    }
    auto poller = std::move(poller_r.value());

#if defined(EPH_USE_DPDK)
    ed::StreamConfig cfg{};
    cfg.legacy          = env.make_tcp_config(bench::random_src_port(),
                                              endpoint.port);
    cfg.pool            = env.pool;
    cfg.connect_timeout = std::chrono::milliseconds{3000};
    cfg.ws_path         = effective_ws_path;
    cfg.ws_timeout      = std::chrono::seconds{10};

    if (auto rr = env.platform.register_poller(0, poller.get()); !rr) {
        std::fprintf(stderr, "lat_ex_market_2p: register_poller failed: %s\n",
                     rr.error().c_str());
        return 3;
    }
    auto stream_r = Stream::create_and_attach(std::move(cfg), env.platform);
    if (!stream_r) {
        std::fprintf(stderr,
                     "lat_ex_market_2p: Stream::create_and_attach failed: %s\n",
                     stream_r.error().detail);
        return 3;
    }
    const int rc = run_measurement(std::move(stream_r.value()), poller,
                                   rec, mode, burst_size, duration_s,
                                   warmup_samples, mode_label, backend);
    poller.reset();
    return rc;
#else
    // Kernel backend: pick the TLS or plain stream typedef at runtime
    // based on the endpoint. The template-on-stream body below handles
    // both cases identically.
    ek::StreamConfig cfg{};
    cfg.reasm_capacity  = 256 * 1024;
    cfg.connect_timeout = std::chrono::milliseconds{3000};
    cfg.ws_path         = effective_ws_path;
    cfg.ws_timeout      = std::chrono::seconds{10};

    int rc = 0;
    // Common kernel-side helper: explicit poller->add (DPDK pre-attaches
    // via create_and_attach above; this branch covers TLS + plain).
    auto attach_and_run = [&](auto stream_uptr) -> int {
        if (auto r = poller->add(stream_uptr.get()); !r) {
            std::fprintf(stderr,
                         "lat_ex_market_2p: poller->add failed: %s\n",
                         r.error().detail);
            return 3;
        }
        return run_measurement(std::move(stream_uptr), poller,
                               rec, mode, burst_size, duration_s,
                               warmup_samples, mode_label, backend);
    };

    if (endpoint.is_real_server) {
        cfg.remote       = remote;
        cfg.tls.hostname = endpoint.host;
        cfg.ws_host      = endpoint.host;
        auto stream_r = StreamTls::create(cfg);
        if (!stream_r) {
            std::fprintf(stderr,
                "lat_ex_market_2p: StreamTls::create('%s:%u%s') failed: %s\n",
                endpoint.host.c_str(), endpoint.port,
                effective_ws_path.c_str(),
                stream_r.error().detail);
            return 3;
        }
        rc = attach_and_run(std::move(stream_r.value()));
    } else {
        cfg.remote = remote;
        auto stream_r = Stream::create(cfg);
        if (!stream_r) {
            std::fprintf(stderr, "lat_ex_market_2p: Stream::create failed: %s\n",
                         stream_r.error().detail);
            return 3;
        }
        rc = attach_and_run(std::move(stream_r.value()));
    }
    poller.reset();
    return rc;
#endif
}

