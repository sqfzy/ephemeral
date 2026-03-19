#pragma once

/// @file transport.hpp
/// Public API for the DPDK WebSocket transport.
///
/// Provides a single entry point for establishing a WSS connection over
/// DPDK and sending/receiving data with minimal latency.
///
/// Architecture:
///   Control thread (handshake):
///     TCP connect → TLS 1.3 handshake → WebSocket Upgrade
///   Data plane (hot path):
///     app send() → SPSC queue → TX lcore → WS frame → TLS encrypt →
///     TCP segment → IP packet → tx_burst
///
/// Thread model:
///   - Application thread: calls send()/recv(), non-blocking
///   - TX lcore: busy-poll SPSC queue, build packets, tx_burst
///   - RX lcore: busy-poll rx_burst, decrypt, parse WS frames, push to recv queue

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
#include <vector>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <rte_ethdev.h>
#include <rte_mbuf.h>

#include "eph/base/cache.hpp"
#include "eph/containers/bounded_queue.hpp"
#include "eph/dpdk/http.hpp"
#include "eph/dpdk/net_header.hpp"
#include "eph/dpdk/tcp.hpp"
#include "eph/dpdk/tls_record.hpp"
#include "eph/dpdk/tls_session.hpp"
#include "eph/dpdk/websocket.hpp"

namespace eph::dpdk {

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

struct TransportConfig {
    // Connection target
    std::string remote_host{};      // Hostname for TLS SNI and HTTP Host
    uint16_t    remote_port = 443;  // Remote TCP port
    std::string ws_path     = "/";  // WebSocket upgrade path
    std::string extra_headers{};    // Additional HTTP headers for upgrade

    // Network identity
    net::ConnectionTuple tuple{};   // Source/dest IP and ports
    rte_ether_addr       src_mac{}; // Source MAC address
    rte_ether_addr       dst_mac{}; // Destination (gateway) MAC address

    // DPDK port/queue
    uint16_t port_id      = 0;
    uint16_t tx_queue_id  = 0;
    uint16_t rx_queue_id  = 0;

    // TLS
    std::string ca_cert_path{};     // CA cert file, empty = system default
    bool        verify_peer = true;

    // Timeouts
    std::chrono::milliseconds tcp_timeout{3000};
    std::chrono::milliseconds tls_timeout{5000};
    std::chrono::milliseconds ws_timeout{3000};

    // Performance
    uint16_t tx_burst_size = 32;    // Max packets per tx_burst
    uint16_t rx_burst_size = 32;    // Max packets per rx_burst
};

// ─────────────────────────────────────────────────────────────────────────────
// Transport stats
// ─────────────────────────────────────────────────────────────────────────────

struct TransportStats {
    uint64_t tx_packets       = 0;
    uint64_t tx_bytes         = 0;
    uint64_t tx_dropped       = 0;
    uint64_t rx_packets       = 0;
    uint64_t rx_bytes         = 0;
    uint64_t encrypt_errors   = 0;
    uint64_t decrypt_errors   = 0;
    uint64_t mbuf_alloc_failures = 0;
    uint64_t queue_full_count = 0;
    uint64_t ws_pings_received = 0;
    uint64_t ws_pongs_sent     = 0;
    uint64_t reconnect_count   = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Internal message types for SPSC queue
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

/// Message passed from application thread to TX lcore via SPSC queue.
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
        auto lg = spdlog::stdout_color_mt("dpdk.transport");
        lg->set_level(spdlog::level::trace);
        return lg;
    }();
    return l;
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Transport — public API
// ─────────────────────────────────────────────────────────────────────────────

/// DPDK WebSocket transport with TLS 1.3 encryption.
///
/// Template parameters:
///   MaxPayload — maximum application payload size per message
///   QueueDepth — SPSC queue capacity (must be power of 2)
///
/// Usage:
///   auto transport = Transport<512, 1024>::create(pool, config);
///   transport->send(data, len);  // Non-blocking
///   transport->recv([](auto* data, auto len) { ... });  // Non-blocking
template <size_t MaxPayload = 512, size_t QueueDepth = 1024>
class Transport {
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
    static constexpr size_t max_payload() noexcept { return MaxPayload; }
    static constexpr size_t queue_depth() noexcept { return QueueDepth; }

    /// Create and connect a transport (TCP + TLS + WebSocket handshake).
    /// This is a blocking call — performs the full handshake sequence.
    static std::expected<Transport, std::string>
    create(rte_mempool* pool, const TransportConfig& config) {
        auto log = detail::transport_logger();

        if (!pool) {
            return std::unexpected("mempool is null");
        }
        if (config.remote_host.empty()) {
            return std::unexpected("remote_host is empty");
        }

        SPDLOG_LOGGER_INFO(log,
            "Creating transport: {}:{}{}", config.remote_host,
            config.remote_port, config.ws_path);

        Transport t;
        t.config_ = config;
        t.pool_   = pool;

        // Phase 1: TCP connect
        TcpConfig tcp_cfg{
            .tuple       = config.tuple,
            .src_mac     = config.src_mac,
            .dst_mac     = config.dst_mac,
            .mss         = net::kDefaultMss,
            .recv_window = 65535,
            .port_id     = config.port_id,
            .tx_queue_id = config.tx_queue_id,
            .rx_queue_id = config.rx_queue_id,
        };

        t.tcp_ = std::make_unique<TcpSession>(tcp_cfg, pool);
        auto tcp_result = t.tcp_->connect(config.tcp_timeout);
        if (!tcp_result) {
            SPDLOG_LOGGER_ERROR(log, "TCP connect failed: {}",
                                tcp_result.error());
            return std::unexpected(std::format(
                "TCP connect failed: {}", tcp_result.error()));
        }

        // Phase 2: TLS handshake
        TlsConfig tls_cfg{
            .hostname = config.remote_host,
            .ca_cert_path = config.ca_cert_path,
            .verify_peer = config.verify_peer,
            .handshake_timeout = config.tls_timeout,
        };

        auto tls_result = TlsSession::create(*t.tcp_, pool, tls_cfg);
        if (!tls_result) {
            SPDLOG_LOGGER_ERROR(log, "TLS session create failed: {}",
                                tls_result.error());
            return std::unexpected(std::format(
                "TLS session failed: {}", tls_result.error()));
        }
        t.tls_ = std::make_unique<TlsSession>(std::move(*tls_result));

        auto hs_result = t.tls_->handshake();
        if (!hs_result) {
            SPDLOG_LOGGER_ERROR(log, "TLS handshake failed: {}",
                                hs_result.error());
            return std::unexpected(std::format(
                "TLS handshake failed: {}", hs_result.error()));
        }

        // Phase 3: WebSocket upgrade
        auto ws_result = t.do_ws_upgrade();
        if (!ws_result) {
            SPDLOG_LOGGER_ERROR(log, "WebSocket upgrade failed: {}",
                                ws_result.error());
            return std::unexpected(std::format(
                "WebSocket upgrade failed: {}", ws_result.error()));
        }

        // Phase 4: Extract TLS session keys for hot path
        auto hot_state = t.tls_->extract_hot_state();
        if (!hot_state) {
            SPDLOG_LOGGER_WARN(log,
                "Failed to extract TLS hot state: {} — "
                "falling back to SSL_write/SSL_read",
                hot_state.error());
            t.use_hot_path_ = false;
        } else {
            auto crypto = TlsRecordCrypto::create(*hot_state);
            if (!crypto) {
                SPDLOG_LOGGER_WARN(log,
                    "Failed to init TLS record crypto: {} — "
                    "falling back to SSL_write/SSL_read",
                    crypto.error());
                t.use_hot_path_ = false;
            } else {
                t.crypto_ = std::make_unique<TlsRecordCrypto>(
                    std::move(*crypto));
                t.use_hot_path_ = true;
            }
        }

        t.running_.store(true, std::memory_order_release);

        // Start TX worker thread
        t.tx_thread_ = std::thread([&t] { t.tx_loop(); });

        // Start RX worker thread
        t.rx_thread_ = std::thread([&t] { t.rx_loop(); });

        SPDLOG_LOGGER_INFO(log,
            "Transport ready: {} (TLS: {}, cipher: {}, hot_path: {})",
            config.remote_host, t.tls_->tls_version(),
            t.tls_->cipher_name(), t.use_hot_path_);

        return t;
    }

    ~Transport() {
        stop();
    }

    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    Transport(Transport&& other) noexcept
        : config_(std::move(other.config_))
        , pool_(other.pool_)
        , tcp_(std::move(other.tcp_))
        , tls_(std::move(other.tls_))
        , crypto_(std::move(other.crypto_))
        , use_hot_path_(other.use_hot_path_)
        , tx_thread_(std::move(other.tx_thread_))
        , rx_thread_(std::move(other.rx_thread_))
        , stats_(other.stats_) {
        running_.store(other.running_.load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
        other.running_.store(false, std::memory_order_relaxed);
        other.pool_ = nullptr;
    }

    Transport& operator=(Transport&&) = delete;

    // ─────────────────────────────────────────────────────────────────────────
    // Send API (application thread)
    // ─────────────────────────────────────────────────────────────────────────

    /// Send data as a WebSocket binary frame (non-blocking).
    ///
    /// @param data     Payload data
    /// @param len      Payload length (must be <= MaxPayload)
    /// @return 0 on success, -EMSGSIZE if too large, -ENOTCONN if not
    ///         connected, -EAGAIN if queue is full
    int send(const void* data, size_t len) noexcept {
        if (len > MaxPayload) return -EMSGSIZE;
        if (!running_.load(std::memory_order_acquire)) return -ENOTCONN;

        bool ok = tx_queue_.try_produce([&](TxMsg& msg) {
            std::memcpy(msg.data, data, len);
            msg.len = static_cast<uint16_t>(len);
            msg.opcode = ws::opcode::kBinary;
        });

        if (!ok) {
            stats_.queue_full_count++;
            return -EAGAIN;
        }
        return 0;
    }

    /// Send data from a span (convenience overload).
    int send(std::span<const uint8_t> data) noexcept {
        return send(data.data(), data.size());
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Receive API (application thread)
    // ─────────────────────────────────────────────────────────────────────────

    /// Try to receive a message (non-blocking).
    /// @param callback  Called with (data_ptr, len) if a message is available
    /// @return true if a message was consumed, false if queue empty
    template <typename F>
        requires std::invocable<F, const uint8_t*, uint16_t>
    bool recv(F&& callback) noexcept {
        return rx_queue_.try_consume([&](RxMsg& msg) {
            std::invoke(std::forward<F>(callback), msg.data, msg.len);
        });
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Lifecycle
    // ─────────────────────────────────────────────────────────────────────────

    /// Stop the transport gracefully. Sends WebSocket Close frame.
    void stop() noexcept {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;

        auto log = detail::transport_logger();
        SPDLOG_LOGGER_INFO(log, "Stopping transport");

        // Send WebSocket Close frame
        if (tls_ && tls_->is_handshake_done()) {
            uint8_t close_buf[128];
            size_t close_len = ws::build_close_frame(
                close_buf, ws::close_code::kNormal, "client shutdown");
            tls_->write(close_buf, static_cast<int>(close_len));
        }

        // Join worker threads
        if (tx_thread_.joinable()) tx_thread_.join();
        if (rx_thread_.joinable()) rx_thread_.join();

        // Close TCP connection
        if (tcp_ && tcp_->is_established()) {
            tcp_->close();
        }

        SPDLOG_LOGGER_INFO(log, "Transport stopped");
    }

    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] TransportStats stats() const noexcept { return stats_; }

private:
    Transport() = default;

    TransportConfig                   config_;
    rte_mempool*                      pool_ = nullptr;
    std::unique_ptr<TcpSession>       tcp_;
    std::unique_ptr<TlsSession>       tls_;
    std::unique_ptr<TlsRecordCrypto>  crypto_;
    bool                              use_hot_path_ = false;

    TxQueue                           tx_queue_{};
    RxQueue                           rx_queue_{};

    std::atomic<bool>                 running_{false};
    std::thread                       tx_thread_;
    std::thread                       rx_thread_;

    TransportStats                    stats_{};

    // ─────────────────────────────────────────────────────────────────────────
    // WebSocket upgrade (Phase 3 of handshake)
    // ─────────────────────────────────────────────────────────────────────────

    std::expected<void, std::string> do_ws_upgrade() {
        auto log = detail::transport_logger();

        // Generate WebSocket key
        std::string ws_key = http::generate_ws_key();

        // Build upgrade request
        std::string host = config_.remote_host;
        if (config_.remote_port != 443) {
            host += ":" + std::to_string(config_.remote_port);
        }

        std::string request = http::build_upgrade_request(
            host, config_.ws_path, ws_key, config_.extra_headers);

        SPDLOG_LOGGER_DEBUG(log, "Sending WebSocket upgrade request");

        // Send upgrade request through TLS
        auto write_result = tls_->write(request.data(),
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
            auto read_result = tls_->read(buf, sizeof(buf));
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

    // ─────────────────────────────────────────────────────────────────────────
    // TX worker loop (runs on dedicated thread)
    // ─────────────────────────────────────────────────────────────────────────

    void tx_loop() {
        auto log = detail::transport_logger();
        SPDLOG_LOGGER_DEBUG(log, "TX loop started");

        // Workspace buffer for frame construction
        // Max: WS header(14) + payload(MaxPayload) + TLS overhead
        constexpr size_t kWorkBufSize =
            ws::kMaxFrameHeaderLen + MaxPayload +
            TlsRecordCrypto::encrypted_size(0) + MaxPayload;
        auto work_buf = std::make_unique<uint8_t[]>(kWorkBufSize);

        ws::FrameTemplate ws_tmpl = ws::FrameTemplate::for_binary();

        while (running_.load(std::memory_order_acquire)) {
            tx_queue_.try_consume([&](TxMsg& msg) {
                // Step 1: Build WebSocket frame
                size_t ws_len = ws_tmpl.encode(
                    work_buf.get(), msg.data, msg.len);

                // Step 2: Encrypt with TLS
                if (use_hot_path_ && crypto_) {
                    // Hot path: direct AEAD encryption
                    uint8_t tls_buf[tls_const::kMaxRecordPayload + 256];
                    uint16_t tls_len = crypto_->encrypt(
                        work_buf.get(), static_cast<uint16_t>(ws_len),
                        tls_buf);

                    if (tls_len == 0) {
                        stats_.encrypt_errors++;
                        SPDLOG_LOGGER_WARN(log, "TLS encrypt failed");
                        return;
                    }

                    // Step 3: Send through TCP
                    auto result = tcp_->send(tls_buf, tls_len);
                    if (!result) {
                        stats_.tx_dropped++;
                        SPDLOG_LOGGER_WARN(log,
                            "TCP send failed: {}", result.error());
                        return;
                    }
                } else {
                    // Slow path: SSL_write (handles TLS record framing)
                    auto result = tls_->write(
                        work_buf.get(), static_cast<int>(ws_len));
                    if (!result || *result <= 0) {
                        stats_.tx_dropped++;
                        return;
                    }
                }

                stats_.tx_packets++;
                stats_.tx_bytes += msg.len;
            });
        }

        SPDLOG_LOGGER_DEBUG(log, "TX loop exited");
    }

    // ─────────────────────────────────────────────────────────────────────────
    // RX worker loop (runs on dedicated thread)
    // ─────────────────────────────────────────────────────────────────────────

    void rx_loop() {
        auto log = detail::transport_logger();
        SPDLOG_LOGGER_DEBUG(log, "RX loop started");

        std::vector<uint8_t> decrypt_buf(tls_const::kMaxRecordPayload + 256);
        std::vector<uint8_t> reassembly_buf;
        reassembly_buf.reserve(tls_const::kMaxRecordPayload);

        while (running_.load(std::memory_order_acquire)) {
            // Read data through TLS
            uint8_t read_buf[16384];
            int bytes_read = 0;

            if (use_hot_path_ && crypto_) {
                // Hot path: rx_burst + manual TLS decrypt
                rte_mbuf* pkts[32];
                uint16_t nb_rx = rte_eth_rx_burst(
                    config_.port_id, config_.rx_queue_id, pkts, 32);

                if (nb_rx == 0) continue;

                // Process through TCP layer
                auto tcp_result = tcp_->process_rx(pkts, nb_rx,
                    [&](const uint8_t* data, uint16_t len) {
                        // Data from TCP → need to decrypt TLS records
                        reassembly_buf.insert(reassembly_buf.end(),
                                              data, data + len);
                    });

                if (!tcp_result) {
                    SPDLOG_LOGGER_WARN(log, "TCP rx error: {}",
                                       tcp_result.error());
                    // Packet loss — trigger reconnect
                    running_.store(false, std::memory_order_release);
                    stats_.reconnect_count++;
                    break;
                }

                // Try to decrypt complete TLS records from reassembly buffer
                while (reassembly_buf.size() >=
                       tls_record::kRecordHeaderLen + tls_record::kAuthTagLen) {
                    uint8_t content_type;
                    uint16_t payload_len;
                    if (!tls_record::parse_record_header(
                            reassembly_buf.data(), content_type, payload_len)) {
                        break;
                    }

                    size_t record_total = tls_record::kRecordHeaderLen + payload_len;
                    if (reassembly_buf.size() < record_total) break;

                    uint16_t decrypted_len;
                    bool ok = crypto_->decrypt(
                        reassembly_buf.data(),
                        static_cast<uint16_t>(record_total),
                        decrypt_buf.data(), decrypted_len);

                    if (!ok) {
                        stats_.decrypt_errors++;
                        SPDLOG_LOGGER_WARN(log, "TLS decrypt failed");
                        break;
                    }

                    // Process decrypted WebSocket frames
                    process_ws_data(decrypt_buf.data(), decrypted_len);

                    // Remove processed record
                    reassembly_buf.erase(
                        reassembly_buf.begin(),
                        reassembly_buf.begin() +
                            static_cast<ptrdiff_t>(record_total));
                }

            } else {
                // Slow path: SSL_read
                auto result = tls_->read(read_buf, sizeof(read_buf));
                if (!result) {
                    SPDLOG_LOGGER_WARN(log, "TLS read error: {}",
                                       result.error());
                    break;
                }

                bytes_read = *result;
                if (bytes_read > 0) {
                    process_ws_data(read_buf, static_cast<uint16_t>(bytes_read));
                }
            }
        }

        SPDLOG_LOGGER_DEBUG(log, "RX loop exited");
    }

    // ─────────────────────────────────────────────────────────────────────────
    // WebSocket frame processing
    // ─────────────────────────────────────────────────────────────────────────

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
            stats_.rx_packets++;

            if (frame->is_ping()) {
                // Auto-respond with pong
                stats_.ws_pings_received++;
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
                continue; // Ignore pongs
            }

            // Data frame — push to receive queue
            if (frame->is_data() && frame->payload_len > 0 &&
                frame->payload_len <= MaxPayload) {

                bool ok = rx_queue_.try_produce([&](RxMsg& msg) {
                    // If server frame is masked (unusual), unmask first
                    if (frame->masked) {
                        std::memcpy(msg.data, frame->payload,
                                    frame->payload_len);
                        ws::apply_mask(msg.data, frame->payload_len,
                                       frame->mask_key);
                    } else {
                        std::memcpy(msg.data, frame->payload,
                                    frame->payload_len);
                    }
                    msg.len = static_cast<uint16_t>(frame->payload_len);
                    msg.opcode = frame->opcode;
                });

                if (ok) {
                    stats_.rx_bytes += frame->payload_len;
                }
            }
        }
    }

    void handle_ping(const ws::DecodedFrame& ping_frame) {
        uint8_t pong_buf[256];

        // Get the unmasked payload for pong response
        if (ping_frame.masked && ping_frame.payload_len > 0) {
            uint8_t unmasked[125];
            std::memcpy(unmasked, ping_frame.payload, ping_frame.payload_len);
            ws::apply_mask(unmasked, ping_frame.payload_len,
                           ping_frame.mask_key);
            size_t pong_len = ws::build_pong_frame(
                pong_buf, unmasked, ping_frame.payload_len);
            tls_->write(pong_buf, static_cast<int>(pong_len));
        } else {
            size_t pong_len = ws::build_pong_frame(
                pong_buf, ping_frame.payload, ping_frame.payload_len);
            tls_->write(pong_buf, static_cast<int>(pong_len));
        }

        stats_.ws_pongs_sent++;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Type aliases for common configurations
// ─────────────────────────────────────────────────────────────────────────────

/// Default transport: 512-byte max payload, 1024-deep queue.
using DefaultTransport = Transport<512, 1024>;

/// Small transport for control messages.
using SmallTransport = Transport<64, 256>;

/// Large transport for bulk data.
using LargeTransport = Transport<4096, 512>;

} // namespace eph::dpdk
