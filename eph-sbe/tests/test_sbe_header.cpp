#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "eph/sbe.hpp"

using namespace eph::sbe;

// ---------------------------------------------------------------------------
// Helper: write a little-endian uint16 into a byte buffer.
// ---------------------------------------------------------------------------
static void put_le16(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

// Build a bare 8-byte SBE message header.
static std::array<uint8_t, kHeaderSize>
make_header(uint16_t block_length, uint16_t template_id, uint16_t schema_id,
            uint16_t version) {
    std::array<uint8_t, kHeaderSize> buf{};
    put_le16(buf.data() + 0, block_length);
    put_le16(buf.data() + 2, template_id);
    put_le16(buf.data() + 4, schema_id);
    put_le16(buf.data() + 6, version);
    return buf;
}

// ===========================================================================
// parse_header
// ===========================================================================

TEST(SbeHeader, parse_valid_decodes_all_fields) {
    auto buf = make_header(/*block_length=*/0, /*template_id=*/212,
                           /*schema_id=*/3, /*version=*/2);
    auto h = parse_header(buf.data(), buf.size());
    ASSERT_TRUE(h.has_value()) << parse_error_name(h.error());
    EXPECT_EQ(h->block_length, 0);
    EXPECT_EQ(h->template_id, 212);
    EXPECT_EQ(h->schema_id, 3);
    EXPECT_EQ(h->version, 2);
}

TEST(SbeHeader, parse_truncated_returns_incomplete) {
    auto buf = make_header(0, 212, 3, 2);
    auto h = parse_header(buf.data(), kHeaderSize - 1); // one byte short
    ASSERT_FALSE(h.has_value());
    EXPECT_EQ(h.error(), ParseError::kIncomplete);
}

TEST(SbeHeader, parse_empty_returns_incomplete) {
    auto h = parse_header(nullptr, 0);
    ASSERT_FALSE(h.has_value());
    EXPECT_EQ(h.error(), ParseError::kIncomplete);
}

// ===========================================================================
// parse() — MessageView
// ===========================================================================

TEST(SbeParse, message_view_exposes_header_and_body_window) {
    // 8-byte header + 4 trailing body bytes.
    auto hdr = make_header(/*block_length=*/0, /*template_id=*/212,
                           /*schema_id=*/3, /*version=*/2);
    std::array<uint8_t, kHeaderSize + 4> buf{};
    std::copy(hdr.begin(), hdr.end(), buf.begin());

    auto v = parse(buf.data(), buf.size());
    ASSERT_TRUE(v.has_value()) << parse_error_name(v.error());
    EXPECT_EQ(v->template_id, 212);
    EXPECT_EQ(v->schema_id, 3);
    EXPECT_EQ(v->version, 2);
    EXPECT_EQ(v->data, buf.data());
    EXPECT_EQ(v->length, buf.size());
    EXPECT_EQ(v->body(), buf.data() + kHeaderSize);
    EXPECT_EQ(v->body_len(), 4u);
}

TEST(SbeParse, incomplete_header_propagates_error) {
    std::array<uint8_t, 3> buf{};
    auto v = parse(buf.data(), buf.size());
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error(), ParseError::kIncomplete);
}

// ===========================================================================
// dispatch() scaffold — unknown template id routes to msg::Unknown
// ===========================================================================

TEST(SbeDispatch, unknown_template_routes_to_unknown_tag) {
    auto hdr = make_header(0, /*template_id=*/9999, 3, 2);
    auto v = parse(hdr.data(), hdr.size());
    ASSERT_TRUE(v.has_value());

    bool saw_unknown = false;
    dispatch(*v, [&](auto tag, const MessageView&) {
        if constexpr (std::is_same_v<decltype(tag), msg::Unknown>)
            saw_unknown = true;
    });
    EXPECT_TRUE(saw_unknown);
}
