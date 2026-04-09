/// @file udp/dpdk_scenario.hpp
/// UdpDpdkRttScenario — RttScenario over DPDK UdpSender + raw rte_eth_rx_burst.
///
/// TX uses eph::dpdk::UdpSender for zero-copy packet construction.
/// RX polls rte_eth_rx_burst with a TSC-deadline timeout, filters by
/// dst_port, and copies payload into the caller's buffer.
#pragma once

#ifdef EPH_USE_DPDK

#include <cstdint>
#include <cstring>
#include <vector>

#include <rte_ethdev.h>
#include <rte_mbuf.h>

#include "eph/dpdk/packet_parse.hpp"
#include "eph/dpdk/udp.hpp"
#include "eph/utils/time.hpp"

#include "../core/dpdk_env.hpp"
#include "../core/sample.hpp"
#include "../core/tsc_protocol.hpp"

namespace bench::udp {

class UdpDpdkRttScenario {
public:
    UdpDpdkRttScenario(eph::dpdk::UdpSender& sender,
                       uint16_t port_id, uint16_t rx_queue,
                       uint16_t expected_dst_port,
                       uint64_t timeout_ns = 100'000'000) // 100 ms default
        : sender_(sender)
        , port_id_(port_id)
        , rx_queue_(rx_queue)
        , expected_dst_port_(expected_dst_port)
        , timeout_cycles_(tsc::ns_to_cycles(timeout_ns))
    {}

    bool prepare(size_t payload) {
        if (payload < tsc::kBinaryHeaderSize) return false;
        send_buf_.assign(payload, 0xCD);
        recv_buf_.assign(payload, 0);
        payload_size_ = payload;
        return true;
    }

    bool do_one_rtt(RttSample& out) {
        out.client_send_tsc = eph::utils::TSC::now();
        std::memcpy(send_buf_.data(), &out.client_send_tsc, 8);

        if (!sender_.send(send_buf_.data(), static_cast<uint16_t>(payload_size_))) {
            return false;
        }

        // Poll RX until a matching UDP packet arrives or timeout.
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
                    // Free all mbufs.
                    for (uint16_t j = 0; j < nb_rx; ++j) rte_pktmbuf_free(pkts[j]);
                    out.client_recv_tsc = eph::utils::TSC::now();
                    std::memcpy(&out.server_recv_tsc, recv_buf_.data() + 8, 8);
                    std::memcpy(&out.server_send_tsc, recv_buf_.data() + 16, 8);
                    return true;
                }
                rte_pktmbuf_free(pkts[i]);
            }
        }
        return false; // timeout
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

} // namespace bench::udp

#endif // EPH_USE_DPDK
