/// @file scenarios/ex_market_2p_push.hpp
/// Phase 3 port of ex_market_2p_push.py — the multi-symbol, burst-on-
/// busy variant used by `lat_ex_market_2p`.
///
/// Two differences from `ex_market_push`:
///   1. the fixture pool carries frames for N symbols (BTC/ETH/SOL/
///      BNB/XRP by default); PayloadPool rotates round-robin so the
///      client sees each symbol in turn
///   2. during the MMPP-2 busy regime, one sampled event fans out
///      into `burst_size` back-to-back frames sharing the same T
///      stamp — reflecting the real exchange pattern where a single
///      price event triggers a chain of depth updates
///
/// Config keys (`[lat_ex_market_2p]` section):
///   port, mockex_params, mockex_payload (required)
///   mockex_seed (optional), burst_size (default 10)
#pragma once

#include <cstdint>
#include <string>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "eph/net/posix_listener.hpp"
#include "mockex/io_stream.hpp"
#include "mockex/mmpp2.hpp"
#include "mockex/payload_pool.hpp"
#include "mockex/push_loop.hpp"
#include "mockex/scenario.hpp"
#include "mockex/tls_server.hpp"
#include "mockex/ws_server.hpp"

namespace mockex::scenarios {

[[nodiscard]] inline int
ex_market_2p_push_run(const ScenarioContext& ctx) noexcept {
    const std::string host = ctx.cfg->networking.server_ip.empty()
                                 ? std::string{"127.0.0.1"}
                                 : ctx.cfg->networking.server_ip;
    const auto params_path  = ctx.scenario->get_or<std::string>("mockex_params", "");
    const auto payload_path = ctx.scenario->get_or<std::string>("mockex_payload", "");
    auto port_e = ctx.scenario->get<uint16_t>("port");
    if (!port_e || params_path.empty() || payload_path.empty()) {
        SPDLOG_ERROR("[ex_market_2p_push] missing one of "
                     "port/mockex_params/mockex_payload in [scenarios.{}]",
                     ctx.scenario_name);
        return 1;
    }
    const uint16_t port = *port_e;

    auto params_e = mockex::Mmpp2Params::load_from_file(params_path);
    if (!params_e) {
        SPDLOG_ERROR("[ex_market_2p_push] load params '{}': {}",
                     params_path, params_e.error());
        return 1;
    }
    auto pool_e = mockex::PayloadPool::load_from_jsonl(payload_path);
    if (!pool_e) {
        SPDLOG_ERROR("[ex_market_2p_push] load payload '{}': {}",
                     payload_path, pool_e.error());
        return 1;
    }

    const uint64_t seed =
        ctx.scenario->get_or<uint64_t>("mockex_seed", params_e->seed_default);
    const size_t burst =
        ctx.scenario->get_or<uint32_t>("burst_size", 10);

    mockex::Mmpp2Sampler<> sampler(*params_e, seed);
    auto pool = std::move(*pool_e);

    auto lfd_e = eph::net::posix::tcp_bind_listen(host, port);
    if (!lfd_e) {
        SPDLOG_ERROR("[ex_market_2p_push] bind {}:{} failed: {}",
                     host, port, lfd_e.error());
        return 1;
    }
    const int lfd = *lfd_e;
    SPDLOG_INFO("[ex_market_2p_push] listening on {}:{} "
                "λ_q={} Hz λ_b={} Hz burst={} seed={}",
                host, port,
                params_e->lambda_quiet_hz, params_e->lambda_busy_hz,
                burst, seed);

    SPDLOG_INFO("[ex_market_2p_push] tls={}", ctx.tls != nullptr);
    while (ctx.running->load(std::memory_order_acquire)) {
        auto cfd_e = eph::net::posix::accept_one(lfd, *ctx.running);
        if (!cfd_e) {
            SPDLOG_WARN("[ex_market_2p_push] accept: {}", cfd_e.error());
            continue;
        }
        const int cfd = *cfd_e;
        if (cfd < 0) break;

        auto run_with_io = [&]<class IoStream>(IoStream io_local) {
            if (!ws::accept_handshake(io_local)) return;
            SPDLOG_INFO("[ex_market_2p_push] client connected fd={} "
                        "— push start", cfd);
            run_push_loop(io_local, sampler, pool, *ctx.running,
                          PushConfig{.scenario_log_tag = "[ex_market_2p_push]",
                                     .burst_multiplier = burst});
        };
        if (ctx.tls) {
            auto conn = ctx.tls->accept_tls(cfd);
            if (conn.fd() >= 0) run_with_io(std::move(conn));
        } else {
            run_with_io(io::RawFdStream{cfd});
        }
        SPDLOG_INFO("[ex_market_2p_push] client disconnected");
    }
    ::close(lfd);
    SPDLOG_INFO("[ex_market_2p_push] shutdown");
    return 0;
}

} // namespace mockex::scenarios
