/// @file test_codec_concept.cpp
/// Compile-time + runtime tests for StreamCodec / DatagramCodec concepts.

#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "eph/core/codec.hpp"
#include "eph/core/error.hpp"

using eph::core::Codec;
using eph::core::DatagramCodec;
using eph::core::Error;
using eph::core::ErrorInfo;
using eph::core::OutputBuffer;
using eph::core::StreamCodec;

// ============================================================================
// Fake PacketView — duck-typed minimal surface required by codecs
// ============================================================================

/// Minimal PacketView stub — satisfies the requirements documented in
/// codec.hpp (writable_data, data, length, trim_front/back, arrival_tsc).
///
/// We store a moving window into an external buffer so trim_front/trim_back
/// semantics are realistic without any allocation.
struct FakePacketView {
    uint8_t*  buf;
    size_t    head;
    size_t    tail;    // one past last byte
    uint64_t  tsc;

    FakePacketView(uint8_t* b, size_t len, uint64_t t = 0)
        : buf(b), head(0), tail(len), tsc(t) {}

    [[nodiscard]] uint8_t*       writable_data()         noexcept { return buf + head; }
    [[nodiscard]] const uint8_t* data()           const  noexcept { return buf + head; }
    [[nodiscard]] size_t         length()         const  noexcept { return tail - head; }
    void trim_front(size_t n) noexcept { head += n; }
    void trim_back(size_t n)  noexcept { tail -= n; }
    [[nodiscard]] uint64_t arrival_tsc() const noexcept { return tsc; }
};

// ============================================================================
// FakeStreamCodec — minimal StreamCodec that passes bytes through as one Frame
// ============================================================================

/// Decodes: emits the entire available window as a single Frame on each call.
/// Encodes: copies Frame bytes into the destination buffer.
struct FakeStreamCodec {
    using Frame          = std::vector<uint8_t>;
    using PacketViewRef  = FakePacketView&;

    static constexpr size_t max_overhead = 0;
    static constexpr bool   is_streaming = true;

    // State that forces `decode` to be non-const (exercises the concept's T&).
    size_t frames_decoded = 0;

    std::expected<std::optional<Frame>, ErrorInfo>
    decode(PacketViewRef view, OutputBuffer& /*out*/) {
        if (view.length() == 0) {
            return std::optional<Frame>{}; // Ok(None) — need more data
        }
        Frame f(view.data(), view.data() + view.length());
        view.trim_front(view.length());
        ++frames_decoded;
        return std::optional<Frame>{std::move(f)};
    }

    std::expected<size_t, ErrorInfo>
    encode(uint8_t* dst, size_t cap, Frame frame) {
        if (frame.size() > cap) {
            return std::unexpected(ErrorInfo{Error::BufferFull, "FakeStreamCodec::encode"});
        }
        std::memcpy(dst, frame.data(), frame.size());
        return frame.size();
    }
};

// ============================================================================
// FakeDatagramCodec — emits exactly N frames per datagram via sink callback
// ============================================================================

/// Decodes: treats each 4-byte prefix as a frame count, then emits N empty frames.
/// Encodes: writes the 4-byte count only.
struct FakeDatagramCodec {
    using Frame         = uint32_t;
    using PacketViewRef = FakePacketView&;

    static constexpr size_t max_overhead = 0;
    static constexpr bool   is_streaming = false;

    size_t packets_decoded = 0;

    std::expected<size_t, ErrorInfo>
    decode(PacketViewRef view, OutputBuffer& /*out*/, std::function<void(Frame)> sink) {
        if (view.length() < sizeof(uint32_t)) {
            return std::unexpected(ErrorInfo{Error::CodecBad, "datagram too short"});
        }
        uint32_t n;
        std::memcpy(&n, view.data(), sizeof(n));
        view.trim_front(view.length()); // datagram codec must consume fully
        for (uint32_t i = 0; i < n; ++i) sink(i);
        ++packets_decoded;
        return static_cast<size_t>(n);
    }

    std::expected<size_t, ErrorInfo>
    encode(uint8_t* dst, size_t cap, Frame f) {
        if (cap < sizeof(uint32_t)) {
            return std::unexpected(ErrorInfo{Error::BufferFull, "FakeDatagramCodec::encode"});
        }
        std::memcpy(dst, &f, sizeof(f));
        return sizeof(f);
    }
};

// ============================================================================
// Compile-time concept checks — positive
// ============================================================================

static_assert(StreamCodec<FakeStreamCodec>,
              "FakeStreamCodec must satisfy StreamCodec");
static_assert(Codec<FakeStreamCodec>,
              "FakeStreamCodec must satisfy the union Codec concept");
static_assert(!DatagramCodec<FakeStreamCodec>,
              "FakeStreamCodec must NOT satisfy DatagramCodec (no sink overload)");

static_assert(DatagramCodec<FakeDatagramCodec>,
              "FakeDatagramCodec must satisfy DatagramCodec");
static_assert(Codec<FakeDatagramCodec>,
              "FakeDatagramCodec must satisfy the union Codec concept");
static_assert(!StreamCodec<FakeDatagramCodec>,
              "FakeDatagramCodec must NOT satisfy StreamCodec (wrong decode signature)");

// ============================================================================
// Compile-time concept checks — negative
// ============================================================================

// Primitive types must not satisfy any of the concepts.
static_assert(!StreamCodec<int>);
static_assert(!DatagramCodec<int>);
static_assert(!Codec<int>);
static_assert(!Codec<void*>);

// A struct missing the Frame type must fail.
struct MissingFrame {
    using PacketViewRef = FakePacketView&;
    static constexpr size_t max_overhead = 0;
    static constexpr bool   is_streaming = true;
};
static_assert(!StreamCodec<MissingFrame>);

// A struct with the right shape but wrong return type must fail.
struct WrongDecodeReturn {
    using Frame         = int;
    using PacketViewRef = FakePacketView&;
    static constexpr size_t max_overhead = 0;
    static constexpr bool   is_streaming = true;

    // Returns std::optional<int> instead of expected<optional<int>, ErrorInfo>
    std::optional<Frame> decode(PacketViewRef, OutputBuffer&) { return {}; }
    std::expected<size_t, ErrorInfo> encode(uint8_t*, size_t, Frame) { return 0; }
};
static_assert(!StreamCodec<WrongDecodeReturn>);

// A struct missing is_streaming should fail.
struct MissingTrait {
    using Frame         = int;
    using PacketViewRef = FakePacketView&;
    static constexpr size_t max_overhead = 0;
    std::expected<std::optional<Frame>, ErrorInfo> decode(PacketViewRef, OutputBuffer&) { return {}; }
    std::expected<size_t, ErrorInfo> encode(uint8_t*, size_t, Frame) { return 0; }
};
static_assert(!StreamCodec<MissingTrait>);

// ============================================================================
// OutputBuffer — runtime behaviour
// ============================================================================

TEST(OutputBuffer, AppendAndSize) {
    uint8_t storage[16] = {};
    OutputBuffer ob{storage, sizeof(storage)};
    EXPECT_EQ(ob.available(), 16u);
    EXPECT_EQ(ob.size(), 0u);

    const uint8_t src_bytes[] = {1, 2, 3};
    auto r = ob.append(std::span<const uint8_t>(src_bytes, sizeof(src_bytes)));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(ob.size(), 3u);
    EXPECT_EQ(ob.available(), 13u);
    EXPECT_EQ(ob.data()[0], 1);
    EXPECT_EQ(ob.data()[2], 3);
}

TEST(OutputBuffer, AppendOverflowReturnsBufferFull) {
    uint8_t storage[4] = {};
    OutputBuffer ob{storage, sizeof(storage)};

    const uint8_t src_bytes[] = {0xA, 0xB, 0xC, 0xD, 0xE};
    auto r = ob.append(std::span<const uint8_t>(src_bytes, sizeof(src_bytes)));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::BufferFull);
    // Failed append must not corrupt partial state.
    EXPECT_EQ(ob.size(), 0u);
}

TEST(OutputBuffer, ReserveSucceedsWhenFits) {
    uint8_t storage[8] = {};
    OutputBuffer ob{storage, sizeof(storage)};
    EXPECT_TRUE(ob.reserve(8).has_value());
    EXPECT_TRUE(ob.reserve(0).has_value());
}

TEST(OutputBuffer, ReserveFailsWhenExceedsCap) {
    uint8_t storage[4] = {};
    OutputBuffer ob{storage, sizeof(storage)};
    auto r = ob.reserve(5);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::BufferFull);
}

TEST(OutputBuffer, WritableTailAndCommit) {
    uint8_t storage[8] = {};
    OutputBuffer ob{storage, sizeof(storage)};

    uint8_t* tail = ob.writable_tail(4);
    ASSERT_NE(tail, nullptr);
    tail[0] = 0xDE;
    tail[1] = 0xAD;
    tail[2] = 0xBE;
    tail[3] = 0xEF;
    ob.commit(4);

    EXPECT_EQ(ob.size(), 4u);
    EXPECT_EQ(ob.data()[0], 0xDE);
    EXPECT_EQ(ob.data()[3], 0xEF);
}

TEST(OutputBuffer, WritableTailReturnsNullOnOverflow) {
    uint8_t storage[3] = {};
    OutputBuffer ob{storage, sizeof(storage)};
    EXPECT_EQ(ob.writable_tail(4), nullptr);
}

TEST(OutputBuffer, EmptyAppendIsNoop) {
    uint8_t storage[4] = {};
    OutputBuffer ob{storage, sizeof(storage)};
    auto r = ob.append(std::span<const uint8_t>{});  // empty span
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(ob.size(), 0u);
}

TEST(OutputBuffer, AppendOverflowSafeAgainstSizeMaxSpan) {
    // Pin the doc'd overflow-safe shape: a hostile / malformed `src.size()`
    // close to SIZE_MAX must NOT wrap `len_ + src.size()` into a small value
    // that silently passes a naive `< cap_` check. The OutputBuffer rewrites
    // the comparison as `src.size() > cap_ - len_` to keep both sides
    // unsigned-non-negative under the `len_ <= cap_` invariant. A regression
    // that flipped back to the wrap-able shape would silently corrupt
    // memory; this test catches it without actually dereferencing the
    // bogus pointer (the bytes-too-large branch fires before memcpy).
    uint8_t storage[4] = {};
    OutputBuffer ob{storage, sizeof(storage)};

    // span over a non-null pointer with size SIZE_MAX. The memcpy is
    // never reached because the overflow check returns BufferFull first.
    // We point at `storage` so the span ctor stays defined; the size is
    // the only thing the validator sees.
    std::span<const uint8_t> hostile{storage, std::numeric_limits<size_t>::max()};
    auto r = ob.append(hostile);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::BufferFull);
    // No partial mutation: the rejection must be all-or-nothing.
    EXPECT_EQ(ob.size(), 0u);

    // Same shape for reserve() and writable_tail().
    auto rr = ob.reserve(std::numeric_limits<size_t>::max());
    ASSERT_FALSE(rr.has_value());
    EXPECT_EQ(rr.error().code, Error::BufferFull);

    EXPECT_EQ(ob.writable_tail(std::numeric_limits<size_t>::max()), nullptr);
}

TEST(OutputBuffer, AppendBoundaryExactFitAccepted) {
    // The overflow-safe form `src.size() > cap_ - len_` must accept
    // `src.size() == cap_ - len_` (the exact-fill case). A regression
    // that wrote `>=` instead of `>` would reject the legitimate edge.
    uint8_t storage[4] = {};
    OutputBuffer ob{storage, sizeof(storage)};
    const uint8_t src[] = {1, 2, 3, 4};
    auto r = ob.append(std::span<const uint8_t>(src, sizeof(src)));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(ob.size(), 4u);
    EXPECT_EQ(ob.available(), 0u);

    // One more byte must now hit BufferFull (no silent advancement).
    auto rr = ob.append(std::span<const uint8_t>(src, 1));
    EXPECT_FALSE(rr.has_value());
}

// ============================================================================
// Fake codecs — runtime drive-through
// ============================================================================

TEST(FakeStreamCodec, DecodeEmptyViewReturnsNone) {
    uint8_t buf[1] = {};
    FakePacketView view{buf, 0};
    uint8_t out_storage[4] = {};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    FakeStreamCodec c;
    auto r = c.decode(view, out);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());       // Ok(None)
    EXPECT_EQ(c.frames_decoded, 0u);
}

TEST(FakeStreamCodec, DecodeAndEncodeRoundTrip) {
    uint8_t buf[]  = {'h', 'i'};
    FakePacketView view{buf, sizeof(buf)};
    uint8_t out_storage[8] = {};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    FakeStreamCodec c;
    auto decoded = c.decode(view, out);
    ASSERT_TRUE(decoded.has_value());
    ASSERT_TRUE(decoded->has_value());
    EXPECT_EQ((*decoded)->size(), 2u);
    EXPECT_EQ(c.frames_decoded, 1u);

    // Re-encode into a fresh buffer and verify bytes match.
    uint8_t enc[8] = {};
    auto written = c.encode(enc, sizeof(enc), **decoded);
    ASSERT_TRUE(written.has_value());
    EXPECT_EQ(*written, 2u);
    EXPECT_EQ(enc[0], 'h');
    EXPECT_EQ(enc[1], 'i');
}

TEST(FakeStreamCodec, EncodeOverflow) {
    FakeStreamCodec c;
    FakeStreamCodec::Frame big(16, 0xAA);
    uint8_t small[4];
    auto r = c.encode(small, sizeof(small), big);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::BufferFull);
}

TEST(FakeDatagramCodec, EmitsNFramesViaSink) {
    // Encode "3" as the frame-count prefix
    uint8_t buf[4];
    uint32_t n = 3;
    std::memcpy(buf, &n, sizeof(n));
    FakePacketView view{buf, sizeof(buf)};

    uint8_t out_storage[8] = {};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    FakeDatagramCodec c;
    std::vector<uint32_t> emitted;
    auto r = c.decode(view, out, [&](uint32_t f) { emitted.push_back(f); });
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 3u);
    EXPECT_EQ(emitted.size(), 3u);
    EXPECT_EQ(emitted[0], 0u);
    EXPECT_EQ(emitted[2], 2u);
    EXPECT_EQ(c.packets_decoded, 1u);
    EXPECT_EQ(view.length(), 0u);  // must consume fully
}

TEST(FakeDatagramCodec, ShortDatagramRejected) {
    uint8_t buf[2] = {0, 0};
    FakePacketView view{buf, sizeof(buf)};
    uint8_t out_storage[4] = {};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    FakeDatagramCodec c;
    auto r = c.decode(view, out, [](uint32_t){});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}
