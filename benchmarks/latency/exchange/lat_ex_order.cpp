/// @file exchange/lat_ex_order.cpp
/// lat_ex_order / lat_ex_order_dpdk — N-inflight order RTT bench. Forks
/// the exchange WS mock and runs an order pipeline scenario in the parent.
///
/// The "payload" parameter to BenchRunner::run_rtt_inflight_sweep is
/// reinterpreted as the inflight target N (1 = synchronous baseline,
/// 4/16/64 = pipelined). Each in-flight order is matched back to its
/// originating client_send_tsc via a 128-slot id table.

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "eph/utils/cpu_pin.hpp"
#include "eph/utils/time.hpp"

#include "../core/config.hpp"
#include "../core/netns.hpp"
#include "../core/runner.hpp"
#include "../core/sample.hpp"
#include "../core/signal.hpp"
#include "../core/tsc_protocol.hpp"
#include "../core/ws_client.hpp"
#include "../core/ws_framing.hpp"

#include "mock_ws.hpp"

#if defined(EPH_USE_DPDK)
#include "../core/dpdk_env.hpp"
#include "eph/dpdk/tcp.hpp"
#endif

using namespace bench;

namespace {
// Order inflight sweep: 1 = synchronous baseline, 4/16/64 = pipelined.
constexpr std::array<int, 4> kDefaultInflights{1, 4, 16, 64};
constexpr size_t kMaxInflight = 128;
} // namespace

namespace bench::exchange::order_client {

#if !defined(EPH_USE_DPDK)
class OrderRttScenario {
public:
    explicit OrderRttScenario(int fd) : fd_(fd) {}

    bool prepare(size_t inflight) {
        if (inflight == 0 || inflight > kMaxInflight) return false;
        inflight_target_ = inflight;
        slots_.fill(0);
        next_id_   = 1;
        in_flight_ = 0;
        return true;
    }

    bool do_one_rtt(RttSample& out) {
        while (in_flight_ < inflight_target_) {
            if (!send_one()) return false;
        }
        return wait_for_one(out);
    }

    void cleanup() {}

private:
    bool send_one() {
        uint64_t id = next_id_++;
        uint64_t now_tsc = eph::utils::TSC::now();
        slots_[id % kMaxInflight] = now_tsc;

        char body[256];
        int n = std::snprintf(body, sizeof(body),
            R"({"id":%llu,"T_send":%llu,"sym":"BTCUSDT","px":"50000.00","qty":"0.001"})",
            static_cast<unsigned long long>(id),
            static_cast<unsigned long long>(now_tsc));
        if (n <= 0) return false;

        std::vector<uint8_t> frame(static_cast<size_t>(n) + 16);
        size_t flen = bench::ws_framing::build_masked_text_frame(
            frame.data(), body, static_cast<size_t>(n), mask_seed_++);
        if (!bench::ws_client::detail::send_all(fd_, frame.data(), flen)) return false;
        ++in_flight_;
        return true;
    }

    bool wait_for_one(RttSample& out) {
        std::array<uint8_t, 2048> buf{};
        for (;;) {
            size_t n = bench::ws_client::recv_one_frame(fd_, buf.data(), buf.size());
            if (n == 0) return false;

            // Match by echoed id — bookTicker pushes have id=0 and are ignored.
            uint64_t id = bench::tsc::detail::parse_uint64_after(
                std::string_view{reinterpret_cast<const char*>(buf.data()), n},
                "\"id\":");
            if (id == 0) continue;

            uint64_t slot = slots_[id % kMaxInflight];
            if (slot == 0) continue; // duplicate / stale
            slots_[id % kMaxInflight] = 0;

            out.client_send_tsc = slot;
            out.server_recv_tsc = bench::tsc::parse_T_recv(buf.data(), n);
            out.server_send_tsc = bench::tsc::parse_T(buf.data(), n);
            out.client_recv_tsc = eph::utils::TSC::now();
            --in_flight_;
            return true;
        }
    }

    int fd_;
    size_t inflight_target_ = 1;
    size_t in_flight_ = 0;
    uint64_t next_id_ = 1;
    uint32_t mask_seed_ = 0xCAFEF00D;
    std::array<uint64_t, kMaxInflight> slots_{};
};

int run(const BenchConfig& cfg, int /*argc*/, char** /*argv*/) {
    if (auto e = bench::enter_netns("bench_ns"); !e) {
        spdlog::error("lat_ex_order: enter_netns: {}", e.error());
        return 1;
    }
    eph::utils::CpuPinPolicy policy;
    if (cfg.allow_non_isolated) policy.require_isolcpus = false;
    if (auto p = eph::utils::pin_thread_strict(cfg.client_cpu, "bench_lat_ex_or", policy); !p) {
        spdlog::error("lat_ex_order: pin failed: {}", p.error());
        return 1;
    }

    int fd = -1;
    for (int i = 0; i < 50; ++i) {
        auto r = bench::ws_client::connect_ws(cfg.server_ip, cfg.server_port);
        if (r) { fd = *r; break; }
        ::usleep(20'000);
    }
    if (fd < 0) {
        spdlog::error("lat_ex_order: failed to connect/handshake after retries");
        return 1;
    }
    spdlog::info("lat_ex_order (kernel): connected to {}:{}",
                 cfg.server_ip, cfg.server_port);

    std::span<const int> inflights =
        cfg.inflights.empty()
            ? std::span<const int>{kDefaultInflights}
            : std::span<const int>{cfg.inflights};

    OrderRttScenario scenario{fd};
    BenchRunner runner{cfg, "exchange/order", "kernel"};
    runner.run_rtt_inflight_sweep(scenario, inflights);
    ::close(fd);
    return 0;
}

#else // EPH_USE_DPDK

class OrderRttDpdkScenario {
public:
    explicit OrderRttDpdkScenario(eph::dpdk::TcpSession<>& session)
        : session_(session) {}

    bool prepare(size_t inflight) {
        if (inflight == 0 || inflight > kMaxInflight) return false;
        inflight_target_ = inflight;
        slots_.fill(0);
        next_id_ = 1;
        in_flight_ = 0;
        rx_accum_.clear();
        return true;
    }

    bool do_one_rtt(RttSample& out) {
        while (in_flight_ < inflight_target_) {
            if (!send_one()) return false;
        }
        return wait_for_one(out);
    }

    void cleanup() {}

private:
    bool send_one() {
        uint64_t id = next_id_++;
        uint64_t now_tsc = eph::utils::TSC::now();
        slots_[id % kMaxInflight] = now_tsc;

        char body[256];
        int n = std::snprintf(body, sizeof(body),
            R"({"id":%llu,"T_send":%llu,"sym":"BTCUSDT","px":"50000.00","qty":"0.001"})",
            static_cast<unsigned long long>(id),
            static_cast<unsigned long long>(now_tsc));
        if (n <= 0) return false;

        frame_buf_.resize(static_cast<size_t>(n) + 16);
        size_t flen = ws_framing::build_masked_text_frame(
            frame_buf_.data(), body, static_cast<size_t>(n), mask_seed_++);

        size_t sent = 0;
        while (sent < flen) {
            size_t chunk = std::min(flen - sent, size_t{1460});
            auto r = session_.send(frame_buf_.data() + sent,
                                   static_cast<uint16_t>(chunk));
            if (!r) return false;
            sent += *r;
        }
        ++in_flight_;
        return true;
    }

    bool wait_for_one(RttSample& out) {
        uint64_t deadline = eph::utils::TSC::now() +
                            tsc::ns_to_cycles(1'000'000'000ULL);
        while (eph::utils::TSC::now() < deadline) {
            auto r = session_.poll_rx(
                [&](const uint8_t* data, uint16_t len) {
                    rx_accum_.insert(rx_accum_.end(), data, data + len);
                });
            if (!r) return false;

            while (rx_accum_.size() >= 2) {
                auto [hdr, plen] = ws_framing::parse_server_frame(
                    rx_accum_.data(), rx_accum_.size());
                if (plen == 0) break;
                const uint8_t* payload = rx_accum_.data() + hdr;

                uint64_t id = tsc::detail::parse_uint64_after(
                    std::string_view{reinterpret_cast<const char*>(payload), plen},
                    "\"id\":");

                if (id > 0 && slots_[id % kMaxInflight] != 0) {
                    out.client_send_tsc = slots_[id % kMaxInflight];
                    slots_[id % kMaxInflight] = 0;
                    out.server_recv_tsc = tsc::parse_T_recv(payload, plen);
                    out.server_send_tsc = tsc::parse_T(payload, plen);
                    out.client_recv_tsc = eph::utils::TSC::now();
                    --in_flight_;
                    rx_accum_.erase(rx_accum_.begin(),
                                    rx_accum_.begin() + static_cast<long>(hdr + plen));
                    return true;
                }
                rx_accum_.erase(rx_accum_.begin(),
                                rx_accum_.begin() + static_cast<long>(hdr + plen));
            }
        }
        return false;
    }

    eph::dpdk::TcpSession<>& session_;
    size_t inflight_target_ = 1;
    size_t in_flight_ = 0;
    uint64_t next_id_ = 1;
    uint32_t mask_seed_ = 0xCAFEF00D;
    std::array<uint64_t, kMaxInflight> slots_{};
    std::vector<uint8_t> frame_buf_;
    std::vector<uint8_t> rx_accum_;
};

int run(const BenchConfig& cfg, int argc, char** argv) {
    auto env = DpdkBenchEnv::create_full(argc, argv,
        cfg.server_ip, cfg.local_ip, cfg.gateway_ip, /*port_id=*/0);
    if (!env) { spdlog::error("lat_ex_order(dpdk): {}", env.error()); return 1; }

    eph::utils::CpuPinPolicy policy;
    if (cfg.allow_non_isolated) policy.require_isolcpus = false;
    if (auto p = eph::utils::pin_thread_strict(cfg.client_cpu, "bench_lat_ex_or", policy); !p) {
        spdlog::error("lat_ex_order: pin failed: {}", p.error());
        return 1;
    }

    static uint16_t src_port = 55900;
    auto tcfg = env->make_tcp_config(src_port++, cfg.server_port);
    eph::dpdk::TcpSession<> session{tcfg, env->pool};
    if (auto r = session.connect(std::chrono::seconds{3}); !r) {
        spdlog::error("lat_ex_order(dpdk): TCP connect: {}", r.error()); return 1;
    }
    std::string host = cfg.server_ip + ":" + std::to_string(cfg.server_port);
    if (auto h = bench::ws_client::dpdk_ws_handshake(session, host); !h) {
        spdlog::error("lat_ex_order(dpdk): WS handshake: {}", h.error()); return 1;
    }
    spdlog::info("lat_ex_order (dpdk): connected to {}:{}",
                 cfg.server_ip, cfg.server_port);

    std::span<const int> inflights =
        cfg.inflights.empty()
            ? std::span<const int>{kDefaultInflights}
            : std::span<const int>{cfg.inflights};

    OrderRttDpdkScenario scenario{session};
    BenchRunner runner{cfg, "exchange/order", "dpdk"};
    runner.run_rtt_inflight_sweep(scenario, inflights);
    (void)session.close();
    return 0;
}
#endif // EPH_USE_DPDK

} // namespace bench::exchange::order_client

int main(int argc, char** argv) {
    install_signal_handlers();
    if (!eph::utils::TSC::init()) {
        spdlog::error("TSC calibration failed");
        return 1;
    }

    auto cfg_r = load_bench_conf();
    if (!cfg_r) { spdlog::error("{}", cfg_r.error()); return 1; }
    const BenchConfig& cfg = *cfg_r;

    pid_t pid = ::fork();
    if (pid < 0) { spdlog::error("fork: {}", std::strerror(errno)); return 1; }
    if (pid == 0) {
        return bench::exchange::run_exchange_ws_mock(cfg);
    }

    int rc = bench::exchange::order_client::run(cfg, argc, argv);

    ::kill(pid, SIGTERM);
    int wstatus = 0;
    ::waitpid(pid, &wstatus, 0);
    return rc;
}
