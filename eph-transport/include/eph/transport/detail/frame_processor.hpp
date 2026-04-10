#pragma once

/// @file frame_processor.hpp
/// Independent frame processing component extracted from the Transport class.
///
/// FrameProcessor decodes frames (WS or generic), handles WS fragmentation
/// reassembly, delivers data frames via a DeliverPolicy, and sends control
/// frame responses (pong/close) via a SendFn. All dependencies are injected
/// through a Deps struct — no Transport coupling.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/core/tcp_concept.hpp"
#include "eph/core/framer_concept.hpp"
#include "eph/core/transport_errors.hpp"
#include "eph/transport/detail/transport_core.hpp"
#include "eph/transport/transport_types.hpp"
#include "eph/transport/detail/websocket.hpp"
#include "eph/transport/detail/message_types.hpp"
#include "eph/utils/hdr_histogram.hpp"
#include "eph/utils/time.hpp"

namespace eph::net {

// kEnableTimestamps is defined once in transport_types.hpp (included above).

// -----------------------------------------------------------------------
// FrameProcessor
// -----------------------------------------------------------------------

/// Stateful frame decoder and dispatcher for the RX path.
///
/// Decodes frames using the configured Framer, handles WebSocket-specific
/// concerns (fragmentation reassembly, control frame responses, ping/pong RTT,
/// batch frame filtering for symbol dedup), and delivers data frames via the
/// injected DeliverPolicy.
///
/// @tparam TcpImpl         Type satisfying the TcpTransport concept
/// @tparam Framer          Message framer (WsFramer for WebSocket, or generic)
/// @tparam DeliverPolicy   Callable: void(const uint8_t* data, uint16_t len, uint8_t opcode)
/// @tparam SendFn          Callable: SendError(const void* data, size_t len, uint8_t opcode)
/// @tparam MaxPayload      Maximum application payload size per message
/// @tparam LastOnlyDeliver When true, only the last data frame per batch is delivered
template <TcpTransport TcpImpl, MessageFramer Framer,
          typename DeliverPolicy, typename SendFn,
          size_t MaxPayload, bool LastOnlyDeliver = false>
class FrameProcessor {
    static_assert(MaxPayload > 0, "MaxPayload must be > 0");

public:
    /// True when using WebSocket framing (enables WS handshake, ping/pong,
    /// close handshake, and fragmentation reassembly).
    static constexpr bool kIsWebSocket = std::is_same_v<Framer, WsFramer>;

    // -------------------------------------------------------------------
    // Dependency bundle — all external state injected here
    // -------------------------------------------------------------------

    struct Deps {
        TransportCore<TcpImpl>& core;
        DeliverPolicy deliver;
        SendFn send_response;
        ThreadStats& rx_stats;
        std::atomic<uint64_t>& ws_pings_received;
        std::atomic<uint64_t>& ws_pongs_sent;
        eph::utils::HdrHistogram& rtt_histogram;
        eph::utils::HdrHistogram& rx_latency_histogram;
        eph::utils::HdrHistogram& rx_decrypt_histogram;
        eph::utils::HdrHistogram& rx_decode_histogram;
        std::atomic<size_t>& rx_hwm;
    };

    // -------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------

    explicit FrameProcessor(Deps deps)
        : deps_(std::move(deps))
    {
        // Pre-allocate WS fragmentation buffer to avoid heap allocation
        // on the RX hot path when reassembling multi-frame messages.
        if constexpr (kIsWebSocket) {
            ws_frag_buf_.reserve(MaxPayload);
        }
    }

    // Non-copyable, movable
    FrameProcessor(const FrameProcessor&) = delete;
    FrameProcessor& operator=(const FrameProcessor&) = delete;
    FrameProcessor(FrameProcessor&&) noexcept = default;
    FrameProcessor& operator=(FrameProcessor&&) noexcept = default;

    // -------------------------------------------------------------------
    // Public API
    // -------------------------------------------------------------------

    /// Process decrypted framed data. Returns the number of bytes consumed.
    /// Unconsumed bytes (partial frames) must be preserved by the caller and
    /// prepended to the next chunk of decrypted data.
    ///
    /// For WsFramer: handles WS control frames (ping/pong/close) and
    /// fragmentation reassembly. For generic framers: simple decode loop.
    [[nodiscard]] size_t process(const uint8_t* data, size_t len) {
        if constexpr (kIsWebSocket) {
            return process_ws_data(data, len);
        } else {
            return process_generic_data(data, len);
        }
    }

    /// Reset internal state (clear fragmentation buffer).
    void reset() noexcept {
        ws_frag_buf_.clear();
        ws_frag_opcode_ = 0;
        framer_ = Framer{};
    }

private:
    // -------------------------------------------------------------------
    // Convenience accessors
    // -------------------------------------------------------------------

    auto& core()        noexcept { return deps_.core; }
    const auto& core()  const noexcept { return deps_.core; }
    auto& config()      noexcept { return deps_.core.config; }
    const auto& config() const noexcept { return deps_.core.config; }

    // -------------------------------------------------------------------
    // RX latency recording (shared by all modes)
    // -------------------------------------------------------------------

    /// Record per-message RX latency breakdown:
    ///   total:   NIC arrival -> decoded
    ///   decrypt: NIC arrival -> decrypt_done (TLS decryption)
    ///   decode:  decrypt_done -> decoded
    ///
    /// current_arrival_tsc is already back-dated to NIC arrival time
    /// (kernel stack delay subtracted at poll_rx), so no additional
    /// kernel delay adjustment is needed here.
    void record_rx_latency() noexcept {
        if constexpr (kEnableTimestamps) {
            uint64_t now_tsc = eph::utils::TSC::now();
            if (core().current_arrival_tsc > 0 && now_tsc > core().current_arrival_tsc) {
                auto cycles_to_ns = [this](uint64_t c) -> uint64_t {
                    return static_cast<uint64_t>(
                        static_cast<double>(c) * core().ns_per_cycle);
                };

                // Total: NIC arrival -> decoded
                uint64_t total = cycles_to_ns(now_tsc - core().current_arrival_tsc);
                deps_.rx_latency_histogram.record(total);

                // Decrypt: arrival -> decrypt_done (TLS only)
                if (core().current_decrypt_done_tsc > core().current_arrival_tsc) {
                    deps_.rx_decrypt_histogram.record(
                        cycles_to_ns(core().current_decrypt_done_tsc -
                                     core().current_arrival_tsc));
                }
                // Decode: decrypt_done -> now (WS frame parsing)
                if (core().current_decrypt_done_tsc > 0 &&
                    now_tsc > core().current_decrypt_done_tsc) {
                    deps_.rx_decode_histogram.record(
                        cycles_to_ns(now_tsc - core().current_decrypt_done_tsc));
                }
            }
        }
    }

    // -------------------------------------------------------------------
    // Generic (non-WS) frame processing
    // -------------------------------------------------------------------

    /// Process data using a generic (non-WS) framer. Simple decode loop
    /// that delivers each successfully decoded frame's payload directly.
    size_t process_generic_data(const uint8_t* data, size_t len) {
        [[maybe_unused]] auto log = detail::transport_logger();
        size_t offset = 0;

        while (offset < len) {
            auto frame = framer_.decode(data + offset, len - offset);
            if (!frame) {
                if (frame.error() == FrameError::kIncomplete) break;
                SPDLOG_LOGGER_WARN(log, "Frame decode error: {}",
                                   frame_error_name(frame.error()));
                break;
            }

            offset += frame->total_len;
            deps_.rx_stats.packets.fetch_add(1, std::memory_order_relaxed);
            record_rx_latency();

            // Deliver payload directly (no control frame handling for non-WS)
            if (frame->payload_len > 0 && frame->payload_len <= MaxPayload) {
                deliver_message(frame->payload,
                               static_cast<uint16_t>(frame->payload_len),
                               frame->msg_type);
            } else if (frame->payload_len > MaxPayload) {
                deps_.rx_stats.dropped.fetch_add(1, std::memory_order_relaxed);
                SPDLOG_LOGGER_WARN(log,
                    "Dropping oversized frame: payload_len={}, max={}",
                    frame->payload_len, MaxPayload);
            }
        }
        return offset;
    }

    // -------------------------------------------------------------------
    // WebSocket frame processing
    // -------------------------------------------------------------------

    /// Process decrypted WebSocket data (WsFramer only). Returns the number
    /// of bytes consumed.
    size_t process_ws_data(const uint8_t* data, size_t len) {
        // Batch frame filter: route to filtered path when configured.
        if (config().on_frame_filter) {
            return process_ws_data_filtered(data, len);
        }

        [[maybe_unused]] auto log = detail::transport_logger();
        size_t offset = 0;

        // EvictingQueue last-only optimization: when the app only reads
        // the latest value, intermediate data frames are wasted work
        // (stats atomics, latency histogram, UTF-8 check, memcpy,
        // enqueue -- all overwritten immediately). Instead, we decode
        // all frames but only deliver the last data frame per call.
        // Control frames (ping/close/pong) are always handled immediately.
        [[maybe_unused]] const ws::DecodedFrame* last_data_frame = nullptr;
        [[maybe_unused]] uint64_t data_frame_count = 0;
        // Accumulate byte-level stats locally to avoid per-frame atomics.
        // Flushed once after the loop -- saves ~15ns/frame on hot path.
        uint64_t batch_bytes = 0;
        uint64_t batch_text_bytes = 0;
        uint64_t batch_text_packets = 0;

        // Pre-compute whether we can use the direct deliver fast path,
        // bypassing deliver_data_frame -> deliver_message overhead.
        // Requires: no UTF-8 validation, no on_message callback.
        const bool fast_deliver = config().skip_utf8_validation &&
                                  !config().on_message;

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
            // which inflated the very latency we measure. Recording once
            // at the end captures worst-case (last frame) latency without
            // the measurement itself contaminating the result.

            if (frame->is_ping()) [[unlikely]] {
                deps_.rx_stats.packets.fetch_add(1, std::memory_order_relaxed);
                deps_.ws_pings_received.fetch_add(1, std::memory_order_relaxed);
                if (config().on_ping) {
                    try {
                        config().on_ping(frame->payload,
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
                deps_.rx_stats.packets.fetch_add(1, std::memory_order_relaxed);
                uint16_t code = frame->close_status_code();
                std::string_view close_reason = frame->close_reason();
                SPDLOG_LOGGER_INFO(log,
                    "Received WS Close frame: code={} reason=\"{}\"",
                    code, close_reason);
                // Notify application of close reason before responding
                if (config().on_close) {
                    try {
                        config().on_close(code, close_reason);
                    } catch (...) {
                        SPDLOG_LOGGER_WARN(log,
                            "on_close callback threw an exception");
                    }
                }
                // Deliver close frame so polling-mode users can detect
                // server-initiated close via try_recv_msg().
                if (frame->payload && frame->payload_len > 0 &&
                    frame->payload_len <= MaxPayload) {
                    deps_.deliver(frame->payload,
                                  static_cast<uint16_t>(frame->payload_len),
                                  ws::opcode::kClose);
                }
                // RFC 6455 section 5.5.1: respond with a Close frame.
                // §7.4.1 forbids codes 0-999, 1004, 1005, 1006, 1015,
                // 1016-2999 from appearing in a Close frame — if the
                // peer sent one anyway, we MUST NOT echo it back (that
                // would also be a violation, and turns us into a
                // protocol-violation amplifier).  Override to
                // kProtocolError per Autobahn §7.9.x.
                uint16_t response_code = code;
                if (frame->payload_len == 0) {
                    response_code = ws::close_code::kNormal;
                } else if (!ws::is_valid_close_code(code)) {
                    SPDLOG_LOGGER_WARN(log,
                        "Received Close frame with invalid status code {} "
                        "(RFC 6455 §7.4.1) — responding with kProtocolError",
                        code);
                    response_code = ws::close_code::kProtocolError;
                }
                handle_close(response_code);
                // Signal TX to drain the Close response before exiting.
                core().closing.store(true, std::memory_order_release);
                break;
            }

            if (frame->is_pong()) [[unlikely]] {
                deps_.rx_stats.packets.fetch_add(1, std::memory_order_relaxed);
                record_rx_latency();
                // Record pong arrival for timeout detection (TX thread reads this).
                core().last_pong_ns.store(
                    std::chrono::steady_clock::now().time_since_epoch().count(),
                    std::memory_order_relaxed);

                // RTT measurement: compute delta from the ping TSC timestamp.
                // Includes kernel TX+RX stack delays when available for full-path RTT.
                uint64_t ping_tsc = core().last_ping_tsc.load(
                    std::memory_order_relaxed);
                if (ping_tsc > 0 && eph::utils::TSC::is_initialized()) {
                    uint64_t pong_tsc = eph::utils::TSC::now();
                    if (pong_tsc > ping_tsc) {
                        auto rtt_ns = eph::utils::TSC::to_ns(pong_tsc - ping_tsc);
                        if (rtt_ns) {
                            uint64_t total = static_cast<uint64_t>(*rtt_ns);
                            // Kernel stack delays are not available in the
                            // extracted FrameProcessor (no tcp_ pointer).
                            // The owning Transport can add them if needed.
                            deps_.rtt_histogram.record(total);
                        }
                    }
                    // Clear ping TSC so we don't double-record on spurious pongs
                    core().last_ping_tsc.store(0, std::memory_order_relaxed);
                }

                if (config().on_pong) {
                    try {
                        config().on_pong(frame->payload,
                                         static_cast<uint16_t>(frame->payload_len));
                    } catch (...) {
                        SPDLOG_LOGGER_WARN(detail::transport_logger(),
                            "on_pong callback threw an exception");
                    }
                }
                continue;
            }

            // Data frame handling with fragmentation reassembly.
            // RFC 6455 section 5.4: first fragment has opcode != 0, FIN=0;
            // continuation fragments have opcode=0; final fragment has FIN=1.
            if (!frame->is_data()) [[unlikely]] continue;

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
                if constexpr (LastOnlyDeliver) {
                    last_data_frame = &*frame;
                } else {
                    // Per-frame record_rx_latency: capture TSC at decode
                    // completion (before delivery) so each frame measures
                    // NIC arrival -> its own decode, not the cumulative
                    // cost of all frames in the batch.
                    record_rx_latency();
                    if (fast_deliver && !frame->masked && frame->payload_len > 0)
                        [[likely]] {
                        deps_.deliver(frame->payload,
                            static_cast<uint16_t>(frame->payload_len),
                            frame->opcode);
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
                deps_.rx_stats.dropped.fetch_add(1, std::memory_order_relaxed);
                SPDLOG_LOGGER_WARN(log,
                    "Dropping oversized WS frame: payload_len={}, "
                    "max={}, opcode=0x{:02x}",
                    frame->payload_len, MaxPayload, frame->opcode);
            } else {
                // Fragmented message: accumulate
                size_t new_size = ws_frag_buf_.size() + frame->payload_len;
                if (new_size > MaxPayload) {
                    deps_.rx_stats.dropped.fetch_add(1, std::memory_order_relaxed);
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
                    // Reassembly complete -- deliver
                    if (!ws_frag_buf_.empty()) {
                        ++data_frame_count;
                        auto frag_len = static_cast<uint16_t>(ws_frag_buf_.size());
                        if constexpr (LastOnlyDeliver) {
                            // Cannot defer fragmented frames (buffer reused
                            // next iteration), so deliver immediately.
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
            if constexpr (LastOnlyDeliver) {
                record_rx_latency();
            }
            deps_.rx_stats.packets.fetch_add(data_frame_count,
                                             std::memory_order_relaxed);
            if (batch_bytes > 0) {
                deps_.rx_stats.bytes.fetch_add(batch_bytes,
                                               std::memory_order_relaxed);
            }
            if (batch_text_packets > 0) {
                deps_.rx_stats.text_packets.fetch_add(batch_text_packets,
                                                      std::memory_order_relaxed);
                deps_.rx_stats.text_bytes.fetch_add(batch_text_bytes,
                                                    std::memory_order_relaxed);
            }
            if constexpr (LastOnlyDeliver) {
                if (last_data_frame) {
                    deliver_data_frame(*last_data_frame);
                }
            }
        }

        return offset;
    }

    // -------------------------------------------------------------------
    // Message delivery
    // -------------------------------------------------------------------

    /// Deliver a decoded payload via the DeliverPolicy.
    /// Text frames are validated for UTF-8 compliance (RFC 6455 section 5.6)
    /// unless TransportConfig::skip_utf8_validation is true.
    ///
    /// @param defer_stats  When true, skip per-message byte/text stats
    ///   updates (caller will batch-flush them after the decode loop).
    void deliver_message(const uint8_t* data, uint16_t len, uint8_t opcode,
                         bool defer_stats = false) noexcept {
        // RFC 6455 section 5.6: text frames must contain valid UTF-8
        if (opcode == ws::opcode::kText && !config().skip_utf8_validation &&
            !ws::is_valid_utf8(data, len)) {
            deps_.rx_stats.dropped.fetch_add(1, std::memory_order_relaxed);
            SPDLOG_LOGGER_WARN(detail::transport_logger(),
                "Dropping text frame with invalid UTF-8 (len={})", len);
            return;
        }

        auto update_rx_stats = [&] {
            if (defer_stats) return;
            deps_.rx_stats.bytes.fetch_add(len, std::memory_order_relaxed);
            if (opcode == ws::opcode::kText) {
                deps_.rx_stats.text_packets.fetch_add(1,
                    std::memory_order_relaxed);
                deps_.rx_stats.text_bytes.fetch_add(len,
                    std::memory_order_relaxed);
            }
        };

        // If on_message callback is set, deliver directly to it and
        // skip queue delivery. This matches the original Transport behavior:
        // on_message is a "push" mode alternative to queue-based "pull" mode.
        if (config().on_message) {
            try {
                config().on_message(data, len, opcode);
            } catch (...) {
                SPDLOG_LOGGER_WARN(detail::transport_logger(),
                    "on_message callback threw an exception");
            }
            update_rx_stats();
            return;
        }

        // No on_message callback — deliver via the policy (queue enqueue etc.)
        deps_.deliver(data, len, opcode);
        update_rx_stats();
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
            deliver_message(frame.payload,
                            static_cast<uint16_t>(frame.payload_len),
                            frame.opcode, defer_stats);
        }
    }

    // -------------------------------------------------------------------
    // Symbol-aware dedup: process_ws_data with per-symbol latest-only delivery
    // -------------------------------------------------------------------

    /// Lightweight frame index entry for the symbol-dedup scan pass.
    /// Stored on the stack (max ~128 frames per TLS record).
    struct FrameIndexEntry {
        size_t         offset;       ///< byte offset in source buffer
        size_t         total_len;    ///< total WS frame length
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
        [[maybe_unused]] auto log = detail::transport_logger();

        // -- Phase 1: single-pass scan --
        // Build frame index (for dispatch) and FrameView (for filter)
        // simultaneously to avoid redundant iteration.
        static constexpr size_t kMaxFramesPerBatch = 128;
        FrameIndexEntry index[kMaxFramesPerBatch];
        FrameView views[kMaxFramesPerBatch];
        size_t    view_to_frame[kMaxFramesPerBatch]; // view idx -> frame idx
        size_t num_frames = 0;
        size_t num_views = 0;
        size_t offset = 0;

        while (offset < len && num_frames < kMaxFramesPerBatch) {
            auto frame = ws::decode_frame(data + offset, len - offset);
            if (!frame) {
                if (frame.error() == ws::DecodeError::kIncomplete) break;
                SPDLOG_LOGGER_WARN(log,
                    "WS frame decode error in filter scan: {}",
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

        // -- Phase 2: call filter --
        if (num_views > 0) {
            config().on_frame_filter(
                std::span<FrameView>(views, num_views));
        }

        // -- Phase 3: dispatch --
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
            deps_.rx_stats.packets.fetch_add(data_total,
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
            deps_.rx_stats.packets.fetch_add(1, std::memory_order_relaxed);
            deps_.ws_pings_received.fetch_add(1, std::memory_order_relaxed);
            if (config().on_ping) {
                try {
                    config().on_ping(frame.payload,
                                     static_cast<uint16_t>(frame.payload_len));
                } catch (const std::exception& e) {
                    SPDLOG_LOGGER_ERROR(detail::transport_logger(),
                        "on_ping callback threw: {}", e.what());
                } catch (...) {
                    SPDLOG_LOGGER_ERROR(detail::transport_logger(),
                        "on_ping callback threw non-std::exception");
                }
            }
            handle_ping(frame);
            return;
        }
        if (frame.is_close()) {
            deps_.rx_stats.packets.fetch_add(1, std::memory_order_relaxed);
            uint16_t code = frame.close_status_code();
            std::string_view reason = frame.close_reason();
            SPDLOG_LOGGER_INFO(detail::transport_logger(),
                "Received WS Close frame: code={} reason=\"{}\"",
                code, reason);
            if (config().on_close) {
                try {
                    config().on_close(code, reason);
                } catch (const std::exception& e) {
                    SPDLOG_LOGGER_ERROR(detail::transport_logger(),
                        "on_close callback threw: {}", e.what());
                } catch (...) {
                    SPDLOG_LOGGER_ERROR(detail::transport_logger(),
                        "on_close callback threw non-std::exception");
                }
            }
            if (frame.payload && frame.payload_len > 0 &&
                frame.payload_len <= MaxPayload) {
                deps_.deliver(frame.payload,
                              static_cast<uint16_t>(frame.payload_len),
                              ws::opcode::kClose);
            }
            // RFC 6455 §7.4.1: codes 0-999, 1004, 1005, 1006, 1015, and
            // 1016-2999 MUST NOT appear in a Close frame.  If the peer
            // sent one anyway, we MUST NOT echo it back — that would
            // violate the same MUST NOT, and would let the peer use us
            // as a protocol-violation amplifier.  Override the response
            // code to kProtocolError (1002) per Autobahn §7.9.x.
            // Code == 0 means the frame had no body, which is legal:
            // respond with kNormal in that case (any send-valid code).
            uint16_t response_code = code;
            if (frame.payload_len == 0) {
                response_code = ws::close_code::kNormal;
            } else if (!ws::is_valid_close_code(code)) {
                SPDLOG_LOGGER_WARN(detail::transport_logger(),
                    "Received Close frame with invalid status code {} "
                    "(RFC 6455 §7.4.1) — responding with kProtocolError",
                    code);
                response_code = ws::close_code::kProtocolError;
            }
            handle_close(response_code);
            core().closing.store(true, std::memory_order_release);
            return;
        }
        if (frame.is_pong()) {
            deps_.rx_stats.packets.fetch_add(1, std::memory_order_relaxed);
            record_rx_latency();
            core().last_pong_ns.store(
                std::chrono::steady_clock::now().time_since_epoch().count(),
                std::memory_order_relaxed);

            // RTT measurement: mirror the logic in process_ws_data() so
            // filtered-mode pongs also record round-trip latency.
            uint64_t ping_tsc = core().last_ping_tsc.load(
                std::memory_order_relaxed);
            if (ping_tsc > 0 && eph::utils::TSC::is_initialized()) {
                uint64_t pong_tsc = eph::utils::TSC::now();
                if (pong_tsc > ping_tsc) {
                    auto rtt_ns = eph::utils::TSC::to_ns(pong_tsc - ping_tsc);
                    if (rtt_ns) {
                        deps_.rtt_histogram.record(
                            static_cast<uint64_t>(*rtt_ns));
                    }
                }
                core().last_ping_tsc.store(0, std::memory_order_relaxed);
            }

            if (config().on_pong) {
                try {
                    config().on_pong(frame.payload,
                                     static_cast<uint16_t>(frame.payload_len));
                } catch (const std::exception& e) {
                    SPDLOG_LOGGER_ERROR(detail::transport_logger(),
                        "on_pong callback threw: {}", e.what());
                } catch (...) {
                    SPDLOG_LOGGER_ERROR(detail::transport_logger(),
                        "on_pong callback threw non-std::exception");
                }
            }
            return;
        }

        // Data frame: deliver
        if (frame.is_data() && frame.payload_len > 0 &&
            frame.payload_len <= MaxPayload) {
            deliver_data_frame(frame);
        }
    }

    // -------------------------------------------------------------------
    // Control frame handlers
    // -------------------------------------------------------------------

    /// Send a pong response via the SendFn.
    void handle_ping(const ws::DecodedFrame& ping_frame) noexcept {
        // Ping payload is at most 125 bytes (RFC 6455 section 5.5).
        size_t pong_payload_len = std::min(
            static_cast<size_t>(ping_frame.payload_len),
            static_cast<size_t>(MaxPayload));

        uint8_t pong_buf[125];
        if (ping_frame.payload && pong_payload_len > 0) {
            std::memcpy(pong_buf, ping_frame.payload, pong_payload_len);
            if (ping_frame.masked) {
                ws::apply_mask(pong_buf, pong_payload_len,
                               ping_frame.mask_key);
            }
        }
        auto err = deps_.send_response(pong_buf, pong_payload_len,
                                        ws::opcode::kPong);
        if (err == SendError::kOk) {
            deps_.ws_pongs_sent.fetch_add(1, std::memory_order_relaxed);
        } else {
            SPDLOG_LOGGER_DEBUG(detail::transport_logger(),
                "send_response pong failed: {}", send_error_name(err));
        }
    }

    /// Send a Close frame response via the SendFn.
    void handle_close(uint16_t status_code) noexcept {
        uint8_t close_payload[2] = {
            static_cast<uint8_t>(status_code >> 8),
            static_cast<uint8_t>(status_code & 0xFF),
        };
        auto err = deps_.send_response(close_payload, 2, ws::opcode::kClose);
        if (err != SendError::kOk) {
            SPDLOG_LOGGER_DEBUG(detail::transport_logger(),
                "send_response close failed: {}", send_error_name(err));
        }
    }

    // -------------------------------------------------------------------
    // Member data
    // -------------------------------------------------------------------

    Deps deps_;
    Framer framer_{};
    std::vector<uint8_t> ws_frag_buf_;
    uint8_t ws_frag_opcode_{0};
};

} // namespace eph::net
