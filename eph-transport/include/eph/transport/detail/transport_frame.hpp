// NOTE: Included inside Transport class body — do not include directly.

    // -----------------------------------------------------------------------
    // RX latency recording (shared by all modes)
    // -----------------------------------------------------------------------

    /// Record per-message RX latency breakdown:
    ///   total:   NIC arrival → decoded
    ///   decrypt: NIC arrival → decrypt_done (TLS decryption)
    ///   decode:  decrypt_done → decoded (WS frame parsing)
    ///
    /// current_arrival_tsc_ is already back-dated to NIC arrival time
    /// (kernel stack delay subtracted at poll_rx), so no additional
    /// kernel delay adjustment is needed here.
    void record_rx_latency() noexcept {
        if constexpr (kEnableTimestamps) {
            uint64_t now_tsc = eph::utils::TSC::now();
            if (current_arrival_tsc_ > 0 && now_tsc > current_arrival_tsc_) {
                auto cycles_to_ns = [this](uint64_t c) -> uint64_t {
                    return static_cast<uint64_t>(static_cast<double>(c) * ns_per_cycle_);
                };

                // Total: NIC arrival → decoded
                uint64_t total = cycles_to_ns(now_tsc - current_arrival_tsc_);
                rx_latency_histogram_.record(total);

                // Decrypt: arrival → decrypt_done (TLS only)
                if (current_decrypt_done_tsc_ > current_arrival_tsc_) {
                    rx_decrypt_histogram_.record(
                        cycles_to_ns(current_decrypt_done_tsc_ - current_arrival_tsc_));
                }
                // Decode: decrypt_done → now (WS frame parsing)
                if (current_decrypt_done_tsc_ > 0 && now_tsc > current_decrypt_done_tsc_) {
                    rx_decode_histogram_.record(
                        cycles_to_ns(now_tsc - current_decrypt_done_tsc_));
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // WebSocket frame processing
    // -----------------------------------------------------------------------

    /// Process decrypted framed data. Returns the number of bytes
    /// consumed. Unconsumed bytes (partial frames) must be preserved
    /// by the caller and prepended to the next chunk of decrypted data.
    /// For WsFramer: handles WS control frames (ping/pong/close) and
    /// fragmentation reassembly. For generic framers: simple decode loop.
    size_t process_frame_data(const uint8_t* data, size_t len) {
        if constexpr (kIsWebSocket) {
            return process_ws_data(data, len);
        } else {
            return process_generic_data(data, len);
        }
    }

    /// Process data using a generic (non-WS) framer. Simple decode loop
    /// that delivers each successfully decoded frame's payload directly.
    size_t process_generic_data(const uint8_t* data, size_t len) {
        auto log = detail::transport_logger();
        size_t offset = 0;

        while (offset < len) {
            auto frame = rx_framer_.decode(data + offset, len - offset);
            if (!frame) {
                if (frame.error() == FrameError::kIncomplete) break;
                SPDLOG_LOGGER_WARN(log, "Frame decode error: {}",
                                   frame_error_name(frame.error()));
                break;
            }

            offset += frame->total_len;
            rx_stats_.packets.fetch_add(1, std::memory_order_relaxed);
            record_rx_latency();

            // Deliver payload directly (no control frame handling for non-WS)
            if (frame->payload_len > 0 && frame->payload_len <= MaxPayload) {
                deliver_message(frame->payload,
                               static_cast<uint16_t>(frame->payload_len),
                               frame->msg_type);
            } else if (frame->payload_len > MaxPayload) {
                rx_stats_.dropped.fetch_add(1, std::memory_order_relaxed);
                SPDLOG_LOGGER_WARN(log,
                    "Dropping oversized frame: payload_len={}, max={}",
                    frame->payload_len, MaxPayload);
            }
        }
        return offset;
    }

    /// Process decrypted WebSocket data (WsFramer only). Returns the number
    /// of bytes consumed.
    size_t process_ws_data(const uint8_t* data, size_t len) {
        // Batch frame filter: route to filtered path when configured.
        if (config_.on_frame_filter) {
            return process_ws_data_filtered(data, len);
        }

        auto log = detail::transport_logger();
        size_t offset = 0;

        // EvictingQueue last-only optimization: when the app only reads
        // the latest value, intermediate data frames are wasted work
        // (stats atomics, latency histogram, UTF-8 check, memcpy,
        // enqueue — all overwritten immediately). Instead, we decode
        // all frames but only deliver the last data frame per call.
        // Control frames (ping/close/pong) are always handled immediately.
        // BoundedQueue mode: every frame delivered as before.
        [[maybe_unused]] const ws::DecodedFrame* last_data_frame = nullptr;
        [[maybe_unused]] uint64_t data_frame_count = 0;
        // Accumulate byte-level stats locally to avoid per-frame atomics.
        // Flushed once after the loop — saves ~15ns/frame on hot path.
        uint64_t batch_bytes = 0;
        uint64_t batch_text_bytes = 0;
        uint64_t batch_text_packets = 0;

        // Pre-compute whether we can use the direct rx_enqueue fast path,
        // bypassing deliver_data_frame → deliver_message overhead.
        // Requires: no UTF-8 validation, no on_message callback.
        const bool fast_deliver = config_.skip_utf8_validation && !config_.on_message;

        while (offset < len) {
            auto frame = ws::decode_frame(data + offset, len - offset);
            if (!frame) {
                if (frame.error() == ws::DecodeError::kIncomplete) break;
                SPDLOG_LOGGER_WARN(log, "WS frame decode error: {}",
                                   ws::decode_error_name(frame.error()));
                break;
            }

            offset += frame->total_len;

            // Stats + latency are batched after the loop for BOTH modes.
            // Per-frame TSC::now() + 3 histogram writes was ~100ns/frame,
            // which inflated the very latency we measure.  Recording once
            // at the end captures worst-case (last frame) latency without
            // the measurement itself contaminating the result.

            if (frame->is_ping()) [[unlikely]] {
                rx_stats_.packets.fetch_add(1, std::memory_order_relaxed);
                ws_pings_received_.fetch_add(1, std::memory_order_relaxed);
                if (config_.on_ping) {
                    try {
                        config_.on_ping(frame->payload,
                                        static_cast<uint16_t>(frame->payload_len));
                    } catch (...) {
                        SPDLOG_LOGGER_WARN(log,
                            "on_ping callback threw an exception");
                    }
                }
                handle_ping(*frame);
                continue;
            }

            if (frame->is_close()) [[unlikely]] {
                rx_stats_.packets.fetch_add(1, std::memory_order_relaxed);
                uint16_t code = frame->close_status_code();
                std::string_view close_reason = frame->close_reason();
                SPDLOG_LOGGER_INFO(log,
                    "Received WS Close frame: code={} reason=\"{}\"",
                    code, close_reason);
                // Notify application of close reason before responding
                if (config_.on_close) {
                    try {
                        config_.on_close(code, close_reason);
                    } catch (...) {
                        SPDLOG_LOGGER_WARN(log,
                            "on_close callback threw an exception");
                    }
                }
                // Deliver close frame to RX queue so polling-mode users
                // can detect server-initiated close via try_recv_msg().
                // The close payload (2-byte code + optional reason) is
                // accessible via ReceivedMessage::close_code()/close_reason().
                if (frame->payload && frame->payload_len > 0 &&
                    frame->payload_len <= MaxPayload) {
                    if constexpr (kHasRxQueue) {
                        (void)rx_enqueue(
                            frame->payload,
                            static_cast<uint16_t>(frame->payload_len),
                            ws::opcode::kClose);
                    } else if (config_.on_close) {
                        // kDirect: already delivered via on_close above
                    }
                }
                // RFC 6455 §5.5.1: respond with a Close frame echoing
                // the status code before shutting down.
                handle_close(code);
                // Signal TX to drain the Close response before exiting.
                // TX checks closing_ and sends remaining queue items.
                closing_.store(true, std::memory_order_release);
                break;
            }

            if (frame->is_pong()) [[unlikely]] {
                rx_stats_.packets.fetch_add(1, std::memory_order_relaxed);
                record_rx_latency();
                // Record pong arrival for timeout detection (TX thread reads this).
                last_pong_ns_.store(
                    std::chrono::steady_clock::now().time_since_epoch().count(),
                    std::memory_order_relaxed);

                // RTT measurement: compute delta from the ping TSC timestamp.
                // Includes kernel TX+RX stack delays when available for full-path RTT.
                uint64_t ping_tsc = last_ping_tsc_.load(std::memory_order_relaxed);
                if (ping_tsc > 0 && eph::utils::TSC::is_initialized()) {
                    uint64_t pong_tsc = eph::utils::TSC::now();
                    if (pong_tsc > ping_tsc) {
                        auto rtt_ns = eph::utils::TSC::to_ns(pong_tsc - ping_tsc);
                        if (rtt_ns) {
                            uint64_t total = static_cast<uint64_t>(*rtt_ns);
                            // Add kernel stack delays for Socket backend fairness
                            if constexpr (kEnableTimestamps) {
                                if constexpr (requires { tcp_->last_kernel_tx_delay_ns(); }) {
                                    total += tcp_->last_kernel_tx_delay_ns();
                                }
                                if constexpr (requires { tcp_->last_kernel_rx_delay_ns(); }) {
                                    total += tcp_->last_kernel_rx_delay_ns();
                                }
                            }
                            rtt_histogram_.record(total);
                        }
                    }
                    // Clear ping TSC so we don't double-record on spurious pongs
                    last_ping_tsc_.store(0, std::memory_order_relaxed);
                }

                if (config_.on_pong) {
                    try {
                        config_.on_pong(frame->payload,
                                        static_cast<uint16_t>(frame->payload_len));
                    } catch (...) {
                        SPDLOG_LOGGER_WARN(detail::transport_logger(),
                            "on_pong callback threw an exception");
                    }
                }
                continue;
            }

            // Data frame handling with fragmentation reassembly.
            // RFC 6455 §5.4: first fragment has opcode != 0, FIN=0;
            // continuation fragments have opcode=0; final fragment has FIN=1.
            if (!frame->is_data()) [[unlikely]] continue;

            // Unmask payload in-place if needed (server frames are usually
            // unmasked, but handle masked frames for robustness).
            // payload pointer is const; we'll unmask during copy below.

            if (frame->opcode != ws::opcode::kContinuation) {
                // Start of a new message (possibly the only frame if FIN=1)
                if (!ws_frag_buf_.empty()) {
                    SPDLOG_LOGGER_WARN(log,
                        "New WS message started while previous fragment "
                        "incomplete, discarding {} buffered bytes",
                        ws_frag_buf_.size());
                    ws_frag_buf_.clear();
                }
                ws_frag_opcode_ = frame->opcode;
            }

            // Append payload to fragment buffer (or process directly if
            // single-frame message).
            bool is_final = frame->fin;
            bool is_single_frame = (frame->opcode != ws::opcode::kContinuation
                                    && is_final);

            if (is_single_frame && frame->payload_len <= MaxPayload) {
                // Fast path: complete single-frame message, no buffering.
                if constexpr (kLastOnlyDeliver) {
                    last_data_frame = &*frame;
                } else {
                    // Hot-path: bypass deliver_data_frame → deliver_message
                    // chain when conditions allow direct enqueue.
                    // Server frames are unmasked; skip_utf8 is checked once;
                    // on_message is checked once; defer_stats is always true here.
                    //
                    // Per-frame record_rx_latency: capture TSC at decode
                    // completion (before enqueue) so each frame measures
                    // NIC arrival → its own decode, not the cumulative
                    // cost of all frames in the batch.
                    record_rx_latency();
                    if constexpr (kHasRxQueue) {
                        if (fast_deliver && !frame->masked && frame->payload_len > 0)
                            [[likely]] {
                            (void)rx_enqueue(frame->payload,
                                static_cast<uint16_t>(frame->payload_len),
                                frame->opcode);
                        } else {
                            deliver_data_frame(*frame, /*defer_stats=*/true);
                        }
                    } else {
                        deliver_data_frame(*frame, /*defer_stats=*/true);
                    }
                    batch_bytes += frame->payload_len;
                    if (frame->opcode == ws::opcode::kText) {
                        batch_text_bytes += frame->payload_len;
                        ++batch_text_packets;
                    }
                }
                ++data_frame_count;
            } else if (is_single_frame) {
                // Single oversized frame
                rx_stats_.dropped.fetch_add(1, std::memory_order_relaxed);
                SPDLOG_LOGGER_WARN(log,
                    "Dropping oversized WS frame: payload_len={}, "
                    "max={}, opcode=0x{:02x}",
                    frame->payload_len, MaxPayload, frame->opcode);
            } else {
                // Fragmented message: accumulate
                size_t new_size = ws_frag_buf_.size() + frame->payload_len;
                if (new_size > MaxPayload) {
                    rx_stats_.dropped.fetch_add(1, std::memory_order_relaxed);
                    SPDLOG_LOGGER_WARN(log,
                        "Dropping oversized fragmented WS message: "
                        "accumulated={}, max={}", new_size, MaxPayload);
                    ws_frag_buf_.clear();
                    continue;
                }

                if (frame->payload && frame->payload_len > 0) {
                    size_t old_size = ws_frag_buf_.size();
                    ws_frag_buf_.resize(new_size);
                    std::memcpy(ws_frag_buf_.data() + old_size,
                                frame->payload, frame->payload_len);
                    if (frame->masked) {
                        ws::apply_mask(
                            ws_frag_buf_.data() + old_size,
                            frame->payload_len, frame->mask_key);
                    }
                }

                if (is_final) {
                    // Reassembly complete — deliver
                    if (!ws_frag_buf_.empty()) {
                        ++data_frame_count;
                        auto frag_len = static_cast<uint16_t>(ws_frag_buf_.size());
                        if constexpr (kLastOnlyDeliver) {
                            // Cannot defer fragmented frames (buffer reused
                            // next iteration), so deliver immediately.
                            // If a later single-frame overwrites last_data_frame,
                            // that's fine — this one is already delivered.
                            last_data_frame = nullptr;
                        }
                        deliver_message(
                            ws_frag_buf_.data(), frag_len,
                            ws_frag_opcode_, /*defer_stats=*/true);
                        batch_bytes += frag_len;
                        if (ws_frag_opcode_ == ws::opcode::kText) {
                            batch_text_bytes += frag_len;
                            ++batch_text_packets;
                        }
                    }
                    ws_frag_buf_.clear();
                }
            }
        }

        // Batch stats + latency for all data frames decoded in this call.
        if (data_frame_count > 0) {
            // For LastOnlyDeliver=true, record latency once per batch
            // (all frames decoded, deliver only the last one).
            // For LastOnlyDeliver=false, per-frame record_rx_latency
            // was already called in the decode loop above.
            if constexpr (kLastOnlyDeliver) {
                record_rx_latency();
            }
            rx_stats_.packets.fetch_add(data_frame_count,
                                        std::memory_order_relaxed);
            if (batch_bytes > 0) {
                rx_stats_.bytes.fetch_add(batch_bytes, std::memory_order_relaxed);
            }
            if (batch_text_packets > 0) {
                rx_stats_.text_packets.fetch_add(batch_text_packets,
                                                 std::memory_order_relaxed);
                rx_stats_.text_bytes.fetch_add(batch_text_bytes,
                                               std::memory_order_relaxed);
            }
            if constexpr (kLastOnlyDeliver) {
                if (last_data_frame) {
                    deliver_data_frame(*last_data_frame);
                }
            }
        }

        return offset;
    }

    /// Deliver a decoded payload to either the on_message callback or the RX queue.
    /// Text frames are validated for UTF-8 compliance (RFC 6455 §5.6) unless
    /// TransportConfig::skip_utf8_validation is true.
    /// @param defer_stats  When true, skip per-message byte/text stats
    ///   updates (caller will batch-flush them after the decode loop).
    ///   All other logic (UTF-8 check, on_message, drop handling) is
    ///   preserved regardless.
    void deliver_message(const uint8_t* data, uint16_t len, uint8_t opcode,
                         bool defer_stats = false) noexcept {
        // RFC 6455 §5.6: text frames must contain valid UTF-8
        if (opcode == ws::opcode::kText && !config_.skip_utf8_validation &&
            !ws::is_valid_utf8(data, len)) {
            rx_stats_.dropped.fetch_add(1, std::memory_order_relaxed);
            SPDLOG_LOGGER_WARN(detail::transport_logger(),
                "Dropping text frame with invalid UTF-8 (len={})", len);
            return;
        }
        auto update_rx_stats = [&] {
            if (defer_stats) return;
            rx_stats_.bytes.fetch_add(len, std::memory_order_relaxed);
            if (opcode == ws::opcode::kText) {
                rx_stats_.text_packets.fetch_add(1, std::memory_order_relaxed);
                rx_stats_.text_bytes.fetch_add(len, std::memory_order_relaxed);
            }
        };

        if (config_.on_message) {
            try {
                config_.on_message(data, len, opcode);
            } catch (...) {
                SPDLOG_LOGGER_WARN(detail::transport_logger(),
                    "on_message callback threw an exception");
            }
            update_rx_stats();
            return;
        }

        if constexpr (!kHasRxQueue) {
            // kDirect mode: no RX queue and no on_message — drop silently.
            SPDLOG_LOGGER_WARN(detail::transport_logger(),
                "kDirect mode: no on_message callback and no RX queue, "
                "dropping frame (len={})", len);
            rx_stats_.dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        } else {

        bool ok = rx_enqueue(data, len, opcode);

        if (ok) {
            update_rx_stats();
            // Sample RX HWM every 64 deliveries (same rationale as TX)
            if ((++rx_hwm_counter_ & 63) == 0) {
                update_hwm(rx_hwm_, rx_size());
            }
        } else {
            auto total = rx_stats_.dropped.fetch_add(1, std::memory_order_relaxed) + 1;
            if (config_.drop_log_interval > 0 &&
                total % config_.drop_log_interval == 1) {
                SPDLOG_LOGGER_WARN(detail::transport_logger(),
                    "RX queue full, dropping data frame "
                    "(total dropped: {})", total);
            }
            if (config_.on_rx_drop) {
                try {
                    config_.on_rx_drop(total);
                } catch (...) {
                    // Callback must not throw
                }
            }
        }

        } // else (kHasRxQueue)
    }

    /// Deliver a complete single-frame data message.
    void deliver_data_frame(const ws::DecodedFrame& frame,
                            bool defer_stats = false) noexcept {
        if (frame.payload_len == 0) return;
        if (frame.payload_len > MaxPayload) [[unlikely]] return;

        // For masked frames, unmask into a temp buffer before delivery
        if (frame.masked) {
            uint8_t tmp[MaxPayload];
            std::memcpy(tmp, frame.payload, frame.payload_len);
            ws::apply_mask(tmp, frame.payload_len, frame.mask_key);
            deliver_message(tmp, static_cast<uint16_t>(frame.payload_len),
                            frame.opcode, defer_stats);
        } else {
            deliver_message(frame.payload, static_cast<uint16_t>(frame.payload_len),
                            frame.opcode, defer_stats);
        }
    }

    // -----------------------------------------------------------------------
    // Symbol-aware dedup: process_ws_data with per-symbol latest-only delivery
    // -----------------------------------------------------------------------

    /// Lightweight frame index entry for the symbol-dedup scan pass.
    /// Stored on the stack (max ~128 frames per TLS record).
    struct FrameIndexEntry {
        size_t         offset;       // byte offset in source buffer
        size_t         total_len;    // total WS frame length
        uint8_t        opcode;
        bool           fin;
        bool           masked;
        uint32_t       mask_key;
        const uint8_t* payload;
        uint64_t       payload_len;
        bool           is_control;
    };

    /// Process WS data with batch frame filter.
    /// Phase 1: forward-scan WS headers, build index + FrameView in single pass.
    /// Phase 2: call filter on FrameView[].
    /// Phase 3: dispatch control frames immediately + data frames per filter.
    size_t process_ws_data_filtered(const uint8_t* data, size_t len) {
        auto log = detail::transport_logger();

        // ── Phase 1: single-pass scan ───────────────────────────────────
        // Build frame index (for dispatch) and FrameView (for filter)
        // simultaneously to avoid redundant iteration.
        static constexpr size_t kMaxFramesPerBatch = 128;
        FrameIndexEntry index[kMaxFramesPerBatch];
        FrameView views[kMaxFramesPerBatch];
        size_t    view_to_frame[kMaxFramesPerBatch]; // view idx → frame idx
        size_t num_frames = 0;
        size_t num_views = 0;
        size_t offset = 0;

        while (offset < len && num_frames < kMaxFramesPerBatch) {
            auto frame = ws::decode_frame(data + offset, len - offset);
            if (!frame) {
                if (frame.error() == ws::DecodeError::kIncomplete) break;
                SPDLOG_LOGGER_WARN(log, "WS frame decode error in filter scan: {}",
                                   ws::decode_error_name(frame.error()));
                break;
            }

            auto& entry = index[num_frames];
            entry.offset      = offset;
            entry.total_len   = frame->total_len;
            entry.opcode      = frame->opcode;
            entry.fin         = frame->fin;
            entry.masked      = frame->masked;
            std::memcpy(&entry.mask_key, frame->mask_key, 4);
            entry.payload     = frame->payload;
            entry.payload_len = frame->payload_len;
            entry.is_control  = frame->is_control();

            // Build FrameView inline for filterable data frames.
            if (!entry.is_control &&
                entry.opcode != ws::opcode::kContinuation &&
                entry.fin &&
                entry.payload && entry.payload_len > 0) {
                auto& v = views[num_views];
                v.payload     = entry.payload;
                v.payload_len = static_cast<uint16_t>(
                    std::min(entry.payload_len, uint64_t{UINT16_MAX}));
                v.opcode      = entry.opcode;
                v.deliver     = true;
                view_to_frame[num_views] = num_frames;
                ++num_views;
            }

            offset += frame->total_len;
            ++num_frames;
        }

        if (num_frames == 0) return offset;

        // ── Phase 2: call filter ────────────────────────────────────────
        if (num_views > 0) {
            config_.on_frame_filter(std::span<FrameView>(views, num_views));
        }

        // ── Phase 3: dispatch ───────────────────────────────────────────
        // Build deliver bitmap from filter results.
        bool deliver[kMaxFramesPerBatch];
        for (size_t i = 0; i < num_frames; ++i) deliver[i] = true;
        for (size_t vi = 0; vi < num_views; ++vi) {
            deliver[view_to_frame[vi]] = views[vi].deliver;
        }

        uint64_t data_total = 0;
        [[maybe_unused]] uint64_t data_delivered = 0;

        for (size_t i = 0; i < num_frames; ++i) {
            auto& entry = index[i];
            if (entry.is_control) {
                dispatch_indexed_frame(entry, data + entry.offset);
                continue;
            }
            ++data_total;
            if (deliver[i]) {
                dispatch_indexed_frame(entry, data + entry.offset);
                ++data_delivered;
            }
        }

        if (data_total > 0) {
            rx_stats_.packets.fetch_add(data_total,
                                        std::memory_order_relaxed);
            record_rx_latency();
        }

        SPDLOG_LOGGER_TRACE(log,
            "Frame filter: {}/{} delivered, {} skipped",
            data_delivered, data_total, data_total - data_delivered);

        return offset;
    }

    /// Dispatch a single indexed frame through the standard control-frame
    /// handling or data delivery path.
    void dispatch_indexed_frame(const FrameIndexEntry& entry,
                                [[maybe_unused]] const uint8_t* frame_start) noexcept {
        // Reconstruct a minimal DecodedFrame for reuse of existing handlers.
        ws::DecodedFrame frame;
        frame.opcode      = entry.opcode;
        frame.fin         = entry.fin;
        frame.masked      = entry.masked;
        std::memcpy(frame.mask_key, &entry.mask_key, 4);
        frame.payload     = entry.payload;
        frame.payload_len = entry.payload_len;
        frame.total_len   = entry.total_len;

        if (frame.is_ping()) {
            ws_pings_received_.fetch_add(1, std::memory_order_relaxed);
            if (config_.on_ping) {
                try {
                    config_.on_ping(frame.payload,
                                    static_cast<uint16_t>(frame.payload_len));
                } catch (...) {}
            }
            handle_ping(frame);
            return;
        }
        if (frame.is_close()) {
            uint16_t code = frame.close_status_code();
            std::string_view reason = frame.close_reason();
            SPDLOG_LOGGER_INFO(detail::transport_logger(),
                "Received WS Close frame: code={} reason=\"{}\"", code, reason);
            if (config_.on_close) {
                try { config_.on_close(code, reason); } catch (...) {}
            }
            if (frame.payload && frame.payload_len > 0 &&
                frame.payload_len <= MaxPayload) {
                if constexpr (kHasRxQueue) {
                    (void)rx_enqueue(frame.payload,
                                     static_cast<uint16_t>(frame.payload_len),
                                     ws::opcode::kClose);
                }
            }
            handle_close(code);
            closing_.store(true, std::memory_order_release);
            return;
        }
        if (frame.is_pong()) {
            last_pong_ns_.store(
                std::chrono::steady_clock::now().time_since_epoch().count(),
                std::memory_order_relaxed);
            if (config_.on_pong) {
                try {
                    config_.on_pong(frame.payload,
                                    static_cast<uint16_t>(frame.payload_len));
                } catch (...) {}
            }
            return;
        }

        // Data frame: deliver
        if (frame.is_data() && frame.payload_len > 0 &&
            frame.payload_len <= MaxPayload) {
            deliver_data_frame(frame);
        }
    }

    /// Enqueue pong response into TX queue so the TX thread sends it.
    /// In kDirect/kDirectTx mode (no TX queue), send the pong directly.
    void handle_ping(const ws::DecodedFrame& ping_frame) noexcept {
        // Ping payload is at most 125 bytes (RFC 6455 §5.5).
        size_t pong_payload_len = std::min(
            static_cast<size_t>(ping_frame.payload_len),
            static_cast<size_t>(MaxPayload));

        if constexpr (kHasTxQueue) {
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
        } else {
            // No TX queue — send pong directly.
            uint8_t pong_buf[125];
            if (ping_frame.payload && pong_payload_len > 0) {
                std::memcpy(pong_buf, ping_frame.payload, pong_payload_len);
                if (ping_frame.masked) {
                    ws::apply_mask(pong_buf, pong_payload_len,
                                   ping_frame.mask_key);
                }
            }
            auto err = send_direct(pong_buf, pong_payload_len,
                                   ws::opcode::kPong);
            if (err == SendError::kOk) {
                ws_pongs_sent_.fetch_add(1, std::memory_order_relaxed);
            } else {
                SPDLOG_LOGGER_DEBUG(detail::transport_logger(),
                    "send_direct pong failed: {}", send_error_name(err));
            }
        }
    }

    /// Send a Close frame response.
    /// In threaded mode, enqueue into TX queue. In direct mode, send immediately.
    void handle_close(uint16_t status_code) noexcept {
        uint8_t close_payload[2] = {
            static_cast<uint8_t>(status_code >> 8),
            static_cast<uint8_t>(status_code & 0xFF),
        };
        if constexpr (kHasTxQueue) {
            tx_queue_.try_produce([&](TxMsg& msg) {
                msg.data[0] = close_payload[0];
                msg.data[1] = close_payload[1];
                msg.len = 2;
                msg.opcode = ws::opcode::kClose;
            });
        } else {
            (void)send_direct(close_payload, 2, ws::opcode::kClose);
        }
    }
