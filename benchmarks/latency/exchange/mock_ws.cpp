/// @file exchange/mock_ws.cpp
/// mock_lat_exchange_ws / *_dpdk — exchange-style WebSocket mock.
///
/// Single connection at a time. Drives a delta-timer scheduler that pushes
/// 4 stream classes per symbol:
///
///   bookTicker  every  --bookticker-us  µs (default 1000 = 1kHz/symbol)
///   depth       every  --depth-ms       ms (default 10)
///   trade       Poisson mean --trade-mean-ms ms (default 5)
///   kline       every  --kline-s        s  (default 1)
///
/// The default rates are deliberately close to real Binance (which pushes
/// bookTicker at ~100-500 Hz per symbol) so that kernel-transport numbers
/// reflect production behaviour. Crank --bookticker-us down to 100 for a
/// 30kHz/total stress test that hits kernel TCP queueing limits.
///
/// In the same loop iteration the mock also non-blocking-recvs from the
/// client; if a complete frame arrives it parses an order JSON and writes
/// back an ExecutionReport echoing the order id, T_send, T_recv, T (server
/// send TSC). Per the plan, the same WS connection carries both market
/// data push and order/exec responses.
///
/// DPDK build is currently a POSIX stub with stderr warning, identical to
/// the stage-3 mocks.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <string_view>
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
#include "../core/socket_bind.hpp"
#include "../core/stream_scheduler.hpp"
#include "eph/utils/cpu.hpp"
#include "../core/ws_framing.hpp"
#include "../core/ws_handshake.hpp"

using namespace bench;

namespace {

// CLI extras for the exchange mock.
struct ExchangeMockOpts {
    std::vector<std::string> symbols = {"BTCUSDT", "ETHUSDT", "SOLUSDT"};
    long bookticker_us  = 1000;  ///< 1 kHz per symbol — close to Binance's real push rate

    long depth_ms       = 10;
    long trade_mean_ms  = 5;
    long kline_s        = 1;
    long depth_bytes    = 1024;
};

ExchangeMockOpts parse_extras(int argc, char** argv) {
    ExchangeMockOpts o;
    for (int i = 0; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--symbols" && i + 1 < argc) {
            o.symbols.clear();
            for (auto& s : config_detail::split(argv[++i], ',')) o.symbols.push_back(s);
        } else if (a == "--bookticker-us" && i + 1 < argc) {
            o.bookticker_us = std::atol(argv[++i]);
        } else if (a == "--depth-ms" && i + 1 < argc) {
            o.depth_ms = std::atol(argv[++i]);
        } else if (a == "--trade-mean-ms" && i + 1 < argc) {
            o.trade_mean_ms = std::atol(argv[++i]);
        } else if (a == "--kline-s" && i + 1 < argc) {
            o.kline_s = std::atol(argv[++i]);
        } else if (a == "--depth-bytes" && i + 1 < argc) {
            o.depth_bytes = std::atol(argv[++i]);
        } else if (a == "--trace" && i + 1 < argc) {
            // Reserved interface; not implemented in stage 4.
            ++i;
            spdlog::warn("--trace mode not implemented; ignoring");
        }
    }
    return o;
}

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
        "[mock_lat_exchange_ws_dpdk] WARNING: DPDK transport not yet "
        "implemented; this binary currently uses POSIX sockets identical "
        "to mock_lat_exchange_ws.\n");
#endif

    install_signal_handlers();
    auto cfg = parse_common(argc, argv);
    auto extras = parse_extras(argc, argv);

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
    if (auto p = eph::utils::pin_thread_strict(cfg.mock_cpu, "mock_ex_ws", policy); !p) {
        spdlog::error("pin_thread_strict failed: {}", p.error());
        return 1;
    }

    auto listen_fd = bench::tcp_bind_listen(cfg.server_ip, cfg.server_port);
    if (!listen_fd) { spdlog::error("{}", listen_fd.error()); return 1; }
    spdlog::info("mock_lat_exchange_ws: listening on {}:{} symbols={} "
                 "bookticker={}us depth={}ms trade_mean={}ms kline={}s "
                 "depth_bytes={} work_ns={}",
                 cfg.server_ip, cfg.server_port, extras.symbols.size(),
                 extras.bookticker_us, extras.depth_ms, extras.trade_mean_ms,
                 extras.kline_s, extras.depth_bytes, cfg.server_work_ns);

    constexpr size_t kFrameBufSize = 8192;
    std::vector<uint8_t> json_buf(kFrameBufSize);
    std::vector<uint8_t> tx_frame(kFrameBufSize + 16);
    constexpr size_t kRxBufCap = 65536;
    std::vector<uint8_t> rx(kRxBufCap);

    while (g_running.load(std::memory_order_acquire)) {
        auto cfd = bench::accept_one(*listen_fd, g_running);
        if (!cfd) { spdlog::error("{}", cfd.error()); break; }
        if (*cfd < 0) break;
        int client_fd = *cfd;

        if (auto h = bench::ws_server_handshake(client_fd); !h) {
            spdlog::error("ws handshake: {}", h.error());
            ::close(client_fd);
            continue;
        }
        spdlog::info("mock_lat_exchange_ws: handshake complete");

        // Build the per-stream scheduler. We reuse the json/tx_frame
        // buffers across all stream emit closures (single-threaded).
        // The closure captures `client_fd` by value and the shared buffers
        // by reference (the buffers outlive the scheduler).
        bench::StreamScheduler sched;
        uint64_t bookticker_count = 0;

        auto emit_book = [&](uint32_t sym_idx) -> bool {
            const auto& sym = extras.symbols[sym_idx];
            uint64_t ts = eph::utils::TSC::now();
            int n = std::snprintf(reinterpret_cast<char*>(json_buf.data()),
                json_buf.size(),
                R"({"e":"bookTicker","s":"%s","b":"50000.00","a":"50001.00","T":%llu})",
                sym.c_str(), static_cast<unsigned long long>(ts));
            if (n <= 0) return false;
            size_t flen = ws_framing::build_server_frame(tx_frame.data(), ws_framing::kOpText,
                                                   json_buf.data(),
                                                   static_cast<size_t>(n));
            if (!send_all_fd(client_fd, tx_frame.data(), flen)) return false;
            ++bookticker_count;
            return true;
        };
        auto emit_depth = [&](uint32_t sym_idx) -> bool {
            const auto& sym = extras.symbols[sym_idx];
            uint64_t ts = eph::utils::TSC::now();
            // Build a depth-snapshot-like JSON, padded to depth_bytes.
            int n = std::snprintf(reinterpret_cast<char*>(json_buf.data()),
                json_buf.size(),
                R"({"e":"depthUpdate","s":"%s","T":%llu,"bids":[["50000","%05ld"]],"pad":")",
                sym.c_str(), static_cast<unsigned long long>(ts),
                static_cast<long>(extras.depth_bytes));
            if (n <= 0) return false;
            size_t cur = static_cast<size_t>(n);
            size_t target = static_cast<size_t>(extras.depth_bytes);
            while (cur + 2 < target && cur < json_buf.size()) {
                json_buf[cur++] = 'x';
            }
            if (cur + 2 > json_buf.size()) cur = json_buf.size() - 2;
            json_buf[cur++] = '"';
            json_buf[cur++] = '}';
            size_t flen = ws_framing::build_server_frame(tx_frame.data(), ws_framing::kOpText,
                                                   json_buf.data(), cur);
            return send_all_fd(client_fd, tx_frame.data(), flen);
        };
        auto emit_trade = [&](uint32_t sym_idx) -> bool {
            const auto& sym = extras.symbols[sym_idx];
            uint64_t ts = eph::utils::TSC::now();
            int n = std::snprintf(reinterpret_cast<char*>(json_buf.data()),
                json_buf.size(),
                R"({"e":"trade","s":"%s","T":%llu,"p":"50000.50","q":"0.001"})",
                sym.c_str(), static_cast<unsigned long long>(ts));
            if (n <= 0) return false;
            size_t flen = ws_framing::build_server_frame(tx_frame.data(), ws_framing::kOpText,
                                                   json_buf.data(),
                                                   static_cast<size_t>(n));
            return send_all_fd(client_fd, tx_frame.data(), flen);
        };
        auto emit_kline = [&](uint32_t sym_idx) -> bool {
            const auto& sym = extras.symbols[sym_idx];
            uint64_t ts = eph::utils::TSC::now();
            int n = std::snprintf(reinterpret_cast<char*>(json_buf.data()),
                json_buf.size(),
                R"({"e":"kline","s":"%s","T":%llu,"k":{"o":"50000","h":"50100","l":"49900","c":"50050"}})",
                sym.c_str(), static_cast<unsigned long long>(ts));
            if (n <= 0) return false;
            size_t flen = ws_framing::build_server_frame(tx_frame.data(), ws_framing::kOpText,
                                                   json_buf.data(),
                                                   static_cast<size_t>(n));
            return send_all_fd(client_fd, tx_frame.data(), flen);
        };

        // Register one stream per (kind × symbol). Use a packed id =
        // kind * 1000 + sym_idx so the emit callback can recover the
        // symbol index.
        for (uint32_t i = 0; i < extras.symbols.size(); ++i) {
            sched.register_periodic(
                /*id=*/ 0 * 1000 + i,
                static_cast<uint64_t>(extras.bookticker_us) * 1000ULL,
                [&, i](uint32_t) { return emit_book(i); });
            sched.register_periodic(
                /*id=*/ 1 * 1000 + i,
                static_cast<uint64_t>(extras.depth_ms) * 1'000'000ULL,
                [&, i](uint32_t) { return emit_depth(i); });
            sched.register_poisson(
                /*id=*/ 2 * 1000 + i,
                static_cast<uint64_t>(extras.trade_mean_ms) * 1'000'000ULL,
                [&, i](uint32_t) { return emit_trade(i); });
            sched.register_periodic(
                /*id=*/ 3 * 1000 + i,
                static_cast<uint64_t>(extras.kline_s) * 1'000'000'000ULL,
                [&, i](uint32_t) { return emit_kline(i); });
        }
        sched.build();
        sched.rebase_now();

        size_t buffered = 0;
        bool ok = true;
        // Per-iteration: try to receive an order frame (non-blocking),
        // then fire any due stream tick.
        while (ok && g_running.load(std::memory_order_acquire)) {
            // ── 1. Non-blocking RX: drain whatever is on the socket.
            for (;;) {
                ssize_t n = ::recv(client_fd, rx.data() + buffered,
                                   rx.size() - buffered, MSG_DONTWAIT);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    ok = false;
                    break;
                }
                if (n == 0) { ok = false; break; }
                buffered += static_cast<size_t>(n);
                if (buffered >= rx.size()) {
                    spdlog::error("mock_lat_exchange_ws: rx buffer overflow");
                    ok = false;
                    break;
                }
            }
            if (!ok) break;

            // ── 2. Try to consume one full client frame.
            while (buffered >= 2) {
                auto frame = ws_framing::parse_client_frame_inplace(
                    rx.data(), buffered, rx.size());
                if (!frame.has_value()) break;

                uint64_t recv_tsc = eph::utils::TSC::now();
                const uint8_t* payload = rx.data() + (frame->total_consumed - frame->payload_len);

                // Parse id + T_send from the order JSON.
                std::string_view body{
                    reinterpret_cast<const char*>(payload), frame->payload_len};
                uint64_t id = bench::tsc::detail::parse_uint64_after(body, "\"id\":");
                uint64_t client_send = bench::tsc::parse_T_send(payload, frame->payload_len);

                eph::utils::spin_for_ns(cfg.server_work_ns);
                uint64_t send_tsc = eph::utils::TSC::now();

                // Build ExecutionReport response.
                int n = std::snprintf(reinterpret_cast<char*>(json_buf.data()),
                    json_buf.size(),
                    R"({"e":"executionReport","id":%llu,"T_send":%llu,"T_recv":%llu,"T":%llu,"X":"NEW"})",
                    static_cast<unsigned long long>(id),
                    static_cast<unsigned long long>(client_send),
                    static_cast<unsigned long long>(recv_tsc),
                    static_cast<unsigned long long>(send_tsc));
                if (n > 0) {
                    size_t flen = ws_framing::build_server_frame(tx_frame.data(),
                        ws_framing::kOpText, json_buf.data(),
                        static_cast<size_t>(n));
                    if (!send_all_fd(client_fd, tx_frame.data(), flen)) {
                        ok = false; break;
                    }
                }

                // Drop consumed bytes from rx buffer.
                size_t consumed = frame->total_consumed;
                if (consumed < buffered) {
                    std::memmove(rx.data(), rx.data() + consumed, buffered - consumed);
                }
                buffered -= consumed;
            }
            if (!ok) break;

            // ── 3. Fire one due stream tick (or no-op if none ready).
            (void)sched.tick();
        }

        ::close(client_fd);
        spdlog::info("mock_lat_exchange_ws: client disconnected (sent {} bookTicker frames)",
                     bookticker_count);
    }
    ::close(*listen_fd);
    return 0;
}
