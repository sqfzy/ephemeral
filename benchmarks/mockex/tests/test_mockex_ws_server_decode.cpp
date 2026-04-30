/// @file tests/test_mockex_ws_server_decode.cpp
/// Unit tests for `mockex::ws::decode_frame` driven by an in-memory
/// IoStream mock. Covers the malformed-input safety contract: a
/// hostile or buggy client must never be able to OOM/terminate the
/// mock by claiming a giant payload length in the WS header.
///
/// `decode_frame` is `noexcept`, so any unguarded `std::vector::resize
/// (length)` from a 64-bit untrusted length will call `std::terminate`
/// on `bad_alloc`. The test below builds a header claiming
/// `length = UINT64_MAX/2` (well within RFC 6455's 63-bit field) and
/// asserts the function returns `nullopt` cleanly instead of allocating.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "mockex/ws_server.hpp"

namespace {

/// In-memory IoStream stub satisfying the duck-typed contract used by
/// `mockex::ws::decode_frame` — only `read_exact` is invoked on the
/// decode path. Acts as a queue of bytes; returns false (EOF) once
/// exhausted.
class MemIoStream {
public:
    explicit MemIoStream(std::vector<uint8_t> data) noexcept
        : data_(std::move(data)) {}

    [[nodiscard]] bool read_exact(void* buf, std::size_t n) noexcept {
        if (pos_ + n > data_.size()) {
            // Mark exhausted so subsequent reads also fail rather than
            // partial-fill.
            pos_ = data_.size();
            return false;
        }
        std::memcpy(buf, data_.data() + pos_, n);
        pos_ += n;
        return true;
    }

    [[nodiscard]] std::size_t pos() const noexcept { return pos_; }

private:
    std::vector<uint8_t> data_;
    std::size_t          pos_ = 0;
};

} // namespace

/// Oversized payload claim attack: opcode=binary, MASK=1, raw_len=127
/// (8-byte extended length) encoding length=2^62. We supply the full
/// header AND a 4-byte mask key so the decoder reaches the
/// `payload.resize(length)` step. Without a bound, the allocator
/// throws bad_alloc inside a `noexcept` function, calling
/// `std::terminate` — i.e. one malformed frame crashes mockex.
///
/// With the bound in place the decoder must short-circuit cleanly and
/// return `nullopt`, leaving the process alive.
TEST(MockexWsServerDecode, RejectsOversizedPayloadLength) {
    std::vector<uint8_t> wire;
    // FIN=1 + opcode=binary
    wire.push_back(0x82);
    // MASK=1 + raw_len=127
    wire.push_back(0x80 | 127);
    // 8-byte length = 2^62 (huge but with high bit clear per RFC).
    const uint64_t huge = (1ull << 62);
    for (int i = 7; i >= 0; --i) {
        wire.push_back(static_cast<uint8_t>((huge >> (i * 8)) & 0xFFu));
    }
    // 4-byte mask key so the decoder gets past `read_exact(mask, 4)`
    // and into the payload allocation.
    wire.insert(wire.end(), {0xAA, 0xBB, 0xCC, 0xDD});

    MemIoStream io{std::move(wire)};
    auto frame = mockex::ws::decode_frame(io);
    EXPECT_FALSE(frame.has_value())
        << "decoder must reject oversized payload claim, not allocate "
        << huge << " bytes";
}

/// 4 GiB length (length=2^32) — also far above any reasonable mock
/// buffer. Same expectation: clean reject without bad_alloc.
TEST(MockexWsServerDecode, RejectsFourGibPayloadLength) {
    std::vector<uint8_t> wire;
    wire.push_back(0x82);
    wire.push_back(0x80 | 127);
    const uint64_t four_gib = (1ull << 32);
    for (int i = 7; i >= 0; --i) {
        wire.push_back(static_cast<uint8_t>((four_gib >> (i * 8)) & 0xFFu));
    }
    wire.insert(wire.end(), {0xAA, 0xBB, 0xCC, 0xDD});
    MemIoStream io{std::move(wire)};
    auto frame = mockex::ws::decode_frame(io);
    EXPECT_FALSE(frame.has_value());
}

/// Sanity: a normal small masked frame round-trips cleanly. Confirms
/// the bounds check doesn't false-reject in-spec input.
TEST(MockexWsServerDecode, AcceptsSmallMaskedFrame) {
    std::vector<uint8_t> wire;
    wire.push_back(0x82);                 // FIN + binary
    wire.push_back(0x80 | 4);             // masked, len=4
    const uint8_t mask[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    wire.insert(wire.end(), mask, mask + 4);
    const uint8_t plain[4] = {0x01, 0x02, 0x03, 0x04};
    for (int i = 0; i < 4; ++i)
        wire.push_back(plain[i] ^ mask[i & 3]);

    MemIoStream io{std::move(wire)};
    auto frame = mockex::ws::decode_frame(io);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->opcode, mockex::ws::kOpcodeBinary);
    ASSERT_EQ(frame->payload.size(), 4u);
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(frame->payload[i], plain[i]);
}

/// Sanity: unmasked client frame is rejected (RFC 6455 §5.3). Already
/// tested implicitly elsewhere; pinned here as a direct unit test so
/// the file documents the full safety surface.
TEST(MockexWsServerDecode, RejectsUnmaskedClientFrame) {
    std::vector<uint8_t> wire;
    wire.push_back(0x82);
    wire.push_back(0x00 | 4);             // MASK=0 — client must mask
    wire.push_back(0xDE); wire.push_back(0xAD);
    wire.push_back(0xBE); wire.push_back(0xEF);

    MemIoStream io{std::move(wire)};
    auto frame = mockex::ws::decode_frame(io);
    EXPECT_FALSE(frame.has_value());
}

/// `extract_sec_ws_key` strips RFC 7230 OWS on both sides of the
/// header value. Without the trailing-OWS strip a non-conforming
/// client emitting "key  \r\n" would compute a Sec-WebSocket-Accept
/// against "key  " (with trailing spaces) and the handshake would
/// fail for an opaque reason.
TEST(MockexWsServerDecode, ExtractKeyStripsLeadingAndTrailingOws) {
    // Leading two spaces, trailing two tabs.
    std::string req =
        "GET / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Sec-WebSocket-Key:  dGhlIHNhbXBsZSBub25jZQ==\t\t\r\n"
        "\r\n";
    auto key = mockex::ws::detail::extract_sec_ws_key(req);
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(*key, "dGhlIHNhbXBsZSBub25jZQ==");
}

TEST(MockexWsServerDecode, ExtractKeyHandlesAllWhitespaceValueAsEmpty) {
    // A pathological "value is only whitespace" — leading strip pushes
    // p past `e` would underflow if not guarded. We expect either
    // empty string or rejection; pin behaviour: empty string is fine
    // (the SHA1 path will then mismatch any real client).
    std::string req =
        "GET / HTTP/1.1\r\n"
        "Sec-WebSocket-Key:   \r\n"
        "\r\n";
    auto key = mockex::ws::detail::extract_sec_ws_key(req);
    ASSERT_TRUE(key.has_value());
    EXPECT_TRUE(key->empty());
}
