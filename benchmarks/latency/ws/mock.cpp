/// @file ws/mock.cpp
/// mock_ws / mock_ws_dpdk — plain WebSocket echo server with TSC stamps.
///
/// Per request:
///   parse client frame → extract T_send → spin work_ns → reply with
///   {"T_send":..., "T_recv":..., "T":...} where T is server_send_tsc and
///   T_recv is server_recv_tsc.
///
/// Single connection at a time. The bench script restarts the mock for
/// each transport variant.
///
/// DPDK build note: same as tcp/udp mocks — POSIX path with stderr warning.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "eph/utils/time.hpp"

#include "../core/config.hpp"
#include "eph/utils/cpu_pin.hpp"
#include "../core/signal.hpp"
#include "../core/tsc_protocol.hpp"
#include "../mock/lib/busy_poll.hpp"
#include "../mock/lib/tcp_bind.hpp"
#include "eph/utils/cpu.hpp"
#include "../mock/lib/ws_frame.hpp"
#include "../mock/lib/ws_handshake.hpp"

using namespace bench;

namespace {

bool send_all_fd(int fd, const void* data, size_t len) noexcept {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, static_cast<const uint8_t*>(data) + sent,
                           len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
#ifdef EPH_USE_DPDK
    std::fprintf(stderr,
        "[mock_lat_ws_dpdk] WARNING: DPDK transport not yet implemented; "
        "this binary currently uses POSIX sockets identical to mock_lat_ws.\n");
#endif

    install_signal_handlers();
    auto cfg = parse_common(argc, argv);
    if (cfg.server_ip.empty() || cfg.server_port == 0) {
        spdlog::error("--server-ip and --port are required");
        return 1;
    }
    if (cfg.mock_cpu < 0) {
        spdlog::error("--mock-cpu <cpu> is required");
        return 1;
    }
    if (!eph::utils::TSC::init()) {
        spdlog::error("TSC calibration failed");
        return 1;
    }

    eph::utils::CpuPinPolicy policy;
    if (cfg.allow_non_isolated) policy.require_isolcpus = false;
    if (auto pinned = eph::utils::pin_thread_strict(cfg.mock_cpu, "mock_ws", policy); !pinned) {
        spdlog::error("pin_thread_strict failed: {}", pinned.error());
        return 1;
    }

    auto listen_fd = mock::tcp_bind_listen(cfg.server_ip, cfg.server_port);
    if (!listen_fd) { spdlog::error("{}", listen_fd.error()); return 1; }
    spdlog::info("mock_ws: listening on {}:{} work_ns={}",
                 cfg.server_ip, cfg.server_port, cfg.server_work_ns);

    constexpr size_t kRxBufCap = 65536;
    std::vector<uint8_t> rx(kRxBufCap);
    std::vector<uint8_t> tx_payload(1024);
    std::vector<uint8_t> tx_frame(1024 + 16);

    while (g_running.load(std::memory_order_acquire)) {
        auto cfd = mock::accept_one(*listen_fd, g_running);
        if (!cfd) { spdlog::error("{}", cfd.error()); break; }
        if (*cfd < 0) break;
        int client_fd = *cfd;

        if (auto h = mock::ws_server_handshake(client_fd); !h) {
            spdlog::error("ws handshake: {}", h.error());
            ::close(client_fd);
            continue;
        }
        spdlog::info("mock_ws: handshake complete");

        // Per-frame echo loop. Read header first, then the rest.
        size_t buffered = 0;
        bool ok = true;
        while (ok && g_running.load(std::memory_order_acquire)) {
            // Refill buffer until at least one full frame is parseable.
            while (buffered < 14) {
                pollfd p{}; p.fd = client_fd; p.events = POLLIN;
                int rv = ::poll(&p, 1, 100);
                if (rv < 0) { if (errno == EINTR) continue; ok = false; break; }
                if (rv == 0) {
                    if (!g_running.load(std::memory_order_acquire)) { ok = false; break; }
                    continue;
                }
                ssize_t n = ::recv(client_fd, rx.data() + buffered,
                                   rx.size() - buffered, 0);
                if (n <= 0) { ok = false; break; }
                buffered += static_cast<size_t>(n);
            }
            if (!ok) break;

            // Try to parse one frame.
            auto frame = mock::parse_client_frame_inplace(rx.data(), buffered, kRxBufCap);
            while (!frame.has_value() && buffered < kRxBufCap) {
                pollfd p{}; p.fd = client_fd; p.events = POLLIN;
                int rv = ::poll(&p, 1, 100);
                if (rv <= 0) { if (rv < 0 && errno == EINTR) continue; ok = false; break; }
                ssize_t n = ::recv(client_fd, rx.data() + buffered,
                                   rx.size() - buffered, 0);
                if (n <= 0) { ok = false; break; }
                buffered += static_cast<size_t>(n);
                frame = mock::parse_client_frame_inplace(rx.data(), buffered, kRxBufCap);
            }
            if (!ok || !frame.has_value()) { ok = false; break; }

            uint64_t recv_tsc = eph::utils::TSC::now();
            // Extract T_send (echoed back as proof of round-trip).
            const uint8_t* payload = rx.data() + (frame->total_consumed - frame->payload_len);
            uint64_t client_send_tsc = tsc::parse_T_send(payload, frame->payload_len);

            eph::utils::spin_for_ns(cfg.server_work_ns);
            uint64_t send_tsc = eph::utils::TSC::now();

            // Build response JSON.
            int n = std::snprintf(reinterpret_cast<char*>(tx_payload.data()),
                tx_payload.size(),
                R"({"T":%llu,"T_recv":%llu,"T_send":%llu})",
                static_cast<unsigned long long>(send_tsc),
                static_cast<unsigned long long>(recv_tsc),
                static_cast<unsigned long long>(client_send_tsc));
            if (n <= 0) { ok = false; break; }

            tx_frame.resize(static_cast<size_t>(n) + 16);
            size_t flen = mock::build_server_frame(tx_frame.data(), mock::kOpText,
                                                   tx_payload.data(),
                                                   static_cast<size_t>(n));
            if (!send_all_fd(client_fd, tx_frame.data(), flen)) { ok = false; break; }

            // Drop the consumed bytes from the buffer.
            size_t consumed = frame->total_consumed;
            std::memmove(rx.data(), rx.data() + consumed, buffered - consumed);
            buffered -= consumed;
        }
        ::close(client_fd);
        spdlog::info("mock_ws: client disconnected");
    }
    ::close(*listen_fd);
    return 0;
}
