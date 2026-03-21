#pragma once

/// @file transport.hpp
/// Generic WebSocket transport over any TcpTransport backend.
///
/// Provides a single entry point for establishing a WSS connection
/// and sending/receiving data with minimal latency.
///
/// Architecture:
///   Control thread (handshake):
///     TCP connect -> TLS 1.3 handshake -> WebSocket Upgrade
///   Data plane (hot path):
///     app send() -> SPSC queue -> TX thread -> WS frame -> TLS encrypt ->
///     TCP send
///
/// Thread model:
///   - Application thread: calls send()/recv(), non-blocking
///   - TX thread: busy-poll SPSC queue, build packets, send
///   - RX thread: busy-poll TCP rx, decrypt, parse WS frames, push to recv queue

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <thread>

#include <cerrno>
#include <sched.h>
#include <system_error>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "eph/base/cache.hpp"
#include "eph/containers/bounded_queue.hpp"
#include "eph/utils/cpu.hpp"
#include "eph/net/http.hpp"
#include "eph/net/tcp_concept.hpp"
#include "eph/net/tls_record.hpp"
#include "eph/net/tls_session.hpp"
#include "eph/net/websocket.hpp"

namespace eph::net {

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct TransportConfig {
    // Connection target
    std::string remote_host{};      // Hostname for TLS SNI and HTTP Host
    uint16_t    remote_port = 443;  // Remote TCP port
    std::string ws_path     = "/";  // WebSocket upgrade path
    std::string extra_headers{};    // Additional HTTP headers for upgrade

    // TLS
    std::string ca_cert_path{};     // CA cert file, empty = system default
    bool        verify_peer = true;

    // Timeouts
    std::chrono::milliseconds tcp_timeout{3000};
    std::chrono::milliseconds tls_timeout{5000};
    std::chrono::milliseconds ws_timeout{3000};

    // Performance
    uint16_t tx_burst_size = 32;    // Max messages per TX drain batch
    uint16_t rx_burst_size = 32;    // Max packets per RX poll

    // Reconnection (fixed-interval, discard old messages during reconnect)
    std::chrono::milliseconds reconnect_interval{100}; // Interval between retries
    int max_reconnect_attempts = 10;                    // 0 = disable auto-reconnect

    // WebSocket ping (sent by TX thread at configured interval)
    std::chrono::seconds ping_interval{30};  // 0 = disable ping

    // CPU affinity for worker threads (-1 = no pinning)
    int tx_cpu = -1;
    int rx_cpu = -1;
};

// ---------------------------------------------------------------------------
// Transport stats
// ---------------------------------------------------------------------------

/// Per-thread stats -- TX thread and RX thread each own their own counters.
/// Merged at query time to avoid atomic contention on the hot path.
struct ThreadStats {
    uint64_t packets       = 0;
    uint64_t bytes         = 0;
    uint64_t dropped       = 0;
    uint64_t crypto_errors = 0;
};

/// Aggregated transport statistics (returned by stats()).
struct TransportStats {
    uint64_t tx_packets        = 0;
    uint64_t tx_bytes          = 0;
    uint64_t tx_dropped        = 0;
    uint64_t rx_packets        = 0;
    uint64_t rx_bytes          = 0;
    uint64_t encrypt_errors    = 0;
    uint64_t decrypt_errors    = 0;
    uint64_t queue_full_count  = 0;
    uint64_t ws_pings_received = 0;
    uint64_t ws_pongs_sent     = 0;
    uint64_t reconnect_count   = 0;
};

// ---------------------------------------------------------------------------
// Internal message types for SPSC queue
// ---------------------------------------------------------------------------

namespace detail {

/// Message passed from application thread to TX thread via SPSC queue.
/// Fixed-size to satisfy TrivialData constraint.
template <size_t MaxPayload>
struct alignas(eph::base::CACHE_LINE_SIZE) TxMessage {
    uint8_t  data[MaxPayload]{};
    uint16_t len = 0;
    uint8_t  opcode = ws::opcode::kBinary;

    static_assert(MaxPayload > 0, "MaxPayload must be > 0");
    static_assert(MaxPayload <= tls_const::kMaxRecordPayload,
                  "MaxPayload exceeds TLS max record size");
};

/// Message passed from RX processing to application via SPSC queue.
template <size_t MaxPayload>
struct alignas(eph::base::CACHE_LINE_SIZE) RxMessage {
    uint8_t  data[MaxPayload]{};
    uint16_t len = 0;
    uint8_t  opcode = ws::opcode::kBinary;
};

inline std::shared_ptr<spdlog::logger> transport_logger() {
    static auto l = [] {
        auto lg = spdlog::stdout_color_mt("net.transport");
        lg->set_level(spdlog::level::trace);
        return lg;
    }();
    return l;
}

} // namespace detail

// ---------------------------------------------------------------------------
// Transport -- public API
// ---------------------------------------------------------------------------

/// Generic WebSocket transport with TLS 1.3 encryption.
///
/// Template parameters:
///   TcpImpl    -- a type satisfying the TcpTransport concept
///   MaxPayload -- maximum application payload size per message
///   QueueDepth -- SPSC queue capacity (must be power of 2)
///
/// Usage:
///   auto factory = [&]() -> std::expected<std::unique_ptr<MyTcp>, std::string> {
///       auto tcp = std::make_unique<MyTcp>(my_config);
///       auto r = tcp->connect(std::chrono::milliseconds{3000});
///       if (!r) return std::unexpected(r.error());
///       return tcp;
///   };
///   auto result = Transport<MyTcp>::create(std::move(factory), config);
///   if (!result) { /* handle error */ }
///   auto& transport = *result;     // unique_ptr<Transport>
///   transport->send(data, len);    // Non-blocking
///   transport->recv([](auto* data, auto len) { ... });
template <TcpTransport TcpImpl, size_t MaxPayload = 512, size_t QueueDepth = 1024>
class Transport {
    static_assert(TcpTransport<TcpImpl>,
                  "TcpImpl must satisfy TcpTransport concept");
    static_assert(MaxPayload > 0, "MaxPayload must be > 0");
    static_assert(MaxPayload <= tls_const::kMaxRecordPayload,
                  "MaxPayload exceeds TLS max record size (16384)");
    static_assert(std::has_single_bit(QueueDepth),
                  "QueueDepth must be power of 2");

    using TxMsg = detail::TxMessage<MaxPayload>;
    using RxMsg = detail::RxMessage<MaxPayload>;
    using TxQueue = eph::containers::BoundedQueue<TxMsg, QueueDepth>;
    using RxQueue = eph::containers::BoundedQueue<RxMsg, QueueDepth>;

public:
    /// Factory callable: creates a new, already-connected TcpImpl instance.
    /// Called during initial connect and on each reconnection attempt.
    using TcpFactory = std::function<
        std::expected<std::unique_ptr<TcpImpl>, std::string>()>;

    static constexpr size_t max_payload() noexcept { return MaxPayload; }
    static constexpr size_t queue_depth() noexcept { return QueueDepth; }

    /// Create and connect a transport (TCP + TLS + WebSocket handshake).
    /// This is a blocking call -- performs the full handshake sequence.
    /// Returns unique_ptr because Transport owns threads and is non-movable.
    static std::expected<std::unique_ptr<Transport>, std::string>
    create(TcpFactory tcp_factory, const TransportConfig& config) {
        auto log = detail::transport_logger();

        if (!tcp_factory) {
            return std::unexpected("tcp_factory is null");
        }
        if (config.remote_host.empty()) {
            return std::unexpected("remote_host is empty");
        }

        SPDLOG_LOGGER_INFO(log,
            "Creating transport: {}:{}{}", config.remote_host,
            config.remote_port, config.ws_path);

        auto t = std::unique_ptr<Transport>(new Transport());
        t->config_      = config;
        t->tcp_factory_ = std::move(tcp_factory);

        auto conn_result = t->do_connect();
        if (!conn_result) {
            SPDLOG_LOGGER_ERROR(log, "Initial connect failed: {}",
                                conn_result.error());
            return std::unexpected(conn_result.error());
        }

        t->running_.store(true, std::memory_order_release);

        // Start worker threads (capture raw pointer -- Transport outlives threads)
        auto* tp = t.get();
        t->tx_thread_ = std::thread([tp] { tp->tx_loop(); });
        t->rx_thread_ = std::thread([tp] { tp->rx_loop(); });

        SPDLOG_LOGGER_INFO(log, "Transport ready: {}", config.remote_host);

        return t;
    }

    ~Transport() {
        stop();
    }

    Transport(const Transport&)            = delete;
    Transport& operator=(const Transport&) = delete;
    Transport(Transport&&)                 = delete;
    Transport& operator=(Transport&&)      = delete;

    // -----------------------------------------------------------------------
    // Send API (application thread)
    // -----------------------------------------------------------------------

    /// Send data as a WebSocket frame (non-blocking).
    ///
    /// @param data     Payload data
    /// @param len      Payload length (must be <= MaxPayload)
    /// @param opcode   WebSocket opcode (default: binary)
    /// @return 0 on success, -EMSGSIZE if too large, -ENOTCONN if not
    ///         connected, -EAGAIN if queue is full
    int send(const void* data, size_t len,
             uint8_t opcode = ws::opcode::kBinary) noexcept {
        if (len > MaxPayload) return -EMSGSIZE;
        if (!running_.load(std::memory_order_acquire)) return -ENOTCONN;

        bool ok = tx_queue_.try_produce([&](TxMsg& msg) {
            std::memcpy(msg.data, data, len);
            msg.len = static_cast<uint16_t>(len);
            msg.opcode = opcode;
        });

        if (!ok) {
            queue_full_count_.fetch_add(1, std::memory_order_relaxed);
            return -EAGAIN;
        }
        return 0;
    }

    /// Send data from a span (convenience overload).
    int send(std::span<const uint8_t> data,
             uint8_t opcode = ws::opcode::kBinary) noexcept {
        return send(data.data(), data.size(), opcode);
    }

    /// Send data as a WebSocket text frame (convenience for JSON APIs).
    int send_text(const void* data, size_t len) noexcept {
        return send(data, len, ws::opcode::kText);
    }

    // -----------------------------------------------------------------------
    // Receive API (application thread)
    // -----------------------------------------------------------------------

    /// Try to receive a message (non-blocking).
    /// @param callback  Called with (data_ptr, len) if a message is available.
    /// @return true if a message was consumed, false if queue empty.
    /// @warning The data pointer passed to callback is only valid for the
    ///          duration of the callback invocation. Copy the data if you
    ///          need it after the callback returns -- the underlying SPSC
    ///          queue slot will be reused.
    template <typename F>
        requires std::invocable<F, const uint8_t*, uint16_t>
    bool recv(F&& callback) {
        return rx_queue_.try_consume([&](RxMsg& msg) {
            std::invoke(std::forward<F>(callback), msg.data, msg.len);
        });
    }

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /// Stop the transport gracefully. Sends WebSocket Close frame.
    ///
    /// Thread safety: waits for TX/RX threads to exit BEFORE touching
    /// crypto_ or tcp_, avoiding data races on shared state.
    void stop() noexcept {
        bool was_running = running_.exchange(false, std::memory_order_acq_rel);

        auto log = detail::transport_logger();
        SPDLOG_LOGGER_INFO(log, "Stopping transport");

        // Join worker threads FIRST — ensures no concurrent access to
        // crypto_/tcp_ from TX/RX threads when we send the Close frame.
        if (tx_thread_.joinable()) tx_thread_.join();
        if (rx_thread_.joinable()) rx_thread_.join();

        // Send WebSocket Close frame after threads have exited (no race)
        if (was_running && crypto_ && tcp_ && tcp_->is_established()) {
            uint8_t close_buf[128];
            size_t close_len = ws::build_close_frame(
                close_buf, ws::close_code::kNormal, "client shutdown");
            uint8_t tls_buf[256];
            uint16_t tls_len = crypto_->encrypt(
                close_buf, static_cast<uint16_t>(close_len), tls_buf);
            if (tls_len > 0) {
                tcp_->send(tls_buf, tls_len);
            }
        }

        // Close TCP connection
        if (tcp_ && tcp_->is_established()) {
            tcp_->close();
        }

        SPDLOG_LOGGER_INFO(log, "Transport stopped");
    }

    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] TransportStats stats() const noexcept {
        return TransportStats{
            .tx_packets        = tx_stats_.packets,
            .tx_bytes          = tx_stats_.bytes,
            .tx_dropped        = tx_stats_.dropped,
            .rx_packets        = rx_stats_.packets,
            .rx_bytes          = rx_stats_.bytes,
            .encrypt_errors    = tx_stats_.crypto_errors,
            .decrypt_errors    = rx_stats_.crypto_errors,
            .queue_full_count  = queue_full_count_.load(std::memory_order_relaxed),
            .ws_pings_received = ws_pings_received_.load(std::memory_order_relaxed),
            .ws_pongs_sent     = ws_pongs_sent_.load(std::memory_order_relaxed),
            .reconnect_count   = reconnect_count_.load(std::memory_order_relaxed),
        };
    }

private:
    Transport() = default;

    TransportConfig                        config_;
    TcpFactory                             tcp_factory_;
    std::unique_ptr<TcpImpl>               tcp_;
    std::unique_ptr<TlsSession<TcpImpl>>   tls_;   // Only used during create(), not on hot path
    std::unique_ptr<TlsRecordCrypto>       crypto_;

    TxQueue                                tx_queue_{};
    RxQueue                                rx_queue_{};

    std::atomic<bool>                      running_{false};
    // RX sets reconnecting_=true before modifying crypto_/tcp_;
    // TX spins while this flag is set to avoid data races.
    std::atomic<bool>                      reconnecting_{false};
    std::thread                            tx_thread_;
    std::thread                            rx_thread_;

    // Per-thread stats to avoid cross-core atomic contention
    ThreadStats                            tx_stats_{};
    ThreadStats                            rx_stats_{};
    // App-thread-only counters (no contention -- only send() writes these)
    std::atomic<uint64_t>                  queue_full_count_{0};
    std::atomic<uint64_t>                  ws_pings_received_{0};
    std::atomic<uint64_t>                  ws_pongs_sent_{0};
    std::atomic<uint64_t>                  reconnect_count_{0};

    // -----------------------------------------------------------------------
    // CPU affinity
    // -----------------------------------------------------------------------

    /// Pin the calling thread to a specific CPU core.
    static void pin_this_thread(int cpu, const char* name) {
        if (cpu < 0) return;
        cpu_set_t cs;
        CPU_ZERO(&cs);
        CPU_SET(cpu, &cs);
        if (sched_setaffinity(0, sizeof(cs), &cs) == 0) {
            SPDLOG_LOGGER_INFO(detail::transport_logger(),
                "{} thread pinned to CPU {}", name, cpu);
        } else {
            SPDLOG_LOGGER_WARN(detail::transport_logger(),
                "Failed to pin {} thread to CPU {}: {}",
                name, cpu, std::generic_category().message(errno));
        }
    }

    // -----------------------------------------------------------------------
    // Connection establishment (reused by create() and reconnect)
    // -----------------------------------------------------------------------

    /// Full connection sequence: TCP (via factory) -> TLS -> WS Upgrade -> key export.
    /// On success, tcp_, tls_, crypto_ are populated and ready.
    /// On failure, previous state is cleaned up.
    std::expected<void, std::string> do_connect() {
        auto log = detail::transport_logger();

        // Phase 1: Create TCP session via factory (factory handles connect)
        auto tcp_result = tcp_factory_();
        if (!tcp_result) {
            return std::unexpected(std::format(
                "TCP factory failed: {}", tcp_result.error()));
        }
        tcp_ = std::move(*tcp_result);

        if (!tcp_->is_established()) {
            return std::unexpected("TCP factory returned non-established session");
        }

        // Phase 2: TLS handshake
        TlsConfig tls_cfg{
            .hostname = config_.remote_host,
            .ca_cert_path = config_.ca_cert_path,
            .verify_peer = config_.verify_peer,
            .handshake_timeout = config_.tls_timeout,
        };

        auto tls_result = TlsSession<TcpImpl>::create(*tcp_, tls_cfg);
        if (!tls_result) {
            return std::unexpected(std::format(
                "TLS session failed: {}", tls_result.error()));
        }
        tls_ = std::make_unique<TlsSession<TcpImpl>>(std::move(*tls_result));

        auto hs_result = tls_->handshake();
        if (!hs_result) {
            return std::unexpected(std::format(
                "TLS handshake failed: {}", hs_result.error()));
        }

        // Phase 3: WebSocket upgrade
        auto ws_result = do_ws_upgrade();
        if (!ws_result) {
            return std::unexpected(std::format(
                "WebSocket upgrade failed: {}", ws_result.error()));
        }

        // Phase 4: Extract keys for AEAD hot path
        auto hot_state = tls_->extract_hot_state();
        if (!hot_state) {
            return std::unexpected(std::format(
                "TLS key export failed: {}", hot_state.error()));
        }

        size_t key_len = tls_->cipher_key_len();
        auto crypto = TlsRecordCrypto::create(*hot_state, key_len);
        if (!crypto) {
            return std::unexpected(std::format(
                "TLS AEAD init failed: {}", crypto.error()));
        }
        crypto_ = std::make_unique<TlsRecordCrypto>(std::move(*crypto));

        SPDLOG_LOGGER_INFO(log,
            "Connected: {} (TLS: {}, cipher: {})",
            config_.remote_host, tls_->tls_version(),
            tls_->cipher_name());
        return {};
    }

    /// Attempt reconnection with fixed interval. Discards old SPSC queue data.
    /// Called from RX thread when disconnect is detected.
    /// Returns true if reconnection succeeded.
    bool do_reconnect() {
        auto log = detail::transport_logger();
        int max_attempts = config_.max_reconnect_attempts;
        if (max_attempts <= 0) {
            SPDLOG_LOGGER_ERROR(log, "Auto-reconnect disabled, stopping");
            return false;
        }

        // Signal TX thread to pause: it must not touch crypto_/tcp_
        // while we are reconnecting.
        reconnecting_.store(true, std::memory_order_release);

        // Drain and discard TX queue (stale market data)
        TxMsg discard;
        while (tx_queue_.try_consume([&](TxMsg& m) { discard = m; })) {}

        for (int attempt = 1; attempt <= max_attempts; ++attempt) {
            SPDLOG_LOGGER_INFO(log,
                "Reconnect attempt {}/{} in {}ms",
                attempt, max_attempts,
                config_.reconnect_interval.count());

            std::this_thread::sleep_for(config_.reconnect_interval);

            // Clean up old connection state
            crypto_.reset();
            tls_.reset();
            tcp_.reset();

            auto result = do_connect();
            if (result) {
                reconnect_count_.fetch_add(1, std::memory_order_relaxed);
                reconnecting_.store(false, std::memory_order_release);
                SPDLOG_LOGGER_INFO(log,
                    "Reconnected successfully on attempt {}", attempt);
                return true;
            }

            SPDLOG_LOGGER_WARN(log,
                "Reconnect attempt {} failed: {}",
                attempt, result.error());
        }

        reconnecting_.store(false, std::memory_order_release);
        SPDLOG_LOGGER_ERROR(log,
            "All {} reconnect attempts exhausted", max_attempts);
        return false;
    }

    // -----------------------------------------------------------------------
    // WebSocket upgrade (Phase 3 of handshake)
    // -----------------------------------------------------------------------

    std::expected<void, std::string> do_ws_upgrade() {
        auto log = detail::transport_logger();

        // Generate WebSocket key
        auto ws_key_result = http::generate_ws_key();
        if (!ws_key_result) {
            return std::unexpected(ws_key_result.error());
        }
        std::string ws_key = std::move(*ws_key_result);

        // Build upgrade request
        std::string host = config_.remote_host;
        if (config_.remote_port != 443) {
            host += ":" + std::to_string(config_.remote_port);
        }

        std::string request = http::build_upgrade_request(
            host, config_.ws_path, ws_key, config_.extra_headers);

        SPDLOG_LOGGER_DEBUG(log, "Sending WebSocket upgrade request");

        // Send upgrade request through TLS (handshake-phase I/O)
        auto write_result = tls_->handshake_write(request.data(),
                                                    static_cast<int>(request.size()));
        if (!write_result || *write_result <= 0) {
            return std::unexpected("Failed to send WebSocket upgrade request");
        }

        // Read upgrade response (with timeout)
        std::vector<uint8_t> response_buf;
        response_buf.reserve(4096);

        auto deadline = std::chrono::steady_clock::now() + config_.ws_timeout;

        while (std::chrono::steady_clock::now() < deadline) {
            uint8_t buf[4096];
            auto read_result = tls_->handshake_read(buf, sizeof(buf));
            if (!read_result) {
                return std::unexpected(std::format(
                    "Failed to read upgrade response: {}",
                    read_result.error()));
            }

            if (*read_result > 0) {
                response_buf.insert(response_buf.end(),
                                    buf, buf + *read_result);
            }

            // Check if we have a complete HTTP response
            auto response_str = std::string_view(
                reinterpret_cast<const char*>(response_buf.data()),
                response_buf.size());

            if (response_str.find("\r\n\r\n") != std::string_view::npos) {
                // Parse the response
                auto parsed = http::parse_upgrade_response(
                    reinterpret_cast<const char*>(response_buf.data()),
                    response_buf.size());

                if (!parsed) {
                    return std::unexpected(std::format(
                        "Failed to parse upgrade response: {}",
                        parsed.error()));
                }

                if (parsed->status_code != 101) {
                    SPDLOG_LOGGER_ERROR(log,
                        "WebSocket upgrade rejected: status={}",
                        parsed->status_code);
                    return std::unexpected(std::format(
                        "WebSocket upgrade rejected (status {})",
                        parsed->status_code));
                }

                if (!parsed->has_upgrade || !parsed->has_connection_upgrade) {
                    return std::unexpected(
                        "Missing Upgrade/Connection headers in response");
                }

                // Validate Sec-WebSocket-Accept
                if (!http::validate_ws_accept(ws_key,
                                               parsed->sec_ws_accept)) {
                    SPDLOG_LOGGER_ERROR(log,
                        "Sec-WebSocket-Accept validation failed");
                    return std::unexpected(
                        "Sec-WebSocket-Accept validation failed");
                }

                SPDLOG_LOGGER_INFO(log, "WebSocket upgrade successful");
                return {};
            }
        }

        return std::unexpected("WebSocket upgrade response timeout");
    }

    // -----------------------------------------------------------------------
    // TX worker loop (runs on dedicated thread)
    // -----------------------------------------------------------------------

    void tx_loop() {
        pin_this_thread(config_.tx_cpu, "TX");
        auto log = detail::transport_logger();
        SPDLOG_LOGGER_DEBUG(log, "TX loop started");

        // WS encode buffer: header + payload + 1 byte for TLS content type append
        constexpr size_t kWsBufSize =
            ws::kMaxFrameHeaderLen + MaxPayload + 1;
        // TLS output buffer: sized for the actual max WS frame (without the +1 temp byte)
        constexpr size_t kMaxWsFrame =
            ws::kMaxFrameHeaderLen + MaxPayload;
        constexpr size_t kTlsBufSize =
            TlsRecordCrypto::encrypted_size(
                static_cast<uint16_t>(kMaxWsFrame));

        // Batch buffers for drain loop (sized from config)
        const int kMaxBatch = config_.tx_burst_size;
        auto batch    = std::make_unique<TxMsg[]>(kMaxBatch);
        auto tls_bufs_storage = std::make_unique<uint8_t[]>(
            static_cast<size_t>(kMaxBatch) * kTlsBufSize);
        auto tls_lens = std::make_unique<uint16_t[]>(kMaxBatch);

        // Single WS encode buffer reused per message
        uint8_t ws_buf[kWsBufSize];

        ws::FrameTemplate ws_tmpl = ws::FrameTemplate::for_binary();

        auto last_ping = std::chrono::steady_clock::now();

        while (running_.load(std::memory_order_acquire)) {
            // Spin-wait while RX thread is reconnecting to avoid
            // touching crypto_/tcp_ which are being replaced.
            if (reconnecting_.load(std::memory_order_acquire)) [[unlikely]] {
                eph::utils::cpu_relax();
                continue;
            }

            // -- WebSocket ping (periodic keepalive, owned by TX thread) --
            if (config_.ping_interval.count() > 0) {
                auto now = std::chrono::steady_clock::now();
                if (now - last_ping >= config_.ping_interval) {
                    send_ws_ping(ws_buf, tls_bufs_storage.get());
                    last_ping = now;
                }
            }

            // Drain: consume as many messages as available, up to kMaxBatch
            int n = 0;
            while (n < kMaxBatch) {
                bool got = tx_queue_.try_consume([&](TxMsg& msg) {
                    batch[n] = msg;
                    n++;
                });
                if (!got) break;
            }

            if (n == 0) continue;

            // WS encode -> TLS encrypt for each message in batch
            for (int i = 0; i < n; ++i) {
                size_t ws_len;

                // Control frames (pong) use encode_frame directly;
                // data frames use the precomputed template.
                if (batch[i].opcode == ws::opcode::kPong) {
                    ws_len = ws::encode_frame(
                        ws_buf, ws::opcode::kPong,
                        batch[i].data, batch[i].len);
                } else {
                    ws_len = ws_tmpl.encode(
                        ws_buf, batch[i].data, batch[i].len);
                }

                uint8_t* tls_buf_i = tls_bufs_storage.get() +
                    static_cast<size_t>(i) * kTlsBufSize;
                tls_lens[i] = crypto_->encrypt(
                    ws_buf, static_cast<uint16_t>(ws_len),
                    tls_buf_i);

                if (tls_lens[i] == 0) {
                    tx_stats_.crypto_errors++;
                }
            }

            // Send all encrypted packets through TCP
            for (int i = 0; i < n; ++i) {
                if (tls_lens[i] == 0) continue;

                uint8_t* tls_buf_i = tls_bufs_storage.get() +
                    static_cast<size_t>(i) * kTlsBufSize;
                auto result = tcp_->send(tls_buf_i, tls_lens[i]);
                if (!result) {
                    tx_stats_.dropped++;
                    SPDLOG_LOGGER_WARN(log,
                        "TCP send failed (dropped): {}", result.error());
                } else {
                    tx_stats_.packets++;
                    tx_stats_.bytes += batch[i].len;
                }
            }
        }

        SPDLOG_LOGGER_DEBUG(log, "TX loop exited");
    }

    // -----------------------------------------------------------------------
    // RX worker loop (runs on dedicated thread)
    // -----------------------------------------------------------------------

    void rx_loop() {
        pin_this_thread(config_.rx_cpu, "RX");
        auto log = detail::transport_logger();
        SPDLOG_LOGGER_DEBUG(log, "RX loop started");

        // Fixed-size RX buffers -- no heap allocation on hot path.
        // reassembly_buf is 2x max TLS record to handle partial records at boundary.
        static constexpr size_t kReassemblyBufSize =
            2 * (tls_const::kMaxRecordPayload + tls_record::kRecordHeaderLen +
                 tls_record::kAuthTagLen + 1);
        auto decrypt_buf = std::make_unique<uint8_t[]>(
            tls_const::kMaxRecordPayload + 256);
        auto reassembly_storage = std::make_unique<uint8_t[]>(kReassemblyBufSize);
        size_t reassembly_len = 0;

        while (running_.load(std::memory_order_acquire)) {
            // -- Receive data via poll_rx --
            bool reconnect_needed = false;
            auto rx_result = tcp_->poll_rx(
                [&](const uint8_t* data, uint16_t len) {
                    if (reassembly_len + len <= kReassemblyBufSize) {
                        std::memcpy(reassembly_storage.get() + reassembly_len,
                                    data, len);
                        reassembly_len += len;
                    } else {
                        SPDLOG_LOGGER_ERROR(log,
                            "RX reassembly buffer overflow ({} + {} > {}), "
                            "triggering reconnect",
                            reassembly_len, len, kReassemblyBufSize);
                        reassembly_len = 0;
                        reconnect_needed = true;
                    }
                });

            // Reassembly buffer overflow -> reconnect
            if (reconnect_needed) {
                if (!do_reconnect()) {
                    running_.store(false, std::memory_order_release);
                    break;
                }
                continue;
            }

            if (!rx_result) {
                SPDLOG_LOGGER_WARN(log, "TCP rx error: {}",
                                   rx_result.error());

                // -- Auto-reconnect (fixed interval, discard old messages) --
                reassembly_len = 0;
                if (do_reconnect()) {
                    continue; // Resume RX loop with new connection
                }

                // Reconnect exhausted -- stop transport
                running_.store(false, std::memory_order_release);
                break;
            }

            // No data received this poll iteration
            if (*rx_result == 0) continue;

            // Decrypt complete TLS records from reassembly buffer
            size_t consumed = 0;
            while (reassembly_len - consumed >=
                   tls_record::kRecordHeaderLen + tls_record::kAuthTagLen) {
                const uint8_t* rec_ptr = reassembly_storage.get() + consumed;

                uint8_t content_type;
                uint16_t payload_len;
                if (!tls_record::parse_record_header(
                        rec_ptr, content_type, payload_len)) {
                    break;
                }

                size_t record_total = tls_record::kRecordHeaderLen + payload_len;
                if (reassembly_len - consumed < record_total) break;

                uint16_t decrypted_len;
                bool ok = crypto_->decrypt(
                    rec_ptr,
                    static_cast<uint16_t>(record_total),
                    decrypt_buf.get(), decrypted_len);

                if (!ok) {
                    rx_stats_.crypto_errors++;
                    SPDLOG_LOGGER_WARN(log,
                        "TLS decrypt failed -- triggering reconnect");
                    // Corrupted record -> link unreliable, reconnect.
                    // Reset reassembly state and skip compact logic below.
                    reassembly_len = 0;
                    consumed = 0;
                    if (!do_reconnect()) {
                        running_.store(false, std::memory_order_release);
                    }
                    break; // Resume with fresh connection or exit outer loop
                }

                process_ws_data(decrypt_buf.get(), decrypted_len);
                consumed += record_total;
            }

            // Compact: move unconsumed data to front (memmove, not erase)
            if (consumed > 0) {
                reassembly_len -= consumed;
                if (reassembly_len > 0) {
                    std::memmove(reassembly_storage.get(),
                                 reassembly_storage.get() + consumed,
                                 reassembly_len);
                }
            }
        }

        SPDLOG_LOGGER_DEBUG(log, "RX loop exited");
    }

    /// Send a WebSocket ping frame (called from TX thread only).
    /// Uses caller-provided buffers to avoid extra stack allocations.
    bool send_ws_ping(uint8_t* ws_buf, uint8_t* tls_buf) noexcept {
        size_t ping_len = ws::build_ping_frame(ws_buf);

        uint16_t tls_len = crypto_->encrypt(
            ws_buf, static_cast<uint16_t>(ping_len), tls_buf);
        if (tls_len == 0) return false;

        auto result = tcp_->send(tls_buf, tls_len);
        if (!result) {
            SPDLOG_LOGGER_DEBUG(detail::transport_logger(),
                "WS ping send failed: {}", result.error());
            return false;
        }
        return true;
    }

    // -----------------------------------------------------------------------
    // WebSocket frame processing
    // -----------------------------------------------------------------------

    void process_ws_data(const uint8_t* data, uint16_t len) {
        auto log = detail::transport_logger();
        size_t offset = 0;

        while (offset < len) {
            auto frame = ws::decode_frame(data + offset, len - offset);
            if (!frame) {
                if (frame.error() == "incomplete") break;
                SPDLOG_LOGGER_WARN(log, "WS frame decode error: {}",
                                   frame.error());
                break;
            }

            offset += frame->total_len;
            rx_stats_.packets++;

            if (frame->is_ping()) {
                ws_pings_received_.fetch_add(1, std::memory_order_relaxed);
                handle_ping(*frame);
                continue;
            }

            if (frame->is_close()) {
                SPDLOG_LOGGER_INFO(log,
                    "Received WS Close frame: code={}",
                    frame->close_status_code());
                running_.store(false, std::memory_order_release);
                break;
            }

            if (frame->is_pong()) {
                continue;
            }

            // Data frame -- push to receive queue
            if (frame->is_data() && frame->payload_len > 0 &&
                frame->payload_len <= MaxPayload) {

                bool ok = rx_queue_.try_produce([&](RxMsg& msg) {
                    std::memcpy(msg.data, frame->payload,
                                frame->payload_len);
                    if (frame->masked) {
                        ws::apply_mask(msg.data, frame->payload_len,
                                       frame->mask_key);
                    }
                    msg.len = static_cast<uint16_t>(frame->payload_len);
                    msg.opcode = frame->opcode;
                });

                if (ok) {
                    rx_stats_.bytes += frame->payload_len;
                } else {
                    rx_stats_.dropped++;
                    // Log every 1000th drop to avoid log flooding
                    if (rx_stats_.dropped % 1000 == 1) {
                        SPDLOG_LOGGER_WARN(log,
                            "RX queue full, dropping data frame "
                            "(total dropped: {})", rx_stats_.dropped);
                    }
                }
            }
        }
    }

    /// Enqueue pong response into TX queue so the TX thread sends it.
    /// This avoids data races: only TX thread touches crypto_->encrypt().
    void handle_ping(const ws::DecodedFrame& ping_frame) {
        // Ping payload is at most 125 bytes (RFC 6455 §5.5).
        // Enqueue the unmasked payload with kPong opcode; TX thread
        // will encode the WS frame and encrypt it.
        size_t pong_payload_len = std::min(
            static_cast<size_t>(ping_frame.payload_len),
            static_cast<size_t>(MaxPayload));

        bool ok = tx_queue_.try_produce([&](TxMsg& msg) {
            if (ping_frame.payload && pong_payload_len > 0) {
                std::memcpy(msg.data, ping_frame.payload, pong_payload_len);
                if (ping_frame.masked) {
                    ws::apply_mask(msg.data, pong_payload_len,
                                   ping_frame.mask_key);
                }
            }
            msg.len = static_cast<uint16_t>(pong_payload_len);
            msg.opcode = ws::opcode::kPong;
        });

        if (ok) {
            ws_pongs_sent_.fetch_add(1, std::memory_order_relaxed);
        } else {
            SPDLOG_LOGGER_DEBUG(detail::transport_logger(),
                "TX queue full, dropping pong response");
        }
    }
};

} // namespace eph::net
