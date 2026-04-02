// NOTE: Included inside Transport class body — do not include directly.

    // -----------------------------------------------------------------------
    // TX worker loop (runs on dedicated thread)
    // -----------------------------------------------------------------------

    void tx_loop() {
        (void)eph::utils::set_thread_affinity(config_.tx_cpu, "TX");
        auto log = detail::transport_logger();
        SPDLOG_LOGGER_DEBUG(log, "TX loop started");

        // Frame encode buffer: header overhead + payload + 1 byte for TLS content type append
        constexpr size_t kFrameOverhead = kIsWebSocket
            ? ws::kMaxFrameHeaderLen : Framer::max_overhead();
        constexpr size_t kWsBufSize = kFrameOverhead + MaxPayload + 1;
        // +1 byte: TLS encrypt() temporarily writes the TLS 1.3 inner content
        // type byte at plaintext[plaintext_len]. This extra byte is mandatory.
        static_assert(kWsBufSize > kFrameOverhead + MaxPayload,
                      "ws_buf must include +1 byte for TLS encrypt content type");
        // TLS output buffer: sized for the actual max frame (without the +1 temp byte)
        constexpr size_t kMaxWsFrame = kFrameOverhead + MaxPayload;
        constexpr size_t kTlsBufSize =
            TlsRecordCrypto::encrypted_size(
                static_cast<uint16_t>(kMaxWsFrame));

        // Batch buffers for drain loop (sized from config)
        const int kMaxBatch = config_.tx_burst_size;
        auto batch    = std::make_unique<TxMsg[]>(kMaxBatch);
        auto tls_bufs_storage = std::make_unique<uint8_t[]>(
            static_cast<size_t>(kMaxBatch) * kTlsBufSize);

        // Single frame encode buffer reused per message
        uint8_t ws_buf[kWsBufSize];

        // WS-specific: precomputed frame template for binary opcode fast path
        [[maybe_unused]] auto ws_tmpl = []() {
            if constexpr (kIsWebSocket) return ws::FrameTemplate::for_binary();
            else return 0; // unused placeholder
        }();

        // Generic framer instance (stateless for most framers)
        [[maybe_unused]] Framer framer_instance{};

        auto last_ping = std::chrono::steady_clock::now();

        while (running_.load(std::memory_order_acquire)) {
            // Spin-wait while RX thread is reconnecting to avoid
            // touching crypto_/tcp_ which are being replaced.
            // Also re-check running_ so stop() can terminate the TX thread
            // even during a prolonged reconnection attempt.
            if (reconnecting_.load(std::memory_order_acquire)) [[unlikely]] {
                if (!running_.load(std::memory_order_acquire)) break;
                eph::utils::cpu_relax();
                continue;
            }

            // Proactive TLS key refresh: warn at 90%, trigger reconnect at 95%.
            // This avoids hitting the hard limit where encrypt() returns 0
            // and messages are lost. Reconnect replaces keys with fresh ones.
            if (config_.use_tls) [[unlikely]] {
                // Guard against null crypto_ during reconnect window —
                // the RX thread may reset crypto_ between the reconnecting_
                // flag check above and this dereference.
                if (!crypto_) continue;
                // Double-check reconnecting_ after crypto_ null-check to minimize
                // the TOCTOU window. The sequence warning is non-critical stats —
                // a rare missed check is acceptable vs. adding a mutex on the hot path.
                if (reconnecting_.load(std::memory_order_acquire)) continue;
                // Recheck after reconnecting_ check: the RX thread may have
                // reset crypto_ between the first null-check and this point.
                if (!crypto_) continue;
                uint64_t seq = crypto_->write_seq();

                if (!seq_warning_logged_ && seq >= tls_record::kSequenceWarnThreshold) {
                    SPDLOG_LOGGER_WARN(log,
                        "TLS write sequence at {}/{} (90%%), "
                        "preemptive reconnect approaching",
                        seq, tls_record::kMaxSequenceNumber);
                    seq_warning_logged_ = true;
                }

                if (seq >= tls_record::kSequenceReconnectThreshold) {
                    SPDLOG_LOGGER_WARN(log,
                        "TLS write sequence at {}/{} (95%%), "
                        "triggering preemptive reconnect for key refresh",
                        seq, tls_record::kMaxSequenceNumber);
                    reconnecting_.store(true, std::memory_order_release);
                    tcp_->reset();
                    continue;
                }
            }

            // -- WebSocket ping / pong timeout (periodic keepalive, owned by TX thread) --
            // Only applicable when using WsFramer; other framers do not have
            // a ping/pong mechanism at the framing layer.
            if constexpr (kIsWebSocket) {
                if (config_.ping_interval.count() > 0) {
                    auto now = std::chrono::steady_clock::now();

                    // Check pong timeout before sending next ping.
                    // If we sent a ping and haven't received a pong within pong_timeout,
                    // the peer is considered dead — trigger reconnect via running_=false.
                    if (config_.pong_timeout.count() > 0 && ping_awaiting_pong_) {
                        auto last_pong_tp = SteadyTimePoint{
                            std::chrono::nanoseconds{
                                last_pong_ns_.load(std::memory_order_relaxed)}};
                        if (now - last_pong_tp > config_.pong_timeout) {
                            pong_timeouts_.fetch_add(1, std::memory_order_relaxed);
                            SPDLOG_LOGGER_WARN(log,
                                "Pong timeout: no pong received within {}s, "
                                "triggering reconnect",
                                config_.pong_timeout.count());
                            // Signal RX thread to reconnect by resetting TCP.
                            // RX will detect the broken connection and handle reconnect.
                            reconnecting_.store(true, std::memory_order_release);
                            tcp_->reset();
                            ping_awaiting_pong_ = false;
                            continue;
                        }
                    }

                    if (now - last_ping >= config_.ping_interval) {
                        if (send_ws_ping(ws_buf, tls_bufs_storage.get())) {
                            ping_awaiting_pong_ = true;
                        }
                        last_ping = now;
                    }
                }
            }

            // Drain: consume as many messages as available, up to kMaxBatch.
            // Uses try_consume_n for amortized atomic operations (single
            // head update for the entire batch vs one per message).
            int n = static_cast<int>(tx_queue_.try_consume_n(
                static_cast<size_t>(kMaxBatch),
                [&](const TxMsg& msg, [[maybe_unused]] size_t idx) {
                    batch[idx] = msg;
                }));

            if (n == 0) {
                // If RX signaled a graceful close and the queue is now
                // empty, the Close response has been sent — exit.
                if (closing_.load(std::memory_order_acquire)) [[unlikely]] {
                    SPDLOG_LOGGER_DEBUG(log,
                        "TX: closing_ set and queue drained, exiting");
                    running_.store(false, std::memory_order_release);
                    break;
                }
                eph::utils::cpu_relax();
                continue;
            }

            // Capture drain TSC once per batch — all messages share the same
            // drain point since try_consume_n is a single atomic operation.
            [[maybe_unused]] uint64_t drain_tsc = 0;
            if constexpr (kEnableTimestamps) {
                drain_tsc = eph::utils::TSC::now();
            }

            // WS encode -> [TLS encrypt] -> TCP send for each message in batch.
            // TLS mode: pack encrypted records contiguously for a single
            // TCP send, reducing syscall count from N to 1 per batch.
            size_t coalesced_len = 0;
            // Track batch stats locally, commit once after the loop.
            // For TLS: enables rollback on coalesced TCP send failure.
            // For plain WS: avoids per-message locked atomic operations.
            uint64_t batch_packets = 0;
            uint64_t batch_bytes = 0;
            uint64_t batch_text_packets = 0;
            uint64_t batch_text_bytes = 0;
            uint64_t batch_dropped = 0;

            for (int i = 0; i < n; ++i) {
                size_t ws_len;

                if constexpr (kIsWebSocket) {
                    // Use precomputed template for the common case (binary),
                    // fall back to encode_frame for other opcodes (text, pong)
                    // to ensure the correct opcode is written into the frame.
                    if (batch[i].opcode == ws::opcode::kBinary) {
                        ws_len = ws_tmpl.encode(
                            ws_buf, batch[i].data, batch[i].len);
                    } else {
                        ws_len = ws::encode_frame(
                            ws_buf, batch[i].opcode,
                            batch[i].data, batch[i].len);
                    }
                } else {
                    // Generic framer: encode payload into wire format
                    ws_len = framer_instance.encode(
                        ws_buf, batch[i].data, batch[i].len, batch[i].opcode);
                }

                // Record TSC for RTT measurement when a ping frame is
                // about to hit the wire (queued via send_ping() API).
                if constexpr (kIsWebSocket) {
                    if (batch[i].opcode == ws::opcode::kPing) {
                        last_ping_tsc_.store(eph::utils::TSC::now(),
                                             std::memory_order_relaxed);
                    }
                }

                // Per-message TX latency breakdown:
                //   total:      enqueue → flush (+ kernel TX if available)
                //   queue_wait: enqueue → drain (SPSC queue transit time)
                //   encode:     drain → flush (WS encode + TLS encrypt)
                if constexpr (kEnableTimestamps) {
                    uint64_t flush_tsc = eph::utils::TSC::now();
                    if (batch[i].tsc > 0 && flush_tsc > batch[i].tsc) {
                        // Total: enqueue → flush + kernel TX
                        auto total_ns = eph::utils::TSC::to_ns(flush_tsc - batch[i].tsc);
                        if (total_ns) {
                            uint64_t total = static_cast<uint64_t>(*total_ns);
                            if constexpr (requires { tcp_->last_kernel_tx_delay_ns(); }) {
                                total += tcp_->last_kernel_tx_delay_ns();
                            }
                            tx_latency_histogram_.record(total);
                        }
                        // Queue wait: enqueue → drain
                        if (drain_tsc > batch[i].tsc) {
                            auto qw_ns = eph::utils::TSC::to_ns(drain_tsc - batch[i].tsc);
                            if (qw_ns) tx_queue_wait_histogram_.record(
                                static_cast<uint64_t>(*qw_ns));
                        }
                        // Encode+encrypt: drain → flush
                        if (flush_tsc > drain_tsc) {
                            auto enc_ns = eph::utils::TSC::to_ns(flush_tsc - drain_tsc);
                            if (enc_ns) tx_encode_histogram_.record(
                                static_cast<uint64_t>(*enc_ns));
                        }
                    }
                }

                if (config_.use_tls) {
                    // Guard against null crypto_ during reconnect (M8).
                    if (!crypto_) break;
                    // Pack encrypted records contiguously into tls_bufs_storage
                    // so we can send the entire batch in a single TCP write.
                    uint8_t* tls_buf_i = tls_bufs_storage.get() + coalesced_len;
                    uint16_t enc_len = crypto_->encrypt(
                        ws_buf, static_cast<uint16_t>(ws_len),
                        tls_buf_i);

                    if (enc_len == 0) {
                        tx_stats_.crypto_errors.fetch_add(1, std::memory_order_relaxed);
                        // Sequence exhaustion: encrypt() returns 0 when
                        // write_seq >= kMaxSequenceNumber. Reconnect for fresh keys.
                        if (!crypto_) break;  // M8: recheck after failed encrypt
                        uint64_t wseq = crypto_->write_seq();
                        if (wseq >= tls_record::kMaxSequenceNumber) {
                            SPDLOG_LOGGER_ERROR(log,
                                "TLS write sequence exhausted ({}), "
                                "triggering reconnect for fresh keys", wseq);
                            reconnecting_.store(true, std::memory_order_release);
                            tcp_->reset();
                            break;
                        }
                    } else {
                        coalesced_len += enc_len;
                        batch_packets++;
                        batch_bytes += batch[i].len;
                        if (batch[i].opcode == ws::opcode::kText) {
                            batch_text_packets++;
                            batch_text_bytes += batch[i].len;
                        }
                    }
                } else {
                    // Plain WS: send WS frame directly over TCP.
                    // Accumulate stats locally and commit once after the loop
                    // to avoid per-message locked atomic operations.
                    auto result = tcp_->send(ws_buf, ws_len);
                    if (!result) {
                        batch_dropped++;
                        SPDLOG_LOGGER_WARN(log,
                            "TCP send failed (dropped): {}", result.error());
                    } else {
                        batch_packets++;
                        batch_bytes += batch[i].len;
                        if (batch[i].opcode == ws::opcode::kText) {
                            batch_text_packets++;
                            batch_text_bytes += batch[i].len;
                        }
                    }
                }
            }

            // Commit batch stats atomically (single set of fetch_add calls)
            if (config_.use_tls && coalesced_len > 0) {
                auto result = tcp_->send(tls_bufs_storage.get(), coalesced_len);
                if (!result) {
                    // All TLS records in this batch are lost
                    batch_dropped += batch_packets;
                    batch_packets = 0;
                    batch_bytes = 0;
                    batch_text_packets = 0;
                    batch_text_bytes = 0;
                    SPDLOG_LOGGER_WARN(log,
                        "Coalesced TCP send failed ({}B, {} records): {}",
                        coalesced_len, batch_dropped, result.error());
                }
            }
            if (batch_packets > 0) {
                tx_stats_.packets.fetch_add(batch_packets, std::memory_order_relaxed);
                tx_stats_.bytes.fetch_add(batch_bytes, std::memory_order_relaxed);
                if (batch_text_packets > 0) {
                    tx_stats_.text_packets.fetch_add(batch_text_packets, std::memory_order_relaxed);
                    tx_stats_.text_bytes.fetch_add(batch_text_bytes, std::memory_order_relaxed);
                }
            }
            if (batch_dropped > 0) {
                tx_stats_.dropped.fetch_add(batch_dropped, std::memory_order_relaxed);
            }
        }

        // Final drain: send any messages queued before stop() was called.
        // The main loop exited because running_=false; the application may
        // have enqueued messages right before calling stop().  One last
        // drain iteration sends them before the thread exits.
        // Skip if reconnecting (crypto_/tcp_ may be invalid).
        if (!reconnecting_.load(std::memory_order_acquire) &&
            tcp_ && tcp_->is_established()) {
            int remaining = static_cast<int>(tx_queue_.try_consume_n(
                static_cast<size_t>(kMaxBatch),
                [&](const TxMsg& msg, [[maybe_unused]] size_t idx) {
                    batch[idx] = msg;
                }));

            if (remaining > 0) {
                SPDLOG_LOGGER_DEBUG(log,
                    "TX: draining {} remaining messages before exit", remaining);

                size_t drain_coalesced = 0;
                for (int i = 0; i < remaining; ++i) {
                    size_t ws_len;
                    if constexpr (kIsWebSocket) {
                        if (batch[i].opcode == ws::opcode::kBinary) {
                            ws_len = ws_tmpl.encode(
                                ws_buf, batch[i].data, batch[i].len);
                        } else {
                            ws_len = ws::encode_frame(
                                ws_buf, batch[i].opcode,
                                batch[i].data, batch[i].len);
                        }
                    } else {
                        ws_len = framer_instance.encode(
                            ws_buf, batch[i].data, batch[i].len, batch[i].opcode);
                    }

                    auto account_drain_msg = [&](uint16_t len, uint8_t opcode) {
                        tx_stats_.packets.fetch_add(1, std::memory_order_relaxed);
                        tx_stats_.bytes.fetch_add(len, std::memory_order_relaxed);
                        if (opcode == ws::opcode::kText) {
                            tx_stats_.text_packets.fetch_add(1, std::memory_order_relaxed);
                            tx_stats_.text_bytes.fetch_add(len, std::memory_order_relaxed);
                        }
                    };

                    if (config_.use_tls) {
                        // Guard against null crypto_ during reconnect (M8).
                        if (!crypto_) break;
                        uint8_t* tls_buf_i = tls_bufs_storage.get() + drain_coalesced;
                        uint16_t enc_len = crypto_->encrypt(
                            ws_buf, static_cast<uint16_t>(ws_len), tls_buf_i);
                        if (enc_len > 0) {
                            drain_coalesced += enc_len;
                            account_drain_msg(batch[i].len, batch[i].opcode);
                        }
                    } else {
                        auto result = tcp_->send(ws_buf, ws_len);
                        if (result) {
                            account_drain_msg(batch[i].len, batch[i].opcode);
                        } else {
                            tx_stats_.dropped.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }

                if (config_.use_tls && drain_coalesced > 0) {
                    (void)tcp_->send(tls_bufs_storage.get(), drain_coalesced);
                }
            }
        }

        SPDLOG_LOGGER_DEBUG(log, "TX loop exited");
    }

    /// Send a WebSocket ping frame (called from TX thread only).
    /// Uses caller-provided buffers to avoid extra stack allocations.
    /// Records TSC timestamp for RTT measurement by the RX thread.
    bool send_ws_ping(uint8_t* ws_buf, uint8_t* tls_buf) noexcept {
        size_t ping_len = ws::build_ping_frame(ws_buf);

        // Record TSC just before TCP send for tightest RTT measurement
        last_ping_tsc_.store(eph::utils::TSC::now(), std::memory_order_relaxed);

        if (config_.use_tls) {
            // Guard against null crypto_ during reconnect (M8).
            if (!crypto_) return false;
            uint16_t tls_len = crypto_->encrypt(
                ws_buf, static_cast<uint16_t>(ping_len), tls_buf);
            if (tls_len == 0) return false;

            auto result = tcp_->send(tls_buf, tls_len);
            if (!result) {
                SPDLOG_LOGGER_DEBUG(detail::transport_logger(),
                    "WS ping send failed: {}", result.error());
                return false;
            }
        } else {
            auto result = tcp_->send(ws_buf, ping_len);
            if (!result) {
                SPDLOG_LOGGER_DEBUG(detail::transport_logger(),
                    "WS ping send failed: {}", result.error());
                return false;
            }
        }
        return true;
    }
