#pragma once

/// @file frame_filter.hpp
/// Batch frame filter for selective delivery in multi-symbol streams.
///
/// Provides FrameView, FrameFilterFn, and the two-phase filter algorithm
/// that delivers only the latest frame per symbol hash.
///
/// Extracted from transport_types.hpp to reduce header coupling:
/// downstream code that only needs config or stats doesn't pull in
/// the filter algorithm and its <array>/<span> dependencies.

#include <array>
#include <cstdint>
#include <functional>
#include <span>

namespace eph::net {

// ---------------------------------------------------------------------------
// Batch frame filter for selective delivery
// ---------------------------------------------------------------------------

/// Lightweight view of a decoded WS data frame, exposed to batch filters.
/// Control frames (ping/pong/close) and fragmented frames are NOT included —
/// they are always delivered unconditionally.
struct FrameView {
    const uint8_t* payload     = nullptr; ///< Payload pointer (unmasked for server frames)
    uint16_t       payload_len = 0;       ///< Payload length
    uint8_t        opcode      = 0;       ///< WS opcode (text/binary)
    bool           deliver     = true;    ///< Set to false to skip delivery
};

/// Batch frame filter callback. Called from the RX thread after decoding all
/// WS frames in a TLS record / TCP segment. The filter inspects the batch
/// and sets `deliver = false` on frames that should be skipped.
///
/// @warning Called from the RX thread — must be non-blocking, no heap allocation.
using FrameFilterFn = std::function<void(std::span<FrameView>)>;

/// Constants for the two-phase frame filter hash table.
namespace filter_detail {
/// Open-addressing hash table slots. 256 slots for ≤128 frames → ≤50% load.
inline constexpr size_t kFilterSlots = 256;
/// Maximum frames processed per batch (bounded by stack allocation).
inline constexpr size_t kMaxFramesPerBatch = 128;

/// Sentinel value for empty hash table slots. UINT32_MAX is used instead
/// of 0 so that 0 can serve as the "unrecognized payload" return value
/// from extractor functions (always delivered, never deduplicated).
inline constexpr uint32_t kEmptySlotHash = UINT32_MAX;

/// Hash value that extractors return for unrecognized payloads.
/// Frames with this hash are always delivered (never deduplicated).
inline constexpr uint32_t kUnrecognizedHash = 0;

/// Core two-phase filter logic, shared between std::function and function pointer overloads.
///
/// Pass 1: extract hashes and record the last frame index per symbol hash.
/// Pass 2: mark all non-latest frames as deliver=false, then restore latest.
///
/// @tparam ExtractorFn  Callable as uint32_t(const uint8_t*, size_t)
/// @param frames  Span of FrameView entries to filter (deliver flags are mutated)
/// @param ext     Symbol hash extractor function
template <typename ExtractorFn>
void apply_twophase_filter(std::span<FrameView> frames, ExtractorFn&& ext) {
    struct Slot { uint32_t hash = kEmptySlotHash; size_t last_idx = 0; };
    std::array<Slot, kFilterSlots> slots;
    // Initialize all slots to the empty sentinel.
    slots.fill({kEmptySlotHash, 0});

    // Compute hashes once, cache for reuse in pass 2.
    std::array<uint32_t, kMaxFramesPerBatch> hashes{};
    size_t n = std::min(frames.size(), kMaxFramesPerBatch);

    // Pass 1: extract hashes + record last index per symbol.
    for (size_t i = 0; i < n; ++i) {
        auto& f = frames[i];
        uint32_t h = ext(f.payload, f.payload_len);
        hashes[i] = h;
        // Hash 0 = unrecognized payload, UINT32_MAX = empty slot sentinel.
        // Both bypass dedup — frame is always delivered.
        if (h == kUnrecognizedHash || h == kEmptySlotHash) continue;
        size_t slot = h & (kFilterSlots - 1);
        for (size_t j = 0; j < kFilterSlots; ++j) {
            size_t s = (slot + j) & (kFilterSlots - 1);
            if (slots[s].hash == kEmptySlotHash) {
                slots[s] = {h, i};
                break;
            }
            if (slots[s].hash == h) {
                slots[s].last_idx = i;
                break;
            }
        }
    }

    // Pass 2: mark non-latest as skip using cached hashes.
    for (size_t i = 0; i < n; ++i) {
        if (hashes[i] != kEmptySlotHash && hashes[i] != kUnrecognizedHash)
            frames[i].deliver = false;
    }
    // Restore latest per symbol.
    for (auto& s : slots) {
        if (s.hash != kEmptySlotHash) {
            frames[s.last_idx].deliver = true;
        }
    }
}
} // namespace filter_detail

/// Create a batch frame filter that delivers only the latest frame per symbol.
///
/// Two-phase forward scan: pass 1 records last index per symbol hash,
/// pass 2 marks all non-latest as deliver=false. Control frames and
/// fragmented frames are not subject to filtering (handled by transport).
///
/// @param extractor  Extracts a symbol hash from payload.
///                   Returns 0 for unrecognized payloads (always delivered).
///
/// Example:
/// @code
///   tc.on_frame_filter = make_twophase_filter(binance_symbol_hash);
/// @endcode
inline FrameFilterFn make_twophase_filter(
    std::function<uint32_t(const uint8_t* data, size_t len)> extractor)
{
    return [ext = std::move(extractor)](std::span<FrameView> frames) {
        filter_detail::apply_twophase_filter(frames, ext);
    };
}

/// @overload Raw function pointer variant — avoids std::function overhead
/// on the hot path. Prefer this when the extractor is a plain function.
inline FrameFilterFn make_twophase_filter(
    uint32_t (*extractor)(const uint8_t* data, size_t len))
{
    return [extractor](std::span<FrameView> frames) {
        filter_detail::apply_twophase_filter(frames, extractor);
    };
}

} // namespace eph::net
