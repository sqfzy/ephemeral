/// @file tcp/scenario.hpp
/// TcpRttScenario — RttScenario impl over a connected TCP fd.
///
/// Each round trip:
///   client stamps client_send_tsc into byte 0..7 of the payload
///   send N bytes
///   recv N bytes (server stamped server_recv into 8..15 and server_send into 16..23)
///   client stamps client_recv_tsc on receipt
///
/// Payload size N is set by `prepare(payload)`. Both buffers grow with the
/// largest sweep value and are reused across rounds.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "client.hpp"
#include "../core/sample.hpp"
#include "../core/tsc_protocol.hpp"
#include "eph/utils/time.hpp"

namespace bench::tcp {

class TcpRttScenario {
public:
    explicit TcpRttScenario(int fd) : fd_(fd) {}

    bool prepare(size_t payload) {
        if (payload < tsc::kBinaryHeaderSize) return false;
        send_buf_.assign(payload, 0xAB);
        recv_buf_.assign(payload, 0);
        return true;
    }

    bool do_one_rtt(RttSample& out) {
        out.client_send_tsc = eph::utils::TSC::now();
        // Stamp client_send into the binary header before send.
        std::memcpy(send_buf_.data(), &out.client_send_tsc, 8);

        if (!send_all(fd_, send_buf_.data(), send_buf_.size())) return false;
        if (!recv_exact(fd_, recv_buf_.data(), recv_buf_.size())) return false;

        out.client_recv_tsc = eph::utils::TSC::now();
        std::memcpy(&out.server_recv_tsc, recv_buf_.data() + 8, 8);
        std::memcpy(&out.server_send_tsc, recv_buf_.data() + 16, 8);
        return true;
    }

    void cleanup() {}

private:
    int fd_;
    std::vector<uint8_t> send_buf_;
    std::vector<uint8_t> recv_buf_;
};

} // namespace bench::tcp
