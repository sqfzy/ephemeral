/// @file mock_ws_server.hpp
/// Mock WebSocket server for benchmarks.
///
/// Runs in-process on a dedicated thread, using kernel TCP (POSIX sockets)
/// on NIC-A. Supports two modes:
///   - Market mode: pushes bookTicker JSON at configurable tick intervals
///   - Order mode: also responds to client orders with ExecutionReport
///
/// Designed for 1:1 benchmarking — accepts exactly ONE client connection.

#pragma once

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "eph/utils/time.hpp"
#include "mock_data_gen.hpp"
#include "mock_ws_handshake.hpp"

namespace bench::mock {

// ── Configuration ────────────────────────────────────────────────────────────

struct MockServerConfig {
    std::string bind_ip = "10.0.0.1";
    uint16_t port = 9999;
    std::vector<std::string> symbols = {"BTCUSDT", "ETHUSDT", "SOLUSDT"};
    std::chrono::microseconds tick_interval{100}; // market data push frequency
    bool order_mode = false; // true = wait for orders and respond with ExecutionReport
};

// ── WebSocket frame helpers (server-side) ────────────────────────────────────

namespace detail {

/// Build an unmasked WebSocket frame (server -> client, RFC 6455 section 5.1).
/// Server frames MUST NOT be masked.
///
/// @param out      Output buffer (must be large enough for header + payload).
/// @param opcode   WebSocket opcode (0x01 = text, 0x02 = binary, 0x08 = close).
/// @param payload  Pointer to payload data.
/// @param len      Payload length in bytes.
/// @return Total frame size (header + payload) written to out.
inline size_t build_server_ws_frame(uint8_t* out, uint8_t opcode,
                                    const uint8_t* payload, size_t len) {
    out[0] = 0x80 | opcode; // FIN=1 + opcode
    size_t hdr = 2;
    if (len < 126) {
        out[1] = static_cast<uint8_t>(len);
    } else if (len < 65536) {
        out[1] = 126;
        out[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
        out[3] = static_cast<uint8_t>(len & 0xFF);
        hdr = 4;
    } else {
        // 64-bit extended length (unlikely for bench payloads, but correct)
        out[1] = 127;
        for (int i = 0; i < 8; ++i) {
            out[2 + i] = static_cast<uint8_t>((len >> (56 - 8 * i)) & 0xFF);
        }
        hdr = 10;
    }
    std::memcpy(out + hdr, payload, len);
    return hdr + len;
}

/// Read exactly n bytes from fd with poll timeout.
/// Returns true if all bytes read, false on error/timeout/disconnect.
inline bool read_exact(int fd, uint8_t* buf, size_t n, int timeout_ms) {
    size_t total = 0;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (total < n) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) return false;

        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        int ret = ::poll(&pfd, 1, static_cast<int>(remaining.count()));
        if (ret <= 0) return false;

        ssize_t r = ::recv(fd, buf + total, n - total, 0);
        if (r <= 0) return false;
        total += static_cast<size_t>(r);
    }
    return true;
}

/// Minimal client WebSocket frame parser (for order mode).
/// Client frames are masked (RFC 6455 section 5.3).
///
/// @param fd          Client socket fd.
/// @param payload     Output buffer for unmasked payload.
/// @param max_len     Maximum payload size.
/// @param timeout_ms  Read timeout in milliseconds.
/// @return Payload length on success, 0 on error/timeout/close frame.
inline size_t read_client_ws_frame(int fd, uint8_t* payload, size_t max_len,
                                   int timeout_ms) {
    // Read 2-byte header
    uint8_t hdr[2];
    if (!read_exact(fd, hdr, 2, timeout_ms)) return 0;

    uint8_t opcode = hdr[0] & 0x0F;
    bool masked = (hdr[1] & 0x80) != 0;
    uint64_t payload_len = hdr[1] & 0x7F;

    // Close frame
    if (opcode == 0x08) {
        spdlog::debug("mock_ws_server: received close frame from client");
        return 0;
    }

    // Extended length
    if (payload_len == 126) {
        uint8_t ext[2];
        if (!read_exact(fd, ext, 2, timeout_ms)) return 0;
        payload_len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (payload_len == 127) {
        uint8_t ext[8];
        if (!read_exact(fd, ext, 8, timeout_ms)) return 0;
        payload_len = 0;
        for (int i = 0; i < 8; ++i) {
            payload_len = (payload_len << 8) | ext[i];
        }
    }

    if (payload_len > max_len) {
        spdlog::error("mock_ws_server: client frame too large ({} > {})",
                      payload_len, max_len);
        return 0;
    }

    // Read mask key (4 bytes) if masked
    uint8_t mask_key[4] = {};
    if (masked) {
        if (!read_exact(fd, mask_key, 4, timeout_ms)) return 0;
    }

    // Read payload
    auto plen = static_cast<size_t>(payload_len);
    if (plen > 0) {
        if (!read_exact(fd, payload, plen, timeout_ms)) return 0;
    }

    // Unmask payload
    if (masked) {
        for (size_t i = 0; i < plen; ++i) {
            payload[i] ^= mask_key[i & 3];
        }
    }

    return plen;
}

/// Extract a JSON string field value by key from a flat JSON object.
/// Searches for "key":"value" and returns the value (without quotes).
/// Returns empty string_view if not found. Does NOT handle nested objects.
inline std::string_view json_extract_string(std::string_view json,
                                            std::string_view key) {
    // Build pattern: "key":"
    char pattern[128];
    int plen = std::snprintf(pattern, sizeof(pattern),
                             "\"%.*s\":\"",
                             static_cast<int>(key.size()), key.data());
    if (plen <= 0) return {};

    auto pos = json.find(std::string_view(pattern, static_cast<size_t>(plen)));
    if (pos == std::string_view::npos) return {};

    auto val_start = pos + static_cast<size_t>(plen);
    auto val_end = json.find('"', val_start);
    if (val_end == std::string_view::npos) return {};

    return json.substr(val_start, val_end - val_start);
}

/// Send all bytes from buf over fd. Returns true on success.
inline bool send_all(int fd, const uint8_t* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            spdlog::error("mock_ws_server: send() failed: {}", std::strerror(errno));
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

} // namespace detail

// ── Main server function ─────────────────────────────────────────────────────

/// Run a mock WebSocket server. Blocks until running becomes false.
///
/// Market mode (order_mode=false):
///   - Accept ONE WS connection
///   - Push bookTicker JSON for each symbol at tick_interval
///   - Each message "T" field = TSC nanosecond timestamp (stamped just before send)
///
/// Order mode (order_mode=true):
///   - Accept ONE WS connection
///   - Also push market data at tick_interval
///   - When a client message is received, parse and respond with ExecutionReport
///   - ExecutionReport "T" field = TSC timestamp of response generation
///
/// The server uses POSIX sockets (kernel TCP). It binds to bind_ip:port.
/// It handles ONE client connection at a time (benchmark is 1:1).
///
/// @param config   Server configuration (bind address, symbols, tick interval, mode).
/// @param running  Atomic flag — set to false to gracefully stop the server.
inline void run_mock_ws_server(const MockServerConfig& config,
                               std::atomic<bool>& running) {
    spdlog::info("mock_ws_server: starting on {}:{} (order_mode={}, symbols={}, "
                 "tick_interval={}us)",
                 config.bind_ip, config.port, config.order_mode,
                 config.symbols.size(),
                 config.tick_interval.count());

    // ── 1. Create TCP server socket ──────────────────────────────────────
    int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        spdlog::error("mock_ws_server: socket() failed: {}", std::strerror(errno));
        return;
    }

    // SO_REUSEADDR to avoid TIME_WAIT issues between benchmark runs
    int opt = 1;
    ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // TCP_NODELAY for low-latency sends
    ::setsockopt(server_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config.port);
    if (::inet_pton(AF_INET, config.bind_ip.c_str(), &addr.sin_addr) != 1) {
        spdlog::error("mock_ws_server: invalid bind IP \"{}\"", config.bind_ip);
        ::close(server_fd);
        return;
    }

    if (::bind(server_fd, reinterpret_cast<struct sockaddr*>(&addr),
               sizeof(addr)) < 0) {
        spdlog::error("mock_ws_server: bind() failed on {}:{}: {}",
                      config.bind_ip, config.port, std::strerror(errno));
        ::close(server_fd);
        return;
    }

    if (::listen(server_fd, 1) < 0) {
        spdlog::error("mock_ws_server: listen() failed: {}", std::strerror(errno));
        ::close(server_fd);
        return;
    }

    spdlog::info("mock_ws_server: listening on {}:{}", config.bind_ip, config.port);

    // ── 2. Accept ONE client (with poll for graceful shutdown) ───────────
    int client_fd = -1;
    while (running.load(std::memory_order_acquire) && client_fd < 0) {
        struct pollfd pfd{};
        pfd.fd = server_fd;
        pfd.events = POLLIN;

        int ret = ::poll(&pfd, 1, 100); // 100ms timeout to check running flag
        if (ret < 0) {
            if (errno == EINTR) continue;
            spdlog::error("mock_ws_server: poll(accept) failed: {}",
                          std::strerror(errno));
            ::close(server_fd);
            return;
        }
        if (ret == 0) continue; // timeout, check running flag

        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        client_fd = ::accept(server_fd,
                             reinterpret_cast<struct sockaddr*>(&client_addr),
                             &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) { client_fd = -1; continue; }
            spdlog::error("mock_ws_server: accept() failed: {}",
                          std::strerror(errno));
            ::close(server_fd);
            return;
        }

        // TCP_NODELAY on client socket too
        ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        char client_ip[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        spdlog::info("mock_ws_server: accepted connection from {}:{}",
                     client_ip, ntohs(client_addr.sin_port));
    }

    if (client_fd < 0) {
        spdlog::info("mock_ws_server: shutting down (no client connected)");
        ::close(server_fd);
        return;
    }

    // ── 3. WebSocket handshake ───────────────────────────────────────────
    if (!handle_ws_upgrade(client_fd)) {
        spdlog::error("mock_ws_server: WebSocket handshake failed");
        ::close(client_fd);
        ::close(server_fd);
        return;
    }
    spdlog::info("mock_ws_server: WebSocket handshake complete");

    // ── 4. Main loop: push market data + (optionally) handle orders ─────
    // Buffers for JSON generation and WS frame encoding
    constexpr size_t kJsonBufSize = 512;
    constexpr size_t kFrameBufSize = 1024; // header + payload
    constexpr size_t kClientBufSize = 2048;

    char json_buf[kJsonBufSize];
    uint8_t frame_buf[kFrameBufSize];
    uint8_t client_payload[kClientBufSize];

    auto last_tick = std::chrono::steady_clock::now();
    uint64_t tick_count = 0;

    while (running.load(std::memory_order_acquire)) {
        auto now = std::chrono::steady_clock::now();

        // ── 4a. Check for client data (order mode or just drain) ─────
        struct pollfd pfd{};
        pfd.fd = client_fd;
        pfd.events = POLLIN;

        // Short poll: 0ms if we need to send a tick soon, otherwise up to 1ms
        auto until_tick = std::chrono::duration_cast<std::chrono::microseconds>(
            (last_tick + config.tick_interval) - now);
        int poll_ms = (until_tick.count() <= 0) ? 0 : 1;

        int ret = ::poll(&pfd, 1, poll_ms);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            size_t plen = detail::read_client_ws_frame(
                client_fd, client_payload, kClientBufSize, 50);

            if (plen == 0 && (pfd.revents & (POLLHUP | POLLERR))) {
                spdlog::info("mock_ws_server: client disconnected");
                break;
            }

            if (plen > 0 && config.order_mode) {
                // Parse the order request and respond with ExecutionReport
                std::string_view order_json(
                    reinterpret_cast<const char*>(client_payload), plen);
                spdlog::debug("mock_ws_server: received order: {}", order_json);

                auto symbol = detail::json_extract_string(order_json, "symbol");
                auto side = detail::json_extract_string(order_json, "side");

                if (symbol.empty()) symbol = "BTCUSDT";
                if (side.empty()) side = "BUY";

                // Stamp raw TSC cycles at response generation time
                // (bench computes diff in cycles, converts to ns)
                uint64_t ts = eph::utils::TSC::now();

                size_t json_len = generate_execution_report(
                    json_buf, kJsonBufSize, symbol, side, ts);

                if (json_len > 0) {
                    size_t frame_len = detail::build_server_ws_frame(
                        frame_buf, 0x01,
                        reinterpret_cast<const uint8_t*>(json_buf), json_len);

                    if (!detail::send_all(client_fd, frame_buf, frame_len)) {
                        spdlog::error("mock_ws_server: failed to send ExecutionReport");
                        break;
                    }
                    spdlog::debug("mock_ws_server: sent ExecutionReport ({} bytes)",
                                  json_len);
                }
            }
        } else if (ret > 0 && (pfd.revents & (POLLHUP | POLLERR))) {
            spdlog::info("mock_ws_server: client connection closed");
            break;
        }

        // ── 4b. Push market data if tick interval elapsed ────────────
        now = std::chrono::steady_clock::now();
        if (now - last_tick >= config.tick_interval) {
            last_tick = now;
            ++tick_count;

            for (const auto& symbol : config.symbols) {
                // Stamp raw TSC cycles just before building the frame.
                // Bench client computes: TSC::to_ns(TSC::now() - T_cycles).
                uint64_t ts = eph::utils::TSC::now();

                size_t json_len = generate_book_ticker(
                    json_buf, kJsonBufSize, symbol, ts);

                if (json_len == 0) {
                    spdlog::error("mock_ws_server: failed to generate bookTicker "
                                  "for {}", symbol);
                    continue;
                }

                size_t frame_len = detail::build_server_ws_frame(
                    frame_buf, 0x01,
                    reinterpret_cast<const uint8_t*>(json_buf), json_len);

                if (!detail::send_all(client_fd, frame_buf, frame_len)) {
                    spdlog::error("mock_ws_server: failed to send bookTicker for {}",
                                  symbol);
                    goto cleanup; // double-break out of both loops
                }
            }

            if (tick_count % 10000 == 0) {
                spdlog::debug("mock_ws_server: sent {} ticks ({} symbols each)",
                              tick_count, config.symbols.size());
            }
        }
    }

cleanup:
    spdlog::info("mock_ws_server: shutting down (sent {} ticks total)", tick_count);

    // ── 5. Cleanup ───────────────────────────────────────────────────────
    ::close(client_fd);
    ::close(server_fd);

    spdlog::info("mock_ws_server: stopped");
}

} // namespace bench::mock
