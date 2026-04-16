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
/// The mock (`ex_market_2p_push.py`) sends bursts of `burst_size` frames per
/// tick, with symbols chosen randomly. In a burst, the same symbol may appear
/// multiple times; two-phase deduplicates these so only the latest is parsed.
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
#include <string>
#include <string_view>
#include <utility>

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
using Stream = ed::DpdkTcpStream<ec::WsCodec, /*EnableTls=*/false>;
using Poller = ed::DpdkPoller<>;
#else
namespace ek = eph::net::kernel;
using Stream = ek::KernelTcpStream<ec::WsCodec, /*EnableTls=*/false>;
using Poller = ek::KernelPoller;
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

    const std::string mock_ip_str = globals.get_string("mock_ip", "127.0.0.1");
    auto ip_r = en::Ipv4Addr::parse(mock_ip_str);
    if (!ip_r) {
        std::fprintf(stderr, "lat_ex_market_2p: invalid mock_ip '%s': %s\n",
                     mock_ip_str.c_str(), ip_r.error().detail);
        return 1;
    }
    const en::SocketAddr remote{ip_r.value(), port};

    const char* mode_label = (mode == Mode::kNaive) ? "naive" : "twophase";
    std::printf("=== lat_ex_market_2p ===\n");
    std::printf("config: mock=%s port=%u ws_path=%s push_rate_hz=%u "
                "burst_size=%u mode=%s duration=%llus warmup=%llu\n",
                mock_ip_str.c_str(),
                static_cast<unsigned>(port),
                ws_path.c_str(),
                static_cast<unsigned>(push_rate_hz),
                static_cast<unsigned>(burst_size),
                mode_label,
                static_cast<unsigned long long>(duration_s),
                static_cast<unsigned long long>(warmup_samples));
    std::fflush(stdout);

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
    cfg.legacy          = env.make_tcp_config(bench::random_src_port(), port);
    cfg.pool            = env.pool;
    cfg.connect_timeout = std::chrono::milliseconds{3000};
    cfg.ws_path         = ws_path;
    cfg.ws_timeout      = std::chrono::seconds{10};
#else
    ek::StreamConfig cfg{};
    cfg.remote          = remote;
    cfg.reasm_capacity  = 256 * 1024;
    cfg.connect_timeout = std::chrono::milliseconds{3000};
    cfg.ws_path         = ws_path;
    cfg.ws_timeout      = std::chrono::seconds{10};
#endif

    auto stream_r = Stream::create(cfg);
    if (!stream_r) {
        std::fprintf(stderr, "lat_ex_market_2p: Stream::create failed: %s\n",
                     stream_r.error().detail);
        return 3;
    }
    auto stream = std::move(stream_r.value());

    // ── Measurement state ──────────────────────────────────────────────
    uint64_t sample_idx      = 0;
    uint64_t total_frames    = 0;
    uint64_t parsed_frames   = 0;
    uint64_t malformed       = 0;
    uint64_t clock_skew      = 0;
    uint64_t slot_overflow   = 0;
    uint64_t t_measure_start = 0;

    // Two-phase state: per-symbol slots.
    Slot slots[kMaxSymbols]{};
    std::size_t n_slots = 0;

    if (mode == Mode::kNaive) {
        // ── Naive mode: full parse every frame ─────────────────────────
        stream->on_message = [&](const uint8_t* d, uint16_t n) {
            ++total_frames;

            // Full JSON parse + BookTicker extraction (the expensive part).
            auto json_r = ej::parse(d, n);
            if (!json_r) {
                ++malformed;
                return;
            }
            auto ticker = ej::binance::BookTicker::from(json_r.value());
            if (!ticker) {
                ++malformed;
                return;
            }
            ++parsed_frames;

            // Extract server timestamp for latency measurement.
            auto t_server_opt = bench::scan_json_uint_field(d, n, "T");
            if (!t_server_opt) {
                ++malformed;
                return;
            }
            const uint64_t t_server = *t_server_opt;

            // Single instrumentation point: stamp after the BookTicker is
            // actionable. In naive mode this is inline in on_message, so
            // burst-tail frames accumulate both HOL blocking and full
            // parse cost before `t_actionable` is taken.
            const uint64_t t_actionable = bench::monotonic_raw_ns();
            if (t_actionable <= t_server) {
                ++clock_skew;
                return;
            }

            if (sample_idx == warmup_samples) {
                t_measure_start = t_actionable;
            }
            if (sample_idx >= warmup_samples) {
                rec.record_ns(t_actionable - t_server);
            }
            ++sample_idx;
        };
    } else {
        // ── Two-phase mode: lightweight Phase 1 in on_message ──────────
        stream->on_message = [&](const uint8_t* d, uint16_t n) {
            ++total_frames;

            // Phase 1: extract symbol hash + T field, copy data to the
            // per-symbol slot. No timestamp is taken here — the single
            // instrumentation point lives in Phase 2, so dedup-overwritten
            // stale frames contribute zero samples (that omission is what
            // two-phase trades parse latency for).
            const uint32_t hash = ej::binance::symbol_hash(d, n);
            if (hash == 0) {
                ++malformed;
                return;
            }

            auto t_server_opt = bench::scan_json_uint_field(d, n, "T");
            if (!t_server_opt) {
                ++malformed;
                return;
            }

            // Find or create slot for this symbol hash.
            Slot* target = nullptr;
            for (std::size_t i = 0; i < n_slots; ++i) {
                if (slots[i].hash == hash) {
                    target = &slots[i];
                    break;
                }
            }
            if (!target) {
                if (n_slots >= kMaxSymbols) {
                    ++slot_overflow;
                    return;
                }
                target = &slots[n_slots++];
                target->hash = hash;
            }

            // Overwrite slot with latest frame data.
            const uint16_t copy_len =
                static_cast<uint16_t>(std::min<std::size_t>(n, kMaxFrameSize));
            std::memcpy(target->data, d, copy_len);
            target->len      = copy_len;
            target->t_server = *t_server_opt;
            target->active   = true;
        };
    }

    if (auto r = poller->add(stream.get()); !r) {
        std::fprintf(stderr, "lat_ex_market_2p: poller->add failed: %s\n",
                     r.error().detail);
        return 3;
    }

    // ── Poll loop ──────────────────────────────────────────────────────
    const uint64_t t_start = bench::monotonic_raw_ns();
    const uint64_t t_deadline =
        t_start + (duration_s + 2) * 1'000'000'000ull;

    while (bench::monotonic_raw_ns() < t_deadline && !bench::shutdown_requested()) {
        poller->poll();

        // Two-phase Phase 2: process surviving slots after each poll().
        if (mode == Mode::kTwophase) {
            for (std::size_t i = 0; i < n_slots; ++i) {
                if (!slots[i].active) continue;
                slots[i].active = false;

                // Full JSON parse (the expensive part — only for latest per symbol).
                auto json_r = ej::parse(slots[i].data, slots[i].len);
                if (!json_r) {
                    ++malformed;
                    continue;
                }
                auto ticker = ej::binance::BookTicker::from(json_r.value());
                if (!ticker) {
                    ++malformed;
                    continue;
                }
                ++parsed_frames;

                // Single instrumentation point: stamp after the BookTicker
                // is actionable. Dedup-overwritten frames never reach here,
                // which is why twophase's sample count < total_frames.
                const uint64_t t_actionable = bench::monotonic_raw_ns();
                if (t_actionable <= slots[i].t_server) {
                    ++clock_skew;
                    continue;
                }

                if (sample_idx == warmup_samples) {
                    t_measure_start = t_actionable;
                }
                if (sample_idx >= warmup_samples) {
                    rec.record_ns(t_actionable - slots[i].t_server);
                }
                ++sample_idx;
            }
        }
    }

    // ── Report ─────────────────────────────────────────────────────────
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

    // Print standard latency report.
    bench::print_report("lat_ex_market_2p", backend, rec,
                        warmup_samples, wall_time_ns);

    // Print two-phase specific stats.
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

    // JSON export.
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
    stream.reset();
    poller.reset();

    return 0;
}
