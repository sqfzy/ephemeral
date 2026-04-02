// NOTE: Included inside Transport class body — do not include directly.

// -----------------------------------------------------------------------
// State change notification
// -----------------------------------------------------------------------

    void notify_state(TransportEvent event, std::string_view detail = {}) noexcept {
        // Always log state transitions for production observability —
        // even if no user callback is registered, the log trail is essential
        // for diagnosing connection issues post-mortem.
        if (detail.empty()) {
            SPDLOG_LOGGER_INFO(detail::transport_logger(),
                "Transport state: {} [{}:{}]",
                transport_event_name(event),
                config_.remote_host, config_.remote_port);
        } else {
            SPDLOG_LOGGER_INFO(detail::transport_logger(),
                "Transport state: {} — {} [{}:{}]",
                transport_event_name(event), detail,
                config_.remote_host, config_.remote_port);
        }

        if (config_.on_state_change) {
            try {
                config_.on_state_change(event, detail);
            } catch (...) {
                // Callback must not throw, but guard defensively
                SPDLOG_LOGGER_WARN(detail::transport_logger(),
                    "on_state_change callback threw an exception");
            }
        }
    }


    // -----------------------------------------------------------------------
    // Connection establishment (reused by create() and reconnect)
    // -----------------------------------------------------------------------

    /// Full connection sequence: TCP (via factory) -> [TLS] -> WS Upgrade -> [key export].
    /// TLS phases are skipped when config_.use_tls is false (plain ws://).
    /// On success, tcp_ (and optionally tls_, crypto_) are populated and ready.
    /// On failure, previous state is cleaned up.
    [[nodiscard]] std::expected<void, ConnectionErrorInfo> do_connect() {
        auto log = detail::transport_logger();
        auto connect_start = std::chrono::steady_clock::now();

        // Phase 1: Create TCP session via factory (factory handles connect)
        auto tcp_phase_start = std::chrono::steady_clock::now();
        auto tcp_result = tcp_factory_();
        if (!tcp_result) {
            return std::unexpected(ConnectionErrorInfo{
                ConnectionError::kFactoryFailed,
                std::format("TCP factory failed: {}", tcp_result.error())});
        }
        tcp_ = std::move(*tcp_result);
        last_tcp_connect_ns_ = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - tcp_phase_start).count());

        if (!tcp_->is_established()) {
            return std::unexpected(ConnectionErrorInfo{
                ConnectionError::kTcpNotEstablished,
                "TCP factory returned non-established session"});
        }

        last_tls_handshake_ns_ = 0;
        if (config_.use_tls) {
            // Phase 2: TLS handshake
            auto tls_phase_start = std::chrono::steady_clock::now();
            TlsConfig tls_cfg{
                .hostname = config_.remote_host,
                .ca_cert_path = config_.ca_cert_path,
                .verify_peer = config_.verify_peer,
                .handshake_timeout = config_.tls_timeout,
                .client_cert_path = config_.client_cert_path,
                .client_key_path = config_.client_key_path,
            };

            auto tls_result = TlsSession<TcpImpl>::create(*tcp_, tls_cfg);
            if (!tls_result) {
                return std::unexpected(ConnectionErrorInfo{
                    ConnectionError::kTlsSessionFailed,
                    std::format("TLS session failed: {}", tls_result.error())});
            }
            tls_ = std::make_unique<TlsSession<TcpImpl>>(std::move(*tls_result));

            auto hs_result = tls_->handshake();
            if (!hs_result) {
                return std::unexpected(ConnectionErrorInfo{
                    ConnectionError::kTlsHandshakeFailed,
                    std::format("TLS handshake failed: {}", hs_result.error())});
            }
            last_tls_handshake_ns_ = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - tls_phase_start).count());
        }

        // Phase 3: WebSocket upgrade (over TLS or plain TCP)
        // Only performed when using WsFramer — other framers skip the
        // HTTP Upgrade handshake and go straight to the data plane.
        if constexpr (kIsWebSocket) {
            auto ws_phase_start = std::chrono::steady_clock::now();
            auto ws_result = do_ws_upgrade();
            if (!ws_result) {
                return std::unexpected(ws_result.error());
            }
            last_ws_upgrade_ns_ = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - ws_phase_start).count());
        }

        if (config_.use_tls) {
            // Phase 4: Extract keys for AEAD hot path
            auto hot_state = tls_->extract_hot_state();
            if (!hot_state) {
                return std::unexpected(ConnectionErrorInfo{
                    ConnectionError::kTlsKeyExportFailed,
                    std::format("TLS key export failed: {}", hot_state.error())});
            }

            size_t key_len = tls_->cipher_key_len();
            auto crypto = TlsRecordCrypto::create(*hot_state, key_len);
            if (!crypto) {
                return std::unexpected(ConnectionErrorInfo{
                    ConnectionError::kTlsKeyExportFailed,
                    std::format("TLS AEAD init failed: {}", crypto.error())});
            }
            crypto_ = std::make_unique<TlsRecordCrypto>(std::move(*crypto));

            // Capture TLS connection metadata
            tls_version_ = tls_->tls_version();
            cipher_name_ = tls_->cipher_name();
        } else {
            tls_version_ = "none";
            cipher_name_ = "none";
        }

        // Extract resolved IP if the TCP backend exposes it
        if constexpr (requires { tcp_->resolved_ip(); }) {
            remote_ip_ = std::string(tcp_->resolved_ip());
        }

        // Initialize pong timestamp to "now" so pong timeout doesn't fire
        // before the first ping/pong exchange completes.
        if constexpr (kIsWebSocket) {
            last_pong_ns_.store(
                std::chrono::steady_clock::now().time_since_epoch().count(),
                std::memory_order_relaxed);
            ping_awaiting_pong_ = false;
        }

        // Reset TLS sequence warning flags — fresh keys reset the counters
        seq_warning_logged_ = false;
        rx_seq_warning_logged_ = false;

        // Record handshake duration
        auto connect_end = std::chrono::steady_clock::now();
        last_handshake_ns_ = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                connect_end - connect_start).count());

        SPDLOG_LOGGER_INFO(log,
            "Connected: {} ({}, handshake: {:.1f}ms)",
            config_.remote_host,
            config_.use_tls
                ? std::format("TLS: {}, cipher: {}", tls_version_, cipher_name_)
                : std::string("plain WS"),
            static_cast<double>(last_handshake_ns_) / 1e6);
        return {};
    }

    /// Attempt reconnection with exponential backoff and jitter.
    /// Discards old SPSC queue data. Called from RX thread when disconnect
    /// is detected. Returns true if reconnection succeeded.
    ///
    /// Backoff schedule: base * 2^(attempt-1), capped at max_reconnect_backoff.
    /// Each delay is jittered by ±25% to avoid thundering herd.
    bool do_reconnect() {
        auto log = detail::transport_logger();
        int max_attempts = config_.max_reconnect_attempts;
        if (max_attempts <= 0) {
            SPDLOG_LOGGER_ERROR(log, "Auto-reconnect disabled, stopping");
            return false;
        }

        notify_state(TransportEvent::kDisconnected, config_.remote_host);

        // Record disconnect time for downtime measurement
        auto disconnect_time = std::chrono::steady_clock::now();

        // Signal TX thread to pause: it must not touch crypto_/tcp_
        // while we are reconnecting.
        reconnecting_.store(true, std::memory_order_release);

        // Discard stale queue data and fragment buffer
        if constexpr (kHasTxQueue) {
            tx_queue_.clear();
        }
        if constexpr (kIsWebSocket) {
            ws_frag_buf_.clear();
        }
        closing_.store(false, std::memory_order_release);

        // Compute backoff cap: explicit max, or 16x base as default
        auto base_ms = config_.reconnect_interval.count();
        auto max_backoff_ms = config_.max_reconnect_backoff.count();
        if (max_backoff_ms <= 0) {
            max_backoff_ms = base_ms * 16;
        }

        // Thread-local RNG for jitter (seeded from hardware entropy)
        thread_local std::mt19937 rng{std::random_device{}()};

        auto current_delay_ms = base_ms;

        for (int attempt = 1; attempt <= max_attempts; ++attempt) {
            // Apply ±25% jitter to current delay
            auto jitter_lo = current_delay_ms * 3 / 4;  // 75%
            auto jitter_hi = current_delay_ms * 5 / 4;  // 125%
            std::uniform_int_distribution<int64_t> dist(
                std::max(jitter_lo, int64_t{1}), std::max(jitter_hi, int64_t{1}));
            auto actual_delay_ms = dist(rng);

            SPDLOG_LOGGER_INFO(log,
                "Reconnect attempt {}/{} in {}ms (backoff: {}ms)",
                attempt, max_attempts, actual_delay_ms, current_delay_ms);

            notify_state(TransportEvent::kReconnecting,
                std::format("{}/{}", attempt, max_attempts));

            std::this_thread::sleep_for(
                std::chrono::milliseconds{actual_delay_ms});

            // Clean up old connection state
            crypto_.reset();
            tls_.reset();
            tcp_.reset();

            auto result = do_connect();
            if (result) {
                auto total = reconnect_count_.fetch_add(1, std::memory_order_relaxed) + 1;
                reconnecting_.store(false, std::memory_order_release);
                notify_state(TransportEvent::kConnected,
                    std::format("reconnect attempt {}", attempt));

                auto downtime_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - disconnect_time).count());

                SPDLOG_LOGGER_INFO(log,
                    "Reconnected successfully on attempt {} (downtime: {:.1f}ms)",
                    attempt, static_cast<double>(downtime_ns) / 1e6);

                // Notify application — ideal for replaying subscriptions
                if (config_.on_reconnected) {
                    try {
                        config_.on_reconnected(attempt, downtime_ns,
                            static_cast<uint64_t>(total));
                    } catch (...) {
                        SPDLOG_LOGGER_WARN(log,
                            "on_reconnected callback threw an exception");
                    }
                }
                return true;
            }

            SPDLOG_LOGGER_WARN(log,
                "Reconnect attempt {} failed: {}",
                attempt, result.error().message());

            // Let application decide whether to continue retrying.
            // Useful for aborting on non-transient errors (e.g., TLS
            // certificate rejection, HTTP 403).
            if (config_.on_reconnect_attempt) {
                try {
                    auto err_msg = result.error().message();
                    bool should_continue = config_.on_reconnect_attempt(
                        attempt, max_attempts, err_msg);
                    if (!should_continue) {
                        SPDLOG_LOGGER_INFO(log,
                            "Reconnect aborted by on_reconnect_attempt "
                            "callback after attempt {}", attempt);
                        break;
                    }
                } catch (...) {
                    SPDLOG_LOGGER_WARN(log,
                        "on_reconnect_attempt callback threw an exception");
                }
            }

            // Exponential backoff: double delay, capped at max
            current_delay_ms = std::min(current_delay_ms * 2, max_backoff_ms);
        }

        reconnecting_.store(false, std::memory_order_release);
        SPDLOG_LOGGER_ERROR(log,
            "All {} reconnect attempts exhausted", max_attempts);
        return false;
    }

    // -----------------------------------------------------------------------
    // WebSocket upgrade (Phase 3 of handshake)
    // -----------------------------------------------------------------------

    [[nodiscard]] std::expected<void, ConnectionErrorInfo> do_ws_upgrade() {
        auto log = detail::transport_logger();

        // Generate WebSocket key
        auto ws_key_result = http::generate_ws_key();
        if (!ws_key_result) {
            return std::unexpected(ConnectionErrorInfo{
                ConnectionError::kWsUpgradeFailed, ws_key_result.error()});
        }
        std::string ws_key = std::move(*ws_key_result);

        // Build upgrade request
        // RFC 6455 §4.1: Host header includes port only when non-default
        // (443 for wss://, 80 for ws://).
        std::string host = config_.remote_host;
        uint16_t default_port = config_.use_tls ? 443 : 80;
        if (config_.remote_port != default_port) {
            host += ":" + std::to_string(config_.remote_port);
        }

        // Build extra headers including subprotocol if configured
        std::string headers = config_.extra_headers;
        if (!config_.ws_subprotocol.empty()) {
            headers += std::format("Sec-WebSocket-Protocol: {}\r\n",
                                   config_.ws_subprotocol);
        }

        auto request_result = http::build_upgrade_request(
            host, config_.ws_path, ws_key, headers);
        if (!request_result) [[unlikely]] {
            return std::unexpected(ConnectionErrorInfo{
                ConnectionError::kWsUpgradeFailed,
                request_result.error()});
        }
        std::string request = std::move(*request_result);

        SPDLOG_LOGGER_DEBUG(log, "Sending WebSocket upgrade request ({})",
            config_.use_tls ? "TLS" : "plain TCP");

        // Send upgrade request through TLS or plain TCP
        if (config_.use_tls) {
            auto write_result = tls_->handshake_write(request.data(),
                                                        static_cast<int>(request.size()));
            if (!write_result || *write_result <= 0) {
                return std::unexpected(ConnectionErrorInfo{
                    ConnectionError::kWsUpgradeFailed,
                    "Failed to send WebSocket upgrade request"});
            }
        } else {
            auto write_result = tcp_->send(request.data(), request.size());
            if (!write_result) {
                return std::unexpected(ConnectionErrorInfo{
                    ConnectionError::kWsUpgradeFailed,
                    std::format("Failed to send WebSocket upgrade request: {}",
                                write_result.error())});
            }
        }

        // Read upgrade response (with timeout).
        // Cap buffer at 64KB to prevent unbounded allocation from
        // a misbehaving server sending oversized HTTP responses.
        static constexpr size_t kMaxUpgradeResponseSize = 65536;
        std::vector<uint8_t> response_buf;
        response_buf.reserve(4096);

        auto deadline = std::chrono::steady_clock::now() + config_.ws_timeout;

        while (std::chrono::steady_clock::now() < deadline) {
            uint8_t buf[4096];
            int bytes_read = 0;

            if (config_.use_tls) {
                auto read_result = tls_->handshake_read(buf, sizeof(buf));
                if (!read_result) {
                    return std::unexpected(ConnectionErrorInfo{
                        ConnectionError::kWsUpgradeFailed,
                        std::format("Failed to read upgrade response: {}",
                                    read_result.error())});
                }
                bytes_read = *read_result;
            } else {
                // Plain TCP: poll_rx with a short timeout to avoid busy-spin
                auto rx_result = tcp_->poll_rx(
                    [&](const uint8_t* data, uint16_t len) {
                        bytes_read = len;
                        std::memcpy(buf, data, std::min(static_cast<size_t>(len),
                                                         sizeof(buf)));
                    });
                if (!rx_result) {
                    return std::unexpected(ConnectionErrorInfo{
                        ConnectionError::kWsUpgradeFailed,
                        std::format("Failed to read upgrade response: {}",
                                    rx_result.error())});
                }
            }

            if (bytes_read > 0) {
                if (response_buf.size() + static_cast<size_t>(bytes_read) > kMaxUpgradeResponseSize) {
                    SPDLOG_LOGGER_ERROR(log,
                        "WebSocket upgrade response exceeds {}B limit",
                        kMaxUpgradeResponseSize);
                    return std::unexpected(ConnectionErrorInfo{
                        ConnectionError::kWsUpgradeFailed,
                        "WebSocket upgrade response too large"});
                }
                response_buf.insert(response_buf.end(),
                                    buf, buf + bytes_read);
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
                    return std::unexpected(ConnectionErrorInfo{
                        ConnectionError::kWsUpgradeFailed,
                        std::format("Failed to parse upgrade response: {}",
                                    parsed.error())});
                }

                if (parsed->status_code != 101) {
                    SPDLOG_LOGGER_ERROR(log,
                        "WebSocket upgrade rejected: status={}",
                        parsed->status_code);
                    return std::unexpected(ConnectionErrorInfo{
                        .code = ConnectionError::kWsUpgradeRejected,
                        .detail = std::format("WebSocket upgrade rejected (status {})",
                                    parsed->status_code),
                        .http_status = parsed->status_code});
                }

                if (!parsed->has_upgrade || !parsed->has_connection_upgrade) {
                    return std::unexpected(ConnectionErrorInfo{
                        ConnectionError::kWsUpgradeFailed,
                        "Missing Upgrade/Connection headers in response"});
                }

                // Validate Sec-WebSocket-Accept
                if (!http::validate_ws_accept(ws_key,
                                               parsed->sec_ws_accept)) {
                    SPDLOG_LOGGER_ERROR(log,
                        "Sec-WebSocket-Accept validation failed");
                    return std::unexpected(ConnectionErrorInfo{
                        ConnectionError::kWsAcceptInvalid,
                        "Sec-WebSocket-Accept validation failed"});
                }

                // Store negotiated subprotocol for user queries
                ws_subprotocol_ = std::move(parsed->sec_ws_protocol);

                SPDLOG_LOGGER_INFO(log, "WebSocket upgrade successful{}",
                    ws_subprotocol_.empty() ? ""
                        : std::format(" (subprotocol: {})", ws_subprotocol_));
                return {};
            }
        }

        return std::unexpected(ConnectionErrorInfo{
            ConnectionError::kWsUpgradeFailed,
            "WebSocket upgrade response timeout"});
    }
