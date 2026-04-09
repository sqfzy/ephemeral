/// @file exchange/lat_ex_market.cpp
/// lat_ex_market / lat_ex_market_dpdk — single-binary multi-symbol market
/// data RX bench. Forks the exchange WS mock, then in the parent runs a
/// 1-leg "wait for next bookTicker push" loop measured against the
/// server-stamped T (TSC).

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

namespace bench::exchange::market_client {

#if !defined(EPH_USE_DPDK)
// ── kernel scenario: read one WS frame, extract server T, yield sample ──
class MarketRxScenario {
public:
    explicit MarketRxScenario(int fd) : fd_(fd), buf_(2048) {}

    bool prepare() { return true; }

    bool do_one_recv(OneWaySample& out) {
        size_t n = bench::ws_client::recv_one_frame(fd_, buf_.data(), buf_.size());
        if (n == 0) return false;
        uint64_t t = bench::tsc::parse_T(buf_.data(), n);
        if (t == 0) return false;
        out.producer_tsc = t;
        out.consumer_tsc = eph::utils::TSC::now();
        return true;
    }

    void cleanup() {}

private:
    int fd_;
    std::vector<uint8_t> buf_;
};

int run(const BenchConfig& cfg, int /*argc*/, char** /*argv*/) {
    if (auto e = bench::enter_netns("bench_ns"); !e) {
        spdlog::error("lat_ex_market: enter_netns: {}", e.error());
        return 1;
    }
    eph::utils::CpuPinPolicy policy;
    if (cfg.allow_non_isolated) policy.require_isolcpus = false;
    if (auto p = eph::utils::pin_thread_strict(cfg.client_cpu, "bench_lat_ex_mk", policy); !p) {
        spdlog::error("lat_ex_market: pin failed: {}", p.error());
        return 1;
    }

    // Forked mock may not be ready instantly — retry the WS handshake.
    int fd = -1;
    for (int i = 0; i < 50; ++i) {
        auto r = bench::ws_client::connect_ws(cfg.server_ip, cfg.server_port);
        if (r) { fd = *r; break; }
        ::usleep(20'000);
    }
    if (fd < 0) {
        spdlog::error("lat_ex_market: failed to connect/handshake after retries");
        return 1;
    }
    spdlog::info("lat_ex_market (kernel): connected to {}:{}",
                 cfg.server_ip, cfg.server_port);

    MarketRxScenario scenario{fd};
    BenchRunner runner{cfg, "exchange/market", "kernel"};
    runner.run_oneway(scenario);
    ::close(fd);
    return 0;
}

#else // EPH_USE_DPDK

class MarketRxDpdkScenario {
public:
    explicit MarketRxDpdkScenario(eph::dpdk::TcpSession<>& session)
        : session_(session) {}

    bool prepare() { return true; }

    bool do_one_recv(OneWaySample& out) {
        rx_accum_.clear();
        uint64_t deadline = eph::utils::TSC::now() +
                            tsc::ns_to_cycles(1'000'000'000ULL); // 1 s
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
                uint64_t t = tsc::parse_T(payload, plen);
                rx_accum_.erase(rx_accum_.begin(),
                                rx_accum_.begin() + static_cast<long>(hdr + plen));
                if (t > 0) {
                    out.producer_tsc = t;
                    out.consumer_tsc = eph::utils::TSC::now();
                    return true;
                }
            }
        }
        return false;
    }

    void cleanup() {}

private:
    eph::dpdk::TcpSession<>& session_;
    std::vector<uint8_t> rx_accum_;
};

int run(const BenchConfig& cfg, int argc, char** argv) {
    auto env = DpdkBenchEnv::create_full(argc, argv,
        cfg.server_ip, cfg.local_ip, cfg.gateway_ip, /*port_id=*/0);
    if (!env) { spdlog::error("lat_ex_market(dpdk): {}", env.error()); return 1; }

    eph::utils::CpuPinPolicy policy;
    if (cfg.allow_non_isolated) policy.require_isolcpus = false;
    if (auto p = eph::utils::pin_thread_strict(cfg.client_cpu, "bench_lat_ex_mk", policy); !p) {
        spdlog::error("lat_ex_market: pin failed: {}", p.error());
        return 1;
    }

    static uint16_t src_port = 55800;
    auto tcfg = env->make_tcp_config(src_port++, cfg.server_port);
    eph::dpdk::TcpSession<> session{tcfg, env->pool};
    if (auto r = session.connect(std::chrono::seconds{3}); !r) {
        spdlog::error("lat_ex_market(dpdk): TCP connect: {}", r.error()); return 1;
    }
    std::string host = cfg.server_ip + ":" + std::to_string(cfg.server_port);
    if (auto h = bench::ws_client::dpdk_ws_handshake(session, host); !h) {
        spdlog::error("lat_ex_market(dpdk): WS handshake: {}", h.error()); return 1;
    }
    spdlog::info("lat_ex_market (dpdk): connected to {}:{}",
                 cfg.server_ip, cfg.server_port);

    MarketRxDpdkScenario scenario{session};
    BenchRunner runner{cfg, "exchange/market", "dpdk"};
    runner.run_oneway(scenario);
    (void)session.close();
    return 0;
}
#endif // EPH_USE_DPDK

} // namespace bench::exchange::market_client

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

    int rc = bench::exchange::market_client::run(cfg, argc, argv);

    ::kill(pid, SIGTERM);
    int wstatus = 0;
    ::waitpid(pid, &wstatus, 0);
    return rc;
}
