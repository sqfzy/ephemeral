// NOTE: Included inside Transport class body — do not include directly.

    // -----------------------------------------------------------------------
    // RX worker loop (runs on dedicated thread)
    // -----------------------------------------------------------------------

    void rx_loop() {
        (void)eph::utils::set_thread_affinity(config_.rx_cpu, "RX");
        auto log = detail::transport_logger();
        SPDLOG_LOGGER_DEBUG(log, "RX loop started");

        // Cache TSC conversion factor once — avoids per-frame acquire
        // load on TSC::initialized_ inside the hot loop.
        if constexpr (kEnableTimestamps) {
            auto npc = eph::utils::TSC::get_ns_per_cycle();
            ns_per_cycle_ = npc.value_or(0.0);
        }

        // Fixed-size RX buffers -- no heap allocation on hot path.
        // TLS reassembly: accumulates raw TCP bytes until complete TLS records form.
        // Sized for 4x max TLS record to handle burst TCP delivery under high load.
        // Previous 2x sizing caused overflow when multiple segments arrived between
        // processing cycles (e.g., 31661 + 1460 > 32812 with MSS=1460).
        static constexpr size_t kReassemblyBufSize =
            4 * (tls_const::kMaxRecordPayload + tls_record::kRecordHeaderLen +
                 tls_record::kAuthTagLen + 1);
        auto decrypt_buf = std::make_unique<uint8_t[]>(
            tls_const::kMaxRecordPayload + 256);
        auto reassembly_storage = std::make_unique<uint8_t[]>(kReassemblyBufSize);
        size_t reassembly_len = 0;

        // Frame reassembly: accumulates decrypted bytes when a framed message
        // spans multiple TLS records. Without this, partial frames at
        // TLS record boundaries would be silently discarded.
        static constexpr size_t kFrameReassemblyOverhead = kIsWebSocket
            ? ws::kMaxFrameHeaderLen : Framer::max_overhead();
        static constexpr size_t kWsReassemblyBufSize =
            kFrameReassemblyOverhead + MaxPayload + 256;
        auto ws_reassembly_storage = std::make_unique<uint8_t[]>(kWsReassemblyBufSize);
        size_t ws_reassembly_len = 0;

        while (running_.load(std::memory_order_acquire)) {
            // After a server Close frame, stop receiving — TX will
            // drain the Close response and set running_=false.
            if (closing_.load(std::memory_order_acquire)) [[unlikely]] {
                eph::utils::cpu_relax();
                continue;
            }

            // Application requested forced reconnect via reconnect_now()
            if (force_reconnect_.exchange(false, std::memory_order_acq_rel)) [[unlikely]] {
                SPDLOG_LOGGER_INFO(log, "Processing forced reconnect request");
                reassembly_len = 0;
                ws_reassembly_len = 0;
                if (!do_reconnect()) {
                    running_.store(false, std::memory_order_release);
                    break;
                }
                continue;
            }

            // -- Receive data via poll_rx --
            bool reconnect_needed = false;
            auto rx_result = tcp_->poll_rx(
                [&](const uint8_t* data, uint16_t len) {
                    if (config_.use_tls) {
                        // TLS mode: accumulate into TLS reassembly buffer
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
                            ws_reassembly_len = 0;
                            reconnect_needed = true;
                        }
                    } else {
                        // Plain WS mode: accumulate into WS reassembly buffer
                        if (ws_reassembly_len + len <= kWsReassemblyBufSize) {
                            std::memcpy(ws_reassembly_storage.get() + ws_reassembly_len,
                                        data, len);
                            ws_reassembly_len += len;
                        } else {
                            SPDLOG_LOGGER_ERROR(log,
                                "WS reassembly buffer overflow ({} + {} > {}), "
                                "triggering reconnect",
                                ws_reassembly_len, len, kWsReassemblyBufSize);
                            ws_reassembly_len = 0;
                            reconnect_needed = true;
                        }
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
                ws_reassembly_len = 0;
                if (do_reconnect()) {
                    continue; // Resume RX loop with new connection
                }

                // Reconnect exhausted -- stop transport
                running_.store(false, std::memory_order_release);
                break;
            }

            // No data received this poll iteration
            if (*rx_result == 0) continue;

            // Set arrival TSC to NIC-arrival time.  Start from the
            // rx_burst TSC (captured right after recvmsg / rte_eth_rx_burst),
            // then back-date by kernel stack delay (SO_TIMESTAMPING) so
            // that all downstream consumers — record_rx_latency(),
            // RxMsg.tsc, app-layer recv(data,len,opcode,tsc) — measure
            // from the same NIC-arrival baseline.
            // DPDK has no kernel delay; the `requires` guard compiles away.
            if constexpr (kEnableTimestamps) {
                current_arrival_tsc_ = tcp_->last_rx_burst_tsc();
                if constexpr (requires { tcp_->last_kernel_rx_delay_ns(); }) {
                    uint64_t delay_ns = tcp_->last_kernel_rx_delay_ns();
                    if (delay_ns > 0 && ns_per_cycle_ > 0) {
                        uint64_t delay_cycles = static_cast<uint64_t>(
                            delay_ns / ns_per_cycle_);
                        if (delay_cycles >= current_arrival_tsc_) {
                            // Kernel delay is larger than the burst TSC value,
                            // which indicates a stale or invalid timestamp.
                            // Zero out rather than wrap-around to avoid stale
                            // burst TSC propagating to downstream latency calcs.
                            current_arrival_tsc_ = 0;
                        } else {
                            current_arrival_tsc_ -= delay_cycles;
                        }
                    }
                }
            }

            // Plain mode: process framed data directly from TCP
            if (!config_.use_tls) {
                // Use reassembly buffer to handle partial frames
                size_t ws_consumed = process_frame_data(
                    ws_reassembly_storage.get(), ws_reassembly_len);

                // Save unconsumed WS bytes for next TCP chunk
                // Clamp to prevent underflow if process_frame_data returns more
                // than available (defensive — should not happen in practice).
                ws_consumed = std::min(ws_consumed, ws_reassembly_len);
                size_t ws_remaining = ws_reassembly_len - ws_consumed;
                if (ws_remaining > 0 && ws_consumed > 0) {
                    std::memmove(ws_reassembly_storage.get(),
                                 ws_reassembly_storage.get() + ws_consumed,
                                 ws_remaining);
                }
                ws_reassembly_len = ws_remaining;
                if constexpr (requires { tcp_->flush_pending_ack(); }) {
                    tcp_->flush_pending_ack();
                }
                continue;
            }

            // Proactive warning at 90% of TLS read sequence limit,
            // symmetric with the TX thread's write sequence check.
            if (!rx_seq_warning_logged_) [[likely]] {
                uint64_t rseq = crypto_->read_seq();
                if (rseq >= tls_record::kSequenceWarnThreshold) [[unlikely]] {
                    SPDLOG_LOGGER_WARN(log,
                        "TLS read sequence at {}/{} (90%%), "
                        "reconnect imminent for key refresh",
                        rseq, tls_record::kMaxSequenceNumber);
                    rx_seq_warning_logged_ = true;
                }
            }

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
                    rx_stats_.crypto_errors.fetch_add(1, std::memory_order_relaxed);
                    SPDLOG_LOGGER_WARN(log,
                        "TLS decrypt failed -- triggering reconnect");
                    // Corrupted record -> link unreliable, reconnect.
                    // Reset reassembly state and skip compact logic below.
                    reassembly_len = 0;
                    ws_reassembly_len = 0;
                    consumed = 0;
                    if (!do_reconnect()) {
                        running_.store(false, std::memory_order_release);
                    }
                    break; // Resume with fresh connection or exit outer loop
                }

                // Prepend any leftover WS bytes from the previous TLS record.
                // This handles WS frames that span TLS record boundaries.
                const uint8_t* ws_data;
                size_t ws_data_len;
                if (ws_reassembly_len > 0) {
                    // Append new decrypted data after existing WS leftovers
                    if (ws_reassembly_len + decrypted_len <= kWsReassemblyBufSize) {
                        std::memcpy(ws_reassembly_storage.get() + ws_reassembly_len,
                                    decrypt_buf.get(), decrypted_len);
                        ws_reassembly_len += decrypted_len;
                    } else {
                        SPDLOG_LOGGER_WARN(log,
                            "WS reassembly buffer overflow ({} + {} > {}), "
                            "discarding partial frame",
                            ws_reassembly_len, decrypted_len,
                            kWsReassemblyBufSize);
                        ws_reassembly_len = 0;
                        consumed += record_total;
                        continue;
                    }
                    ws_data = ws_reassembly_storage.get();
                    ws_data_len = ws_reassembly_len;
                } else {
                    ws_data = decrypt_buf.get();
                    ws_data_len = decrypted_len;
                }

                // Capture TSC after TLS decrypt, before WS decode.
                if constexpr (kEnableTimestamps) {
                    current_decrypt_done_tsc_ = eph::utils::TSC::now();
                }

                size_t ws_consumed = process_frame_data(ws_data, ws_data_len);

                // Save unconsumed WS bytes for next TLS record
                ws_consumed = std::min(ws_consumed, ws_data_len);
                size_t ws_remaining = ws_data_len - ws_consumed;
                if (ws_remaining > 0) {
                    if (ws_data == decrypt_buf.get()) {
                        // First time: copy leftovers into WS reassembly buffer
                        std::memcpy(ws_reassembly_storage.get(),
                                    decrypt_buf.get() + ws_consumed, ws_remaining);
                    } else {
                        // Already in WS reassembly buffer: compact to front
                        std::memmove(ws_reassembly_storage.get(),
                                     ws_reassembly_storage.get() + ws_consumed,
                                     ws_remaining);
                    }
                    ws_reassembly_len = ws_remaining;
                } else {
                    ws_reassembly_len = 0;
                }

                consumed += record_total;
            }

            // After decrypt failure + failed reconnect, running_ is false.
            // Break immediately to avoid flush/compact on dead connection.
            if (!running_.load(std::memory_order_acquire)) break;

            // Flush deferred ACK after TLS decrypt completes, keeping
            // the ACK's rte_eth_tx_burst off the RX latency measurement path.
            if constexpr (requires { tcp_->flush_pending_ack(); }) {
                tcp_->flush_pending_ack();
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

    // record_rx_latency() is defined in transport_frame.hpp (shared by all modes)
