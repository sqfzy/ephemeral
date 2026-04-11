/// @file lat_ex_order.cpp
/// Phase 10 latency benchmark: N-inflight pipelined WebSocket order RTT
/// against the Python echo mock `benchmarks/latency/mocks/ex_order_echo.py`.
///
/// Sub-phase 10.5. Per
/// `.artifacts/plan-phase-10-latency-bench-20260411-040540.md` §Sub-phase
/// 10.5 and §Interface design → "Client API pattern" (N-inflight variant).
///
/// Semantics:
///
///   * The client maintains up to `inflight` outstanding orders. Each
///     order gets a unique strictly-incrementing `id` and a `t_send`
///     CLOCK_MONOTONIC_RAW timestamp.
///   * For each in-flight slot the client sends a WS-binary frame with
///     payload `{"e":"NewOrder","id":N}`. `KernelTcpStream::send` does
///     NOT run bytes through the codec encode path, so the client
///     pre-encodes each frame via `WsCodec::encode` into a stack buffer.
///   * The mock (ex_order_echo.py) is a plain WS server that echoes each
///     received binary frame verbatim. The payload shape is preserved,
///     so the `"id":N` field round-trips back intact.
///   * On each received frame, `on_message` extracts the `id` via
///     `scan_json_uint_field(..., "id")` (plan D-5 — no eph-json), looks
///     up the slot in a 128-entry fixed-size table keyed by `id % 128`,
///     and records `t_recv - slot.t_send_ns` into the Recorder when the
///     slot is valid and past warmup.
///   * Loop terminates on whichever happens first: duration deadline,
///     `order_count + warmup_samples` orders completed, or SIGINT.
///
/// Measurement clock: `bench::monotonic_raw_ns()` (plan D-6). Same clock
/// as the other 10.3/10.4 scenarios — kernel-only one-way comparison is
/// not in scope here (this is RTT), but the clock choice keeps the
/// timestamps consistent across the lat suite.
///
/// Pipelining sanity: at `inflight=1` the scenario degenerates to
/// classic one-at-a-time RTT, which should print numbers close to
/// lat_ws (both are WS-binary echo round-trip measurements on the same
/// mock framework). At higher inflight the mean sample reflects the
/// mock's server-side service time per frame plus wire time; p99
/// exposes head-of-line blocking.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

// eph-* headers (plan D-4: v3.3 API only).
#include "eph/codec/ws_codec.hpp"
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

namespace {

namespace ec = eph::codec;
namespace eu = eph::utils;
namespace en = eph::net;

#if defined(EPH_USE_DPDK)
namespace ed = eph::net::dpdk;
using DpdkStream = ed::DpdkTcpStream<ec::WsCodec, /*EnableTls=*/false>;
using DpdkPoller = ed::DpdkPoller<>;
static_assert(sizeof(DpdkStream*) > 0);
static_assert(sizeof(DpdkPoller*) > 0);
#else
namespace ek = eph::net::kernel;
using Stream = ek::KernelTcpStream<ec::WsCodec, /*EnableTls=*/false>;
#endif

/// Fixed-size id→slot correlation table. Size is a hard cap on inflight
/// depth — 128 comfortably covers realistic pipeline windows (the plan
/// puts the bench.conf default at 16 and expects users to sweep 1/4/16/64)
/// while keeping the slot math to a single `id & 127` modulo on the hot
/// path. Slots are re-used as orders complete; a stale slot (in_flight
/// cleared before the response arrives) is a broken-mock signal, not a
/// capacity issue.
constexpr std::size_t kSlotCount = 128;

struct Slot {
    uint64_t order_id{0};    ///< zero means "free"
    uint64_t t_send_ns{0};
    bool     in_flight{false};
};

constexpr const char* kDefaultConfigPath = "benchmarks/latency/bench.conf";

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

#if defined(EPH_USE_DPDK)
int main(int /*argc*/, char** /*argv*/) {
    spdlog::set_level(spdlog::level::info);
    std::printf("lat_ex_order_dpdk: v3.3 DpdkTcpStream<WsCodec> API compiled.\n"
                "Real DPDK measurement loop is deferred to a follow-up phase "
                "(EAL init + vfio-pci NIC plumbing). Use lat_ex_order (kernel) "
                "for live measurements.\n");
    return 0;
}
#else
int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::info);

    const char* conf_path = parse_config_path(argc, argv);

    // ── Load config: globals + [lat_ex_order] section ───────────────────
    auto globals_r = bench::ScenarioConfig::load_globals(conf_path);
    if (!globals_r) {
        std::fprintf(stderr, "lat_ex_order: %s\n", globals_r.error().c_str());
        return 1;
    }
    auto scenario_r = bench::ScenarioConfig::load(conf_path, "lat_ex_order");
    if (!scenario_r) {
        std::fprintf(stderr, "lat_ex_order: %s\n", scenario_r.error().c_str());
        return 1;
    }
    const auto& globals  = globals_r.value();
    const auto& scenario = scenario_r.value();

    auto port_r = scenario.get_u32("port");
    if (!port_r) {
        std::fprintf(stderr, "lat_ex_order: %s\n", port_r.error().c_str());
        return 1;
    }
    const uint16_t port = static_cast<uint16_t>(port_r.value());

    const std::string ws_path = scenario.get_string("ws_path", "/ws/order");

    auto inflight_r = scenario.get_u32("inflight", 16);
    if (!inflight_r) {
        std::fprintf(stderr, "lat_ex_order: %s\n", inflight_r.error().c_str());
        return 1;
    }
    const uint32_t inflight_cap = inflight_r.value();
    if (inflight_cap == 0 || inflight_cap > kSlotCount) {
        std::fprintf(stderr,
                     "lat_ex_order: inflight=%u must be in [1, %zu]\n",
                     static_cast<unsigned>(inflight_cap), kSlotCount);
        return 1;
    }

    auto order_count_r = scenario.get_u64("order_count", 10000);
    if (!order_count_r) {
        std::fprintf(stderr, "lat_ex_order: %s\n", order_count_r.error().c_str());
        return 1;
    }
    const uint64_t order_count = order_count_r.value();

    auto duration_r = scenario.get_u32("duration_seconds", 30);
    if (!duration_r) {
        std::fprintf(stderr, "lat_ex_order: %s\n", duration_r.error().c_str());
        return 1;
    }
    const uint64_t duration_s = duration_r.value();

    auto warmup_r = globals.get_u64("warmup_samples", 1000);
    if (!warmup_r) {
        std::fprintf(stderr, "lat_ex_order: %s\n", warmup_r.error().c_str());
        return 1;
    }
    const uint64_t warmup_samples = warmup_r.value();

    const std::string mock_ip_str = globals.get_string("mock_ip", "127.0.0.1");
    auto ip_r = en::Ipv4Addr::parse(mock_ip_str);
    if (!ip_r) {
        std::fprintf(stderr, "lat_ex_order: invalid mock_ip '%s': %s\n",
                     mock_ip_str.c_str(), ip_r.error().detail);
        return 1;
    }
    const en::SocketAddr remote{ip_r.value(), port};

    std::printf("=== lat_ex_order ===\n");
    std::printf("config: mock=%s port=%u ws_path=%s inflight=%u order_count=%llu "
                "duration=%llus warmup_samples=%llu\n",
                mock_ip_str.c_str(),
                static_cast<unsigned>(port),
                ws_path.c_str(),
                static_cast<unsigned>(inflight_cap),
                static_cast<unsigned long long>(order_count),
                static_cast<unsigned long long>(duration_s),
                static_cast<unsigned long long>(warmup_samples));
    std::fflush(stdout);

    bench::install_signal_handler();

    // Construct the Recorder BEFORE Stream::create so that TSC::init's
    // ~1 s calibration spin runs at startup, not while the WS handshake
    // is completing. Same pattern as lat_ex_market (10.4 lesson).
    eu::Recorder rec{"lat_ex_order"};

    auto poller_r = ek::KernelPoller::create({});
    if (!poller_r) {
        std::fprintf(stderr, "lat_ex_order: Poller::create failed: %s\n",
                     poller_r.error().detail);
        return 1;
    }
    auto poller = std::move(poller_r.value());

    ek::StreamConfig cfg{};
    cfg.remote          = remote;
    // Order payloads are tiny (~30 B); 64 KiB reassembly is ample.
    cfg.reasm_capacity  = 64 * 1024;
    cfg.connect_timeout = std::chrono::milliseconds{3000};
    cfg.ws_path         = ws_path;
    cfg.ws_timeout      = std::chrono::seconds{10};

    auto stream_r = Stream::create(cfg);
    if (!stream_r) {
        std::fprintf(stderr, "lat_ex_order: Stream::create failed: %s\n",
                     stream_r.error().detail);
        return 2;
    }
    auto stream = std::move(stream_r.value());

    // ── Correlation table + counters (captured by reference below) ──────
    //
    // All of these live on the main() stack and outlive the poller, so
    // the on_message lambda is safe to take them by reference.
    std::array<Slot, kSlotCount> table{};
    uint64_t next_id         = 1;     // 0 is the "free" sentinel
    uint32_t inflight_count  = 0;
    uint64_t sample_idx      = 0;     // completed orders, incl. warmup
    uint64_t recorded_count  = 0;     // post-warmup recorded samples
    uint64_t stale_count     = 0;     // id matched no in-flight slot
    uint64_t malformed_count = 0;     // payload had no "id":N field

    stream->on_message = [&](const uint8_t* d, uint16_t n) {
        const uint64_t t_recv = bench::monotonic_raw_ns();

        auto id_opt = bench::scan_json_uint_field(d, static_cast<std::size_t>(n), "id");
        if (!id_opt) {
            ++malformed_count;
            return;
        }
        const uint64_t id = *id_opt;
        const std::size_t slot_idx = static_cast<std::size_t>(id % kSlotCount);
        auto& slot = table[slot_idx];

        // Guard against collisions: a response with an `id` that no
        // longer matches the slot's in-flight order is either a
        // duplicate echo from the mock or a stale leftover frame from
        // a previous iteration. Skip rather than pollute the histogram.
        if (!slot.in_flight || slot.order_id != id) {
            ++stale_count;
            return;
        }

        if (sample_idx >= warmup_samples) {
            rec.record_ns(t_recv - slot.t_send_ns);
            ++recorded_count;
        }
        ++sample_idx;

        slot.in_flight = false;
        slot.order_id  = 0;
        if (inflight_count > 0) --inflight_count;
    };

    if (auto r = poller->add(stream.get()); !r) {
        std::fprintf(stderr, "lat_ex_order: poller->add failed: %s\n",
                     r.error().detail);
        return 3;
    }

    // ── Encode buffer for outbound WS-binary frames ─────────────────────
    //
    // Each sample encodes `{"e":"NewOrder","id":NNNNNNNNNNNNNNNNNNNN}` —
    // worst case is 20-digit uint64, giving 42 bytes of JSON. A 64-byte
    // JSON buffer plus WsCodec::max_overhead covers every case.
    constexpr std::size_t kJsonBufSize = 64;
    char      json_buf[kJsonBufSize];
    std::vector<uint8_t> frame(ec::WsCodec::max_overhead + kJsonBufSize);
    ec::WsCodec encoder{};

    // ── Measurement loop ────────────────────────────────────────────────
    //
    // Total orders to issue = order_count + warmup_samples (the warmup
    // prefix is not counted toward `order_count`). We fill the pipeline
    // up to `inflight_cap`, poll once to drain completions, and repeat.
    //
    // Exit conditions:
    //   1. duration_seconds deadline elapses
    //   2. all planned orders completed AND pipeline drained
    //   3. SIGINT/SIGTERM sets the shutdown flag
    //   4. send() error (mock died) — bail with a diagnostic
    const uint64_t t_start = bench::monotonic_raw_ns();
    const uint64_t t_deadline =
        t_start + (duration_s + 2) * 1'000'000'000ull;
    const uint64_t plan_total = order_count + warmup_samples;

    bool send_failed = false;
    while (!bench::shutdown_requested()) {
        const uint64_t now = bench::monotonic_raw_ns();
        if (now >= t_deadline) break;

        // Fill the pipeline up to the cap, subject to total-order bound.
        while (inflight_count < inflight_cap && next_id <= plan_total) {
            const uint64_t id = next_id;
            const std::size_t slot_idx = static_cast<std::size_t>(id % kSlotCount);
            auto& slot = table[slot_idx];

            // Should never trigger given inflight_cap <= kSlotCount, but
            // defends against future code motion that raises the cap.
            if (slot.in_flight) break;

            const int jn = std::snprintf(
                json_buf, sizeof(json_buf),
                "{\"e\":\"NewOrder\",\"id\":%llu}",
                static_cast<unsigned long long>(id));
            if (jn <= 0 || static_cast<std::size_t>(jn) >= sizeof(json_buf)) {
                std::fprintf(stderr,
                             "lat_ex_order: json snprintf overflow at id=%llu\n",
                             static_cast<unsigned long long>(id));
                send_failed = true;
                break;
            }

            auto enc_r = encoder.encode(
                frame.data(), frame.size(),
                std::span<const uint8_t>{
                    reinterpret_cast<const uint8_t*>(json_buf),
                    static_cast<std::size_t>(jn)});
            if (!enc_r) {
                std::fprintf(stderr, "lat_ex_order: WsCodec::encode failed: %s\n",
                             enc_r.error().detail);
                send_failed = true;
                break;
            }

            // Stamp `t_send_ns` as close to the actual send as we can —
            // but BEFORE send() returns so a slow send() call counts
            // against that sample rather than the next one.
            slot.order_id  = id;
            slot.t_send_ns = bench::monotonic_raw_ns();
            slot.in_flight = true;
            ++inflight_count;
            ++next_id;

            auto send_r = stream->send(
                std::span<const uint8_t>{frame.data(), *enc_r});
            if (!send_r) {
                std::fprintf(stderr,
                             "lat_ex_order: send failed at id=%llu: %s\n",
                             static_cast<unsigned long long>(id),
                             send_r.error().detail);
                send_failed = true;
                break;
            }
        }
        if (send_failed) break;

        // Drain completions. poll() returns immediately (epoll_wait with
        // timeout 0) so this loop body is a tight busy-spin — appropriate
        // for a latency-sensitive bench but expensive on a shared core.
        poller->poll();

        // All planned orders issued AND drained → done.
        if (next_id > plan_total && inflight_count == 0) break;
    }

    if (malformed_count > 0 || stale_count > 0) {
        std::fprintf(stderr,
                     "lat_ex_order: skipped %llu malformed + %llu stale responses\n",
                     static_cast<unsigned long long>(malformed_count),
                     static_cast<unsigned long long>(stale_count));
    }
    std::fprintf(stderr,
                 "lat_ex_order: completed %llu orders (recorded %llu post-warmup)\n",
                 static_cast<unsigned long long>(sample_idx),
                 static_cast<unsigned long long>(recorded_count));

    const char* backend =
#if defined(EPH_USE_DPDK)
        "dpdk";
#else
        "kernel";
#endif
    bench::print_report("lat_ex_order", backend, rec, warmup_samples);

    (void)stream->close_gracefully();
    poller->poll(std::chrono::milliseconds{50});
    (void)poller->remove(stream.get());
    stream.reset();
    poller.reset();

    return send_failed ? 4 : 0;
}
#endif // EPH_USE_DPDK
