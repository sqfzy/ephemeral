/// @file core/dpdk_ws_util.hpp
/// Shared WS framing utilities for DPDK scenarios.
/// Build masked client frame + parse unmasked server frame.
#pragma once

#ifdef EPH_USE_DPDK

#include <cstdint>
#include <cstring>
#include <vector>

namespace bench::dpdk_ws {

/// Build a masked client→server text frame. Returns total bytes written.
inline size_t build_masked_frame(uint8_t* out, const void* payload,
                                 size_t len, uint32_t seed) noexcept {
    out[0] = 0x81;
    size_t cursor;
    if (len < 126) {
        out[1] = static_cast<uint8_t>(0x80 | len);
        cursor = 2;
    } else if (len < 65536) {
        out[1] = static_cast<uint8_t>(0x80 | 126);
        out[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
        out[3] = static_cast<uint8_t>(len & 0xFF);
        cursor = 4;
    } else {
        out[1] = static_cast<uint8_t>(0x80 | 127);
        for (int i = 0; i < 8; ++i)
            out[2 + i] = static_cast<uint8_t>((len >> (56 - 8*i)) & 0xFF);
        cursor = 10;
    }
    uint8_t mask[4];
    mask[0] = static_cast<uint8_t>(seed);
    mask[1] = static_cast<uint8_t>(seed >> 8);
    mask[2] = static_cast<uint8_t>(seed >> 16);
    mask[3] = static_cast<uint8_t>(seed >> 24);
    std::memcpy(out + cursor, mask, 4);
    cursor += 4;
    auto* src = static_cast<const uint8_t*>(payload);
    for (size_t i = 0; i < len; ++i)
        out[cursor + i] = src[i] ^ mask[i & 3];
    return cursor + len;
}

/// Compute the WS frame header size from the first 2 bytes.
inline size_t frame_header_size(const uint8_t* buf, size_t buf_len) noexcept {
    if (buf_len < 2) return 0;
    uint8_t b1 = buf[1] & 0x7F;
    if (b1 < 126) return 2;
    if (b1 == 126) return (buf_len >= 4) ? 4 : 0;
    return (buf_len >= 10) ? 10 : 0;
}

/// Try to parse a complete unmasked server frame from buf.
/// Returns payload length or 0 if incomplete/invalid.
inline size_t try_parse_server_frame(const uint8_t* buf, size_t buf_len) noexcept {
    size_t hdr = frame_header_size(buf, buf_len);
    if (hdr == 0) return 0;
    uint64_t plen = buf[1] & 0x7F;
    if (plen == 126) {
        plen = (uint64_t(buf[2]) << 8) | buf[3];
    } else if (plen == 127) {
        plen = 0;
        for (int i = 0; i < 8; ++i) plen = (plen << 8) | buf[2 + i];
    }
    if (buf_len < hdr + plen) return 0;
    return static_cast<size_t>(plen);
}

/// Consume one server frame from a vector accumulator. Returns (header_size, payload_len)
/// or (0,0) if no complete frame is available. Caller should erase hdr+plen bytes.
inline std::pair<size_t, size_t> consume_frame(const std::vector<uint8_t>& buf) noexcept {
    size_t plen = try_parse_server_frame(buf.data(), buf.size());
    if (plen == 0) return {0, 0};
    size_t hdr = frame_header_size(buf.data(), buf.size());
    return {hdr, plen};
}

} // namespace bench::dpdk_ws

#endif // EPH_USE_DPDK
