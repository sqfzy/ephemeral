/// @file tcp/dpdk_scenario.hpp
/// TcpDpdkRttScenario — RttScenario over DPDK TcpSession.
///
/// send uses session.send() (builds TCP packet + rte_eth_tx_burst).
/// recv uses session.poll_rx() with a callback that accumulates bytes.
#pragma once

#ifdef EPH_USE_DPDK

#include <cstdint>
#include <cstring>
#include <vector>

#include "eph/dpdk/tcp.hpp"
#include "eph/utils/time.hpp"

#include "../core/dpdk_env.hpp"
#include "../core/sample.hpp"
#include "../core/tsc_protocol.hpp"

namespace bench::tcp {

class TcpDpdkRttScenario {
public:
    explicit TcpDpdkRttScenario(eph::dpdk::TcpSession<>& session)
        : session_(session) {}

    bool prepare(size_t payload) {
        if (payload < tsc::kBinaryHeaderSize) return false;
        send_buf_.assign(payload, 0xAB);
        recv_buf_.assign(payload, 0);
        payload_size_ = payload;
        return true;
    }

    bool do_one_rtt(RttSample& out) {
        out.client_send_tsc = eph::utils::TSC::now();
        std::memcpy(send_buf_.data(), &out.client_send_tsc, 8);

        // Send all bytes (may need multiple send calls if > MSS).
        size_t sent = 0;
        while (sent < payload_size_) {
            size_t chunk = std::min(payload_size_ - sent, size_t{1460});
            auto r = session_.send(send_buf_.data() + sent,
                                   static_cast<uint16_t>(chunk));
            if (!r) return false;
            sent += *r;
        }

        // Receive exactly payload_size_ bytes via poll_rx callback.
        size_t got = 0;
        uint64_t deadline = eph::utils::TSC::now() +
                            tsc::ns_to_cycles(100'000'000); // 100 ms timeout
        while (got < payload_size_ && eph::utils::TSC::now() < deadline) {
            auto r = session_.poll_rx(
                [&](const uint8_t* data, uint16_t len) {
                    size_t take = std::min(static_cast<size_t>(len),
                                           payload_size_ - got);
                    std::memcpy(recv_buf_.data() + got, data, take);
                    got += take;
                });
            if (!r) return false;
        }
        if (got < payload_size_) return false;

        out.client_recv_tsc = eph::utils::TSC::now();
        std::memcpy(&out.server_recv_tsc, recv_buf_.data() + 8, 8);
        std::memcpy(&out.server_send_tsc, recv_buf_.data() + 16, 8);
        return true;
    }

    void cleanup() {}

private:
    eph::dpdk::TcpSession<>& session_;
    size_t payload_size_ = 0;
    std::vector<uint8_t> send_buf_;
    std::vector<uint8_t> recv_buf_;
};

} // namespace bench::tcp

#endif // EPH_USE_DPDK
