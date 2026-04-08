/// @file scenario/order_rtt.hpp
/// WS order submit + execution report RTT — synchronous version.
///
/// Sends an order JSON, polls until the matching executionReport
/// arrives, records 4-leg latency. The mock server (order_mode=true)
/// also pushes market ticks; the on_message filter ignores those.
///
/// Decision: order_rtt is now SYNCHRONOUS (one in-flight order at a
/// time), matching ws_echo. The original async design with
/// `--order-interval` was correct only when interval ≫ RTT; for the
/// general case sync is the only correct approach. The CLI parameter
/// is silently accepted but ignored.

#pragma once

#include <cstdint>
#include <cstdio>
#include <string_view>

#include "framework/bench_runner.hpp"
#include "framework/signal.hpp"
#include "framework/tsc_protocol.hpp"
#include "eph/utils/time.hpp"

namespace bench::scenario {

template <typename Transport>
class OrderRttScenario {
public:
    explicit OrderRttScenario(Transport& transport) : transport_(transport) {
        auto& tc = const_cast<eph::net::TransportConfig&>(transport_.config());
        tc.on_message = [this](const uint8_t* data, uint16_t len, uint8_t) {
            // Filter: only count executionReport messages. The mock server
            // also pushes market ticks (order_mode=true) that we want to ignore.
            std::string_view json(reinterpret_cast<const char*>(data), len);
            if (json.find("\"e\":\"executionReport\"") == std::string_view::npos) return;

            current_.client_recv_tsc = eph::utils::TSC::now();
            current_.server_send_tsc = tsc::parse_T(data, len);
            current_.server_recv_tsc = tsc::parse_T_recv(data, len);
            got_response_ = true;
        };
    }

    bool prepare(size_t /*payload*/) { return true; }

    bool do_one_round(RoundTrip& rt) {
        got_response_ = false;
        current_ = RoundTrip{};

        rt.client_send_tsc = eph::utils::TSC::now();
        current_.client_send_tsc = rt.client_send_tsc;

        char buf[256];
        int n = std::snprintf(buf, sizeof(buf),
            R"({"method":"order.place","symbol":"BTCUSDT",)"
            R"("side":"BUY","price":"50000.00",)"
            R"("quantity":"0.001","T_send":%llu})",
            static_cast<unsigned long long>(rt.client_send_tsc));
        [[maybe_unused]] auto err = transport_.send_text(
            std::string_view(buf, static_cast<size_t>(n)));

        while (!got_response_
               && g_running.load(std::memory_order_relaxed)
               && transport_.is_running()) {
            [[maybe_unused]] auto _ = transport_.poll();
        }
        if (!got_response_) return false;

        rt = current_;
        return true;
    }

    void cleanup() {}

private:
    Transport& transport_;
    bool got_response_ = false;
    RoundTrip current_;
};

} // namespace bench::scenario
