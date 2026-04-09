/// @file tcp/mock.cpp
/// mock_tcp / mock_tcp_dpdk — TCP echo server with TSC stamping + work_spin.
///
/// Per-round-trip server side:
///   1. recv_exact(N) into buffer
///   2. stamp server_recv at offset 8
///   3. work_spin(server_work_ns)
///   4. stamp server_send at offset 16
///   5. send_all(N)
///
/// N is determined by the *first* recv: peek the first 4 bytes (which the
/// client sets to a placeholder TSC fragment) — actually we keep N
/// configurable via `--msg-size N` so the mock and bench agree out of band.
/// This is the same approach the old code took: the bench script restarts
/// the mock per payload sweep step (and the new bench_latency.sh in stage 5
/// is responsible for that).
///
/// DPDK build note: this mock currently uses POSIX sockets in both kernel
/// and EPH_USE_DPDK builds. A real DPDK fast-path mock requires EAL init,
/// Platform setup, manual rte_eth_rx_burst handling — substantial work
/// scoped out of stage 3. The mock_tcp_dpdk binary still exists (so the
/// build matrix is complete) but emits a startup warning to stderr.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

#include <unistd.h>

#include <spdlog/spdlog.h>

#include "eph/utils/time.hpp"

#include "../core/config.hpp"
#include "eph/utils/cpu_pin.hpp"
#include "../core/signal.hpp"
#include "../core/socket_bind.hpp"
#include "eph/utils/cpu.hpp"

using namespace bench;

namespace {
size_t parse_msg_size(int argc, char** argv, size_t fallback) {
    for (int i = 0; i < argc - 1; ++i) {
        if (std::string_view{argv[i]} == "--msg-size") {
            return static_cast<size_t>(std::atol(argv[i + 1]));
        }
    }
    return fallback;
}
} // namespace

int main(int argc, char** argv) {
#ifdef EPH_USE_DPDK
    std::fprintf(stderr,
        "[mock_lat_tcp_dpdk] WARNING: DPDK transport not yet implemented; "
        "this binary currently uses POSIX sockets identical to mock_lat_tcp. "
        "See benchmarks/latency/tcp/mock.cpp header for details.\n");
#endif

    install_signal_handlers();
    auto cfg = parse_common(argc, argv);

    if (cfg.server_ip.empty()) {
        spdlog::error("--server-ip <bind-ip> is required");
        return 1;
    }
    if (cfg.server_port == 0) {
        spdlog::error("--port <bind-port> is required");
        return 1;
    }
    if (cfg.mock_cpu < 0) {
        spdlog::error("--mock-cpu <cpu> is required");
        return 1;
    }

    size_t msg_size = parse_msg_size(argc, argv, /*fallback*/ 64);
    if (msg_size < 24) {
        spdlog::error("--msg-size must be >= 24 (binary header size)");
        return 1;
    }

    if (!eph::utils::TSC::init()) {
        spdlog::error("TSC calibration failed");
        return 1;
    }

    eph::utils::CpuPinPolicy policy;
    if (cfg.allow_non_isolated) policy.require_isolcpus = false;
    if (auto pinned = eph::utils::pin_thread_strict(cfg.mock_cpu, "mock_tcp", policy); !pinned) {
        spdlog::error("pin_thread_strict failed: {}", pinned.error());
        return 1;
    }

    auto listen_fd = bench::tcp_bind_listen(cfg.server_ip, cfg.server_port);
    if (!listen_fd) {
        spdlog::error("{}", listen_fd.error());
        return 1;
    }
    spdlog::info("mock_tcp: listening on {}:{} msg_size={} work_ns={}",
                 cfg.server_ip, cfg.server_port, msg_size, cfg.server_work_ns);

    std::vector<uint8_t> buf(msg_size);

    while (g_running.load(std::memory_order_acquire)) {
        auto cfd = bench::accept_one(*listen_fd, g_running);
        if (!cfd) { spdlog::error("{}", cfd.error()); break; }
        if (*cfd < 0) break; // shutdown
        int client_fd = *cfd;
        spdlog::info("mock_tcp: client connected fd={}", client_fd);

        // Per-round echo loop.
        while (g_running.load(std::memory_order_acquire)) {
            // Read N bytes (blocking — TCP is byte-stream).
            size_t got = 0;
            bool ok = true;
            while (got < msg_size) {
                ssize_t n = ::recv(client_fd, buf.data() + got, msg_size - got, 0);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    ok = false; break;
                }
                if (n == 0) { ok = false; break; }
                got += static_cast<size_t>(n);
            }
            if (!ok) break;

            // Stamp server_recv, spin business work, stamp server_send.
            uint64_t recv_tsc = eph::utils::TSC::now();
            std::memcpy(buf.data() + 8, &recv_tsc, 8);
            eph::utils::spin_for_ns(cfg.server_work_ns);
            uint64_t send_tsc = eph::utils::TSC::now();
            std::memcpy(buf.data() + 16, &send_tsc, 8);

            // Send N back.
            size_t sent = 0;
            while (sent < msg_size) {
                ssize_t n = ::send(client_fd, buf.data() + sent,
                                   msg_size - sent, MSG_NOSIGNAL);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    ok = false; break;
                }
                sent += static_cast<size_t>(n);
            }
            if (!ok) break;
        }
        ::close(client_fd);
        spdlog::info("mock_tcp: client disconnected");
    }
    ::close(*listen_fd);
    return 0;
}
