/// @file ws/dpdk_scenario.hpp
/// WsDpdkRttScenario — RttScenario over DPDK TcpSession with WS framing.
#pragma once

#ifdef EPH_USE_DPDK

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "eph/dpdk/tcp.hpp"
#include "eph/utils/time.hpp"

#include "../core/dpdk_env.hpp"
#include "../core/dpdk_ws_util.hpp"
#include "../core/sample.hpp"
#include "../core/tsc_protocol.hpp"

namespace bench::ws {

/// Perform client-side WS upgrade handshake over a DPDK TcpSession.
[[nodiscard]] inline std::expected<void, std::string>
dpdk_ws_handshake(eph::dpdk::TcpSession<>& session, std::string_view host) {
    std::string req =
        std::string("GET / HTTP/1.1\r\nHost: ") + std::string(host) +
        "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";

    size_t sent = 0;
    while (sent < req.size()) {
        size_t chunk = std::min(req.size() - sent, size_t{1460});
        auto r = session.send(
            reinterpret_cast<const uint8_t*>(req.data()) + sent,
            static_cast<uint16_t>(chunk));
        if (!r) return std::unexpected("ws handshake send: " + r.error());
        sent += *r;
    }

    std::string resp;
    resp.reserve(512);
    uint64_t deadline = eph::utils::TSC::now() + tsc::ns_to_cycles(2'000'000'000ULL);
    while (eph::utils::TSC::now() < deadline) {
        auto r = session.poll_rx(
            [&](const uint8_t* data, uint16_t len) {
                resp.append(reinterpret_cast<const char*>(data), len);
            });
        if (!r) return std::unexpected("ws handshake recv: " + r.error());
        if (resp.find("\r\n\r\n") != std::string::npos) break;
    }
    if (resp.find("101") == std::string::npos) {
        return std::unexpected("ws handshake rejected (no 101)");
    }
    return {};
}

class WsDpdkRttScenario {
public:
    explicit WsDpdkRttScenario(eph::dpdk::TcpSession<>& session)
        : session_(session) {}

    bool prepare(size_t payload) {
        if (payload < 64) return false;
        target_payload_ = payload;
        return true;
    }

    bool do_one_rtt(RttSample& out) {
        out.client_send_tsc = eph::utils::TSC::now();

        char prefix[80];
        int n = std::snprintf(prefix, sizeof(prefix),
            R"({"T_send":%llu,"pad":")",
            static_cast<unsigned long long>(out.client_send_tsc));
        if (n <= 0) return false;
        size_t prefix_len = static_cast<size_t>(n);
        size_t suffix_len = 2;
        if (prefix_len + suffix_len >= target_payload_) return false;
        size_t pad_len = target_payload_ - prefix_len - suffix_len;

        json_buf_.resize(target_payload_);
        std::memcpy(json_buf_.data(), prefix, prefix_len);
        std::memset(json_buf_.data() + prefix_len, 'x', pad_len);
        json_buf_[prefix_len + pad_len + 0] = '"';
        json_buf_[prefix_len + pad_len + 1] = '}';

        frame_buf_.resize(target_payload_ + 16);
        size_t frame_len = dpdk_ws::build_masked_frame(
            frame_buf_.data(), json_buf_.data(), target_payload_, mask_seed_++);

        size_t sent = 0;
        while (sent < frame_len) {
            size_t chunk = std::min(frame_len - sent, size_t{1460});
            auto r = session_.send(frame_buf_.data() + sent,
                                   static_cast<uint16_t>(chunk));
            if (!r) return false;
            sent += *r;
        }

        rx_accum_.clear();
        uint64_t deadline = eph::utils::TSC::now() +
                            tsc::ns_to_cycles(100'000'000ULL);
        while (eph::utils::TSC::now() < deadline) {
            auto r = session_.poll_rx(
                [&](const uint8_t* data, uint16_t len) {
                    rx_accum_.insert(rx_accum_.end(), data, data + len);
                });
            if (!r) return false;
            auto [hdr, plen] = dpdk_ws::consume_frame(rx_accum_);
            if (plen > 0) {
                out.client_recv_tsc = eph::utils::TSC::now();
                const uint8_t* payload = rx_accum_.data() + hdr;
                out.server_recv_tsc = tsc::parse_T_recv(payload, plen);
                out.server_send_tsc = tsc::parse_T(payload, plen);
                return true;
            }
        }
        return false;
    }

    void cleanup() {}

private:
    eph::dpdk::TcpSession<>& session_;
    size_t target_payload_ = 0;
    uint32_t mask_seed_ = 0xDEADBEEF;
    std::vector<uint8_t> json_buf_;
    std::vector<uint8_t> frame_buf_;
    std::vector<uint8_t> rx_accum_;
};

} // namespace bench::ws

#endif // EPH_USE_DPDK
