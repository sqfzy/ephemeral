/// @file framework/ws_mock_helper.hpp
/// In-process WebSocket mock startup helper for DPDK bench scenarios.
///
/// Provides start_ws_mock() which builds a MockServerConfig from the
/// BenchConfig and launches the mock on a dedicated thread pinned to
/// cfg.mock_cpu. Used by ws_echo / market_rx / order_rtt DPDK paths.

#pragma once

#include <thread>

#include "bench_config.hpp"
#include "signal.hpp"
#include "mock/mock_handle.hpp"
#include "mock/mock_ws_server.hpp"

namespace bench {

/// Mode flags for the WS mock server.
struct WsMockOptions {
    bool order_mode = false;
    bool echo_mode  = false;
};

/// Start the WS mock server on a dedicated thread, pinned to cfg.mock_cpu.
/// Returns a MockHandle for lifecycle management (call stop_mock() to join).
inline MockHandle start_ws_mock(const BenchConfig& cfg, WsMockOptions opts) {
    MockHandle h;
    bench::mock::MockServerConfig mock_cfg{
        .bind_ip = cfg.server_ip,
        .port = cfg.server_port,
        .symbols = cfg.symbols,
        .tick_interval = cfg.tick_interval,
        .order_mode = opts.order_mode,
        .echo_mode = opts.echo_mode,
    };
    int mock_cpu = cfg.mock_cpu;
    h.thread = std::thread([mock_cfg, mock_cpu, &running = *h.running] {
        bench::pin_or_die(mock_cpu, "mock-server");
        bench::mock::run_mock_ws_server(mock_cfg, running);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    return h;
}

} // namespace bench
