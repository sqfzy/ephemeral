/// @file exchange/mock_ws.hpp
/// Exchange WebSocket mock — pushes 4 stream classes per symbol and
/// answers order frames with execution reports on the same connection.
///
/// Shared by `lat_ex_market.cpp` (consumer of bookTicker pushes) and
/// `lat_ex_order.cpp` (consumer of order/exec round trips). Both binaries
/// fork the mock at startup; this header keeps the mock loop in one place.
///
/// Per stream:
///   bookTicker  every  bookticker_us  µs (default 1000 = 1 kHz/symbol)
///   depth       every  depth_ms       ms (default 10)
///   trade       Poisson mean trade_mean_ms ms (default 5)
///   kline       every  kline_s        s  (default 1)
///
/// The defaults are deliberately close to real Binance push rates so
/// kernel-transport numbers reflect production behaviour. Lower
/// `bookticker_us` (e.g. 100) for a stress test that hits TCP queue limits.
#pragma once

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "eph/utils/cpu.hpp"
#include "eph/utils/cpu_pin.hpp"
#include "eph/utils/time.hpp"

#include "../core/config.hpp"
#include "../core/signal.hpp"
#include "../core/socket_bind.hpp"
#include "../core/stream_scheduler.hpp"
#include "../core/tsc_protocol.hpp"
#include "../core/ws_framing.hpp"
#include "../core/ws_handshake.hpp"

namespace bench::exchange {

namespace mock_ws_detail {

inline bool send_all_fd(int fd, const void* data, size_t len) noexcept {
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

} // namespace mock_ws_detail

/// Run the exchange WS mock loop. Blocks until `g_running` goes false.
/// Returns 0 on clean shutdown, non-zero on bind / pin / startup failure.
inline int run_exchange_ws_mock(const BenchConfig& cfg) {
    eph::utils::CpuPinPolicy policy;
    if (cfg.allow_non_isolated) policy.require_isolcpus = false;
    if (auto p = eph::utils::pin_thread_strict(cfg.mock_cpu, "mock_lat_ex_ws", policy); !p) {
        spdlog::error("mock_lat_ex_ws: pin failed: {}", p.error());
        return 1;
    }

    auto listen_fd = bench::tcp_bind_listen(cfg.server_ip, cfg.server_port);
    if (!listen_fd) {
        spdlog::error("mock_lat_ex_ws: {}", listen_fd.error());
        return 1;
    }
    spdlog::info("mock_lat_ex_ws: listening on {}:{} symbols={} "
                 "bookticker={}us depth={}ms trade_mean={}ms kline={}s "
                 "depth_bytes={} work_ns={}",
                 cfg.server_ip, cfg.server_port, cfg.symbols.size(),
                 cfg.bookticker_us, cfg.depth_ms, cfg.trade_mean_ms,
                 cfg.kline_s, cfg.depth_bytes, cfg.server_work_ns);

    constexpr size_t kFrameBufSize = 8192;
    std::vector<uint8_t> json_buf(kFrameBufSize);
    std::vector<uint8_t> tx_frame(kFrameBufSize + 16);
    constexpr size_t kRxBufCap = 65536;
    std::vector<uint8_t> rx(kRxBufCap);

    while (g_running.load(std::memory_order_acquire)) {
        auto cfd = bench::accept_one(*listen_fd, g_running);
        if (!cfd) { spdlog::error("mock_lat_ex_ws: {}", cfd.error()); break; }
        if (*cfd < 0) break;
        int client_fd = *cfd;

        if (auto h = bench::ws_server_handshake(client_fd); !h) {
            spdlog::error("mock_lat_ex_ws handshake: {}", h.error());
            ::close(client_fd);
            continue;
        }
        spdlog::info("mock_lat_ex_ws: handshake complete");

        bench::StreamScheduler sched;
        uint64_t bookticker_count = 0;

        // Stream emit closures share the json_buf / tx_frame buffers across
        // all symbols (single-threaded loop). They capture client_fd by
        // value and the buffers by reference.
        auto emit_book = [&](uint32_t sym_idx) -> bool {
            const auto& sym = cfg.symbols[sym_idx];
            uint64_t ts = eph::utils::TSC::now();
            int n = std::snprintf(reinterpret_cast<char*>(json_buf.data()),
                json_buf.size(),
                R"({"e":"bookTicker","s":"%s","b":"50000.00","a":"50001.00","T":%llu})",
                sym.c_str(), static_cast<unsigned long long>(ts));
            if (n <= 0) return false;
            size_t flen = ws_framing::build_server_frame(tx_frame.data(),
                ws_framing::kOpText, json_buf.data(), static_cast<size_t>(n));
            if (!mock_ws_detail::send_all_fd(client_fd, tx_frame.data(), flen)) return false;
            ++bookticker_count;
            return true;
        };
        auto emit_depth = [&](uint32_t sym_idx) -> bool {
            const auto& sym = cfg.symbols[sym_idx];
            uint64_t ts = eph::utils::TSC::now();
            int n = std::snprintf(reinterpret_cast<char*>(json_buf.data()),
                json_buf.size(),
                R"({"e":"depthUpdate","s":"%s","T":%llu,"bids":[["50000","%05ld"]],"pad":")",
                sym.c_str(), static_cast<unsigned long long>(ts),
                static_cast<long>(cfg.depth_bytes));
            if (n <= 0) return false;
            size_t cur = static_cast<size_t>(n);
            size_t target = cfg.depth_bytes;
            while (cur + 2 < target && cur < json_buf.size()) {
                json_buf[cur++] = 'x';
            }
            if (cur + 2 > json_buf.size()) cur = json_buf.size() - 2;
            json_buf[cur++] = '"';
            json_buf[cur++] = '}';
            size_t flen = ws_framing::build_server_frame(tx_frame.data(),
                ws_framing::kOpText, json_buf.data(), cur);
            return mock_ws_detail::send_all_fd(client_fd, tx_frame.data(), flen);
        };
        auto emit_trade = [&](uint32_t sym_idx) -> bool {
            const auto& sym = cfg.symbols[sym_idx];
            uint64_t ts = eph::utils::TSC::now();
            int n = std::snprintf(reinterpret_cast<char*>(json_buf.data()),
                json_buf.size(),
                R"({"e":"trade","s":"%s","T":%llu,"p":"50000.50","q":"0.001"})",
                sym.c_str(), static_cast<unsigned long long>(ts));
            if (n <= 0) return false;
            size_t flen = ws_framing::build_server_frame(tx_frame.data(),
                ws_framing::kOpText, json_buf.data(), static_cast<size_t>(n));
            return mock_ws_detail::send_all_fd(client_fd, tx_frame.data(), flen);
        };
        auto emit_kline = [&](uint32_t sym_idx) -> bool {
            const auto& sym = cfg.symbols[sym_idx];
            uint64_t ts = eph::utils::TSC::now();
            int n = std::snprintf(reinterpret_cast<char*>(json_buf.data()),
                json_buf.size(),
                R"({"e":"kline","s":"%s","T":%llu,"k":{"o":"50000","h":"50100","l":"49900","c":"50050"}})",
                sym.c_str(), static_cast<unsigned long long>(ts));
            if (n <= 0) return false;
            size_t flen = ws_framing::build_server_frame(tx_frame.data(),
                ws_framing::kOpText, json_buf.data(), static_cast<size_t>(n));
            return mock_ws_detail::send_all_fd(client_fd, tx_frame.data(), flen);
        };

        // One stream per (kind × symbol). Packed id = kind*1000 + sym_idx.
        for (uint32_t i = 0; i < cfg.symbols.size(); ++i) {
            sched.register_periodic(0 * 1000 + i,
                static_cast<uint64_t>(cfg.bookticker_us) * 1000ULL,
                [&, i](uint32_t) { return emit_book(i); });
            sched.register_periodic(1 * 1000 + i,
                static_cast<uint64_t>(cfg.depth_ms) * 1'000'000ULL,
                [&, i](uint32_t) { return emit_depth(i); });
            sched.register_poisson(2 * 1000 + i,
                static_cast<uint64_t>(cfg.trade_mean_ms) * 1'000'000ULL,
                [&, i](uint32_t) { return emit_trade(i); });
            sched.register_periodic(3 * 1000 + i,
                static_cast<uint64_t>(cfg.kline_s) * 1'000'000'000ULL,
                [&, i](uint32_t) { return emit_kline(i); });
        }
        sched.build();
        sched.rebase_now();

        size_t buffered = 0;
        bool ok = true;
        while (ok && g_running.load(std::memory_order_acquire)) {
            // Drain whatever is on the socket non-blocking.
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
                    spdlog::error("mock_lat_ex_ws: rx buffer overflow");
                    ok = false;
                    break;
                }
            }
            if (!ok) break;

            // Consume any complete client frames (orders).
            while (buffered >= 2) {
                auto frame = ws_framing::parse_client_frame_inplace(
                    rx.data(), buffered, rx.size());
                if (!frame.has_value()) break;

                uint64_t recv_tsc = eph::utils::TSC::now();
                const uint8_t* payload = rx.data() + (frame->total_consumed - frame->payload_len);

                std::string_view body{
                    reinterpret_cast<const char*>(payload), frame->payload_len};
                uint64_t id = bench::tsc::detail::parse_uint64_after(body, "\"id\":");
                uint64_t client_send = bench::tsc::parse_T_send(payload, frame->payload_len);

                eph::utils::spin_for_ns(cfg.server_work_ns);
                uint64_t send_tsc = eph::utils::TSC::now();

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
                    if (!mock_ws_detail::send_all_fd(client_fd, tx_frame.data(), flen)) {
                        ok = false; break;
                    }
                }

                size_t consumed = frame->total_consumed;
                if (consumed < buffered) {
                    std::memmove(rx.data(), rx.data() + consumed, buffered - consumed);
                }
                buffered -= consumed;
            }
            if (!ok) break;

            // Fire one due stream tick (no-op if none ready).
            (void)sched.tick();
        }

        ::close(client_fd);
        spdlog::info("mock_lat_ex_ws: client disconnected (sent {} bookTicker frames)",
                     bookticker_count);
    }
    ::close(*listen_fd);
    return 0;
}

} // namespace bench::exchange
