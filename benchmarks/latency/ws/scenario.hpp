/// @file ws/scenario.hpp
/// WsRttScenario — RttScenario over a plain WebSocket text channel.
///
/// Each round trip the client sends a small JSON object with a "T_send"
/// field carrying the client TSC, then a fixed-size padding string to hit
/// the requested payload byte budget. The mock echoes back a JSON with
/// "T_recv", "T_send" (server send), and the original "T_send" preserved.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "client.hpp"
#include "../core/sample.hpp"
#include "../core/tsc_protocol.hpp"
#include "eph/utils/time.hpp"

namespace bench::ws {

class WsRttScenario {
public:
    explicit WsRttScenario(int fd) : fd_(fd) {}

    bool prepare(size_t payload) {
        if (payload < 64) return false; // not enough room for the JSON envelope
        target_payload_ = payload;
        return true;
    }

    bool do_one_rtt(RttSample& out) {
        out.client_send_tsc = eph::utils::TSC::now();

        // Build a fixed-prefix JSON with T_send first, then a padding string
        // that brings the total payload to target_payload_ bytes.
        char prefix[80];
        int n = std::snprintf(prefix, sizeof(prefix),
            R"({"T_send":%llu,"pad":")",
            static_cast<unsigned long long>(out.client_send_tsc));
        if (n <= 0) return false;
        size_t prefix_len = static_cast<size_t>(n);
        size_t suffix_len = 2; // "}\0
        if (prefix_len + suffix_len >= target_payload_) return false;
        size_t pad_len = target_payload_ - prefix_len - suffix_len;

        json_buf_.resize(target_payload_);
        std::memcpy(json_buf_.data(), prefix, prefix_len);
        std::memset(json_buf_.data() + prefix_len, 'x', pad_len);
        json_buf_[prefix_len + pad_len + 0] = '"';
        json_buf_[prefix_len + pad_len + 1] = '}';

        frame_buf_.resize(target_payload_ + 16);
        size_t frame_len = build_masked_text_frame(
            frame_buf_.data(), json_buf_.data(), target_payload_, mask_seed_++);

        if (!detail::send_all(fd_, frame_buf_.data(), frame_len)) return false;

        recv_buf_.resize(2048);
        size_t plen = recv_one_frame(fd_, recv_buf_.data(), recv_buf_.size());
        if (plen == 0) return false;

        out.client_recv_tsc = eph::utils::TSC::now();
        out.server_recv_tsc = tsc::parse_T_recv(recv_buf_.data(), plen);
        out.server_send_tsc = tsc::parse_T(recv_buf_.data(), plen);
        return true;
    }

    void cleanup() {}

private:
    int fd_;
    size_t target_payload_ = 0;
    uint32_t mask_seed_ = 0xDEADBEEF;
    std::vector<uint8_t> json_buf_;
    std::vector<uint8_t> frame_buf_;
    std::vector<uint8_t> recv_buf_;
};

} // namespace bench::ws
