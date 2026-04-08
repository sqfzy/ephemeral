/// @file scenario/tcp_echo.hpp
/// Raw TCP echo scenario implementation (fixed-size framing).
///
/// Both sides agree on payload size via configuration. Each round trip:
///   - client sends exactly N bytes (with TSC stamp at offset 0)
///   - server reads exactly N bytes, stamps recv/send TSCs, sends N back
///   - client reads exactly N bytes
///
/// The TcpStream concept must provide:
///   bool send_all(const void* data, size_t n)  → false on error
///   bool recv_exact(uint8_t* buf, size_t n)    → false on error / disconnect

#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "framework/bench_runner.hpp"
#include "framework/tsc_protocol.hpp"
#include "eph/utils/time.hpp"

namespace bench::scenario {

template <typename TcpStream>
class TcpEchoScenario {
public:
    explicit TcpEchoScenario(TcpStream& stream) : stream_(stream) {}

    bool prepare(size_t payload) {
        if (payload < tsc::kBinaryHeaderSize) return false;
        send_buf_.assign(payload, 0xAB);
        recv_buf_.assign(payload, 0);
        return true;
    }

    bool do_one_round(RoundTrip& rt) {
        rt.client_send_tsc = eph::utils::TSC::now();
        std::memcpy(send_buf_.data(), &rt.client_send_tsc, 8);

        if (!stream_.send_all(send_buf_.data(), send_buf_.size())) {
            return false;
        }
        if (!stream_.recv_exact(recv_buf_.data(), recv_buf_.size())) {
            return false;
        }

        rt.client_recv_tsc = eph::utils::TSC::now();
        std::memcpy(&rt.server_recv_tsc, recv_buf_.data() + 8, 8);
        std::memcpy(&rt.server_send_tsc, recv_buf_.data() + 16, 8);
        return true;
    }

    void cleanup() {}

private:
    TcpStream& stream_;
    std::vector<uint8_t> send_buf_;
    std::vector<uint8_t> recv_buf_;
};

} // namespace bench::scenario
