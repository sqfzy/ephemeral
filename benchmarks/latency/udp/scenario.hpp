/// @file udp/scenario.hpp
/// UdpRttScenario — RttScenario over a connected UDP endpoint with binary
/// 24B TSC header. A single dropped packet returns false from do_one_rtt
/// (the runner will simply skip the round and continue).
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "client.hpp"
#include "../core/sample.hpp"
#include "../core/tsc_protocol.hpp"
#include "eph/utils/time.hpp"

namespace bench::udp {

class UdpRttScenario {
public:
    explicit UdpRttScenario(Endpoint& ep) : ep_(ep) {}

    bool prepare(size_t payload) {
        if (payload < tsc::kBinaryHeaderSize) return false;
        send_buf_.assign(payload, 0xCD);
        recv_buf_.assign(payload, 0);
        return true;
    }

    bool do_one_rtt(RttSample& out) {
        out.client_send_tsc = eph::utils::TSC::now();
        std::memcpy(send_buf_.data(), &out.client_send_tsc, 8);

        if (!sendto_one(ep_, send_buf_.data(), send_buf_.size())) return false;
        ssize_t n = recvfrom_one(ep_, recv_buf_.data(), recv_buf_.size());
        if (n < static_cast<ssize_t>(tsc::kBinaryHeaderSize)) return false;

        out.client_recv_tsc = eph::utils::TSC::now();
        std::memcpy(&out.server_recv_tsc, recv_buf_.data() + 8, 8);
        std::memcpy(&out.server_send_tsc, recv_buf_.data() + 16, 8);
        return true;
    }

    void cleanup() {}

private:
    Endpoint& ep_;
    std::vector<uint8_t> send_buf_;
    std::vector<uint8_t> recv_buf_;
};

} // namespace bench::udp
