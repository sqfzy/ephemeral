/// @file scenario/udp_relay.hpp
/// UDP relay (store-and-forward) scenario.
///
/// Topology:  Client → Relay → Client
///   - Client sends to relay's listen port
///   - Relay timestamps recv/send, forwards to client's recv port
///   - Client receives forwarded packet
///
/// The Sender and Receiver template parameters are the same concepts as
/// UdpEchoScenario — what's different is that they target/listen on
/// different (port, ip) combinations: Sender targets the relay's listen
/// port; Receiver listens on the client's recv port.

#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "framework/bench_runner.hpp"
#include "framework/tsc_protocol.hpp"
#include "eph/utils/time.hpp"

namespace bench::scenario {

template <typename Sender, typename Receiver>
class UdpRelayScenario {
public:
    UdpRelayScenario(Sender& sender, Receiver& receiver)
        : sender_(sender), receiver_(receiver) {}

    bool prepare(size_t payload) {
        if (payload < tsc::kBinaryHeaderSize) return false;
        send_buf_.assign(payload, 0xAB);
        return true;
    }

    bool do_one_round(RoundTrip& rt) {
        rt.client_send_tsc = eph::utils::TSC::now();
        std::memcpy(send_buf_.data(), &rt.client_send_tsc, 8);

        if (!sender_.send(send_buf_.data(), static_cast<uint16_t>(send_buf_.size()))) {
            return false;
        }

        ssize_t n = receiver_.recv(recv_buf_, sizeof(recv_buf_));
        if (n < static_cast<ssize_t>(tsc::kBinaryHeaderSize)) {
            return false;
        }

        rt.client_recv_tsc = eph::utils::TSC::now();
        // For relay, server_recv/send fields actually hold relay timestamps:
        std::memcpy(&rt.server_recv_tsc, recv_buf_ + 8, 8);
        std::memcpy(&rt.server_send_tsc, recv_buf_ + 16, 8);
        return true;
    }

    void cleanup() {}

private:
    Sender& sender_;
    Receiver& receiver_;
    std::vector<uint8_t> send_buf_;
    uint8_t recv_buf_[2048];
};

} // namespace bench::scenario
