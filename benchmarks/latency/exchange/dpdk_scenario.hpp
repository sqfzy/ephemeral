/// @file exchange/dpdk_scenario.hpp
/// DPDK variants of the three exchange scenarios.
///
/// MarketRxDpdkScenario  — OneWay over DPDK TcpSession (WS framing)
/// OrderRttDpdkScenario  — RTT N-inflight over DPDK TcpSession (WS framing)
/// MdUdpDpdkScenario     — RTT over DPDK UdpSender + raw RX
///
/// These mirror the kernel variants in exchange/scenario.hpp but use
/// eph::dpdk::TcpSession / UdpSender for the network path.
#pragma once

#ifdef EPH_USE_DPDK

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <rte_ethdev.h>
#include <rte_mbuf.h>

#include "eph/dpdk/packet_parse.hpp"
#include "eph/dpdk/tcp.hpp"
#include "eph/dpdk/udp.hpp"
#include "eph/utils/time.hpp"

#include "../core/dpdk_env.hpp"
#include "../core/sample.hpp"
#include "../core/tsc_protocol.hpp"
#include "../core/ws_framing.hpp"

namespace bench::exchange {

// ── 1. MarketRxDpdkScenario ─────────────────────────────────────────────

class MarketRxDpdkScenario {
public:
    explicit MarketRxDpdkScenario(eph::dpdk::TcpSession<>& session)
        : session_(session) {}

    bool prepare() { return true; }

    bool do_one_recv(OneWaySample& out) {
        // Poll until we get a frame with a "T" field (bookTicker).
        rx_accum_.clear();
        uint64_t deadline = eph::utils::TSC::now() +
                            tsc::ns_to_cycles(1'000'000'000ULL); // 1s
        while (eph::utils::TSC::now() < deadline) {
            auto r = session_.poll_rx(
                [&](const uint8_t* data, uint16_t len) {
                    rx_accum_.insert(rx_accum_.end(), data, data + len);
                });
            if (!r) return false;

            // Try to find a complete server frame.
            while (rx_accum_.size() >= 2) {
                auto [hdr, plen] = ws_framing::parse_server_frame(
                    rx_accum_.data(), rx_accum_.size());
                if (plen == 0) break;
                const uint8_t* payload = rx_accum_.data() + hdr;
                uint64_t t = tsc::parse_T(payload, plen);
                rx_accum_.erase(rx_accum_.begin(),
                                rx_accum_.begin() + static_cast<long>(hdr + plen));
                if (t > 0) {
                    out.producer_tsc = t;
                    out.consumer_tsc = eph::utils::TSC::now();
                    return true;
                }
            }
        }
        return false;
    }

    void cleanup() {}

private:
    eph::dpdk::TcpSession<>& session_;
    std::vector<uint8_t> rx_accum_;
};

// ── 2. OrderRttDpdkScenario ─────────────────────────────────────────────

class OrderRttDpdkScenario {
public:
    static constexpr size_t kMaxInflight = 128;

    explicit OrderRttDpdkScenario(eph::dpdk::TcpSession<>& session)
        : session_(session) {}

    bool prepare(size_t inflight) {
        if (inflight == 0 || inflight > kMaxInflight) return false;
        inflight_target_ = inflight;
        slots_.fill(0);
        next_id_ = 1;
        in_flight_ = 0;
        rx_accum_.clear();
        return true;
    }

    bool do_one_rtt(RttSample& out) {
        while (in_flight_ < inflight_target_) {
            if (!send_one()) return false;
        }
        return wait_for_one(out);
    }

    void cleanup() {}

private:
    bool send_one() {
        uint64_t id = next_id_++;
        uint64_t now_tsc = eph::utils::TSC::now();
        slots_[id % kMaxInflight] = now_tsc;

        char body[256];
        int n = std::snprintf(body, sizeof(body),
            R"({"id":%llu,"T_send":%llu,"sym":"BTCUSDT","px":"50000.00","qty":"0.001"})",
            static_cast<unsigned long long>(id),
            static_cast<unsigned long long>(now_tsc));
        if (n <= 0) return false;

        frame_buf_.resize(static_cast<size_t>(n) + 16);
        size_t flen = ws_framing::build_masked_text_frame(
            frame_buf_.data(), body, static_cast<size_t>(n), mask_seed_++);

        size_t sent = 0;
        while (sent < flen) {
            size_t chunk = std::min(flen - sent, size_t{1460});
            auto r = session_.send(frame_buf_.data() + sent,
                                   static_cast<uint16_t>(chunk));
            if (!r) return false;
            sent += *r;
        }
        ++in_flight_;
        return true;
    }

    bool wait_for_one(RttSample& out) {
        uint64_t deadline = eph::utils::TSC::now() +
                            tsc::ns_to_cycles(1'000'000'000ULL);
        while (eph::utils::TSC::now() < deadline) {
            auto r = session_.poll_rx(
                [&](const uint8_t* data, uint16_t len) {
                    rx_accum_.insert(rx_accum_.end(), data, data + len);
                });
            if (!r) return false;

            // Try to parse frames and match order id.
            while (rx_accum_.size() >= 2) {
                auto [hdr, plen] = ws_framing::parse_server_frame(
                    rx_accum_.data(), rx_accum_.size());
                if (plen == 0) break;
                const uint8_t* payload = rx_accum_.data() + hdr;

                uint64_t id = tsc::detail::parse_uint64_after(
                    std::string_view{reinterpret_cast<const char*>(payload), plen},
                    "\"id\":");

                if (id > 0 && slots_[id % kMaxInflight] != 0) {
                    out.client_send_tsc = slots_[id % kMaxInflight];
                    slots_[id % kMaxInflight] = 0;
                    out.server_recv_tsc = tsc::parse_T_recv(payload, plen);
                    out.server_send_tsc = tsc::parse_T(payload, plen);
                    out.client_recv_tsc = eph::utils::TSC::now();
                    --in_flight_;
                    rx_accum_.erase(rx_accum_.begin(),
                                    rx_accum_.begin() + static_cast<long>(hdr + plen));
                    return true;
                }
                rx_accum_.erase(rx_accum_.begin(),
                                rx_accum_.begin() + static_cast<long>(hdr + plen));
            }
        }
        return false;
    }

    eph::dpdk::TcpSession<>& session_;
    size_t inflight_target_ = 1;
    size_t in_flight_ = 0;
    uint64_t next_id_ = 1;
    uint32_t mask_seed_ = 0xCAFEF00D;
    std::array<uint64_t, kMaxInflight> slots_{};
    std::vector<uint8_t> frame_buf_;
    std::vector<uint8_t> rx_accum_;
};

// ── 3. MdUdpDpdkScenario ────────────────────────────────────────────────

/// Same as UdpDpdkRttScenario but with market-data payload fill.
class MdUdpDpdkScenario {
public:
    MdUdpDpdkScenario(eph::dpdk::UdpSender& sender,
                      uint16_t port_id, uint16_t rx_queue,
                      uint16_t expected_dst_port)
        : sender_(sender), port_id_(port_id), rx_queue_(rx_queue),
          expected_dst_port_(expected_dst_port),
          timeout_cycles_(tsc::ns_to_cycles(100'000'000))
    {}

    bool prepare(size_t payload) {
        if (payload < tsc::kBinaryHeaderSize) return false;
        send_buf_.resize(payload);
        recv_buf_.resize(payload);
        payload_size_ = payload;
        constexpr std::string_view tmpl =
            R"({"s":"BTCUSDT","b":"50000.00","a":"50001.00","T":0})";
        size_t cursor = tsc::kBinaryHeaderSize;
        while (cursor < payload) {
            size_t take = std::min(payload - cursor, tmpl.size());
            std::memcpy(send_buf_.data() + cursor, tmpl.data(), take);
            cursor += take;
        }
        return true;
    }

    bool do_one_rtt(RttSample& out) {
        out.client_send_tsc = eph::utils::TSC::now();
        std::memcpy(send_buf_.data(), &out.client_send_tsc, 8);

        if (!sender_.send(send_buf_.data(), static_cast<uint16_t>(payload_size_)))
            return false;

        uint64_t deadline = eph::utils::TSC::now() + timeout_cycles_;
        rte_mbuf* pkts[32];
        while (eph::utils::TSC::now() < deadline) {
            uint16_t nb_rx = rte_eth_rx_burst(port_id_, rx_queue_, pkts, 32);
            for (uint16_t i = 0; i < nb_rx; ++i) {
                auto parsed = eph::dpdk::net::parse_udp_packet(pkts[i]);
                if (parsed &&
                    parsed.dst_port() == expected_dst_port_ &&
                    parsed.payload_len >= tsc::kBinaryHeaderSize) {
                    size_t n = std::min(static_cast<size_t>(parsed.payload_len),
                                        recv_buf_.size());
                    std::memcpy(recv_buf_.data(), parsed.payload, n);
                    for (uint16_t j = 0; j < nb_rx; ++j) rte_pktmbuf_free(pkts[j]);
                    out.client_recv_tsc = eph::utils::TSC::now();
                    std::memcpy(&out.server_recv_tsc, recv_buf_.data() + 8, 8);
                    std::memcpy(&out.server_send_tsc, recv_buf_.data() + 16, 8);
                    return true;
                }
                rte_pktmbuf_free(pkts[i]);
            }
        }
        return false;
    }

    void cleanup() {}

private:
    eph::dpdk::UdpSender& sender_;
    uint16_t port_id_;
    uint16_t rx_queue_;
    uint16_t expected_dst_port_;
    uint64_t timeout_cycles_;
    size_t payload_size_ = 0;
    std::vector<uint8_t> send_buf_;
    std::vector<uint8_t> recv_buf_;
};

} // namespace bench::exchange

#endif // EPH_USE_DPDK
