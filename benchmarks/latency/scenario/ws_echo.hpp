/// @file scenario/ws_echo.hpp
/// WebSocket echo scenario — synchronous send → wait → record.
///
/// Sends a JSON message with T_send TSC field, polls the transport
/// until the echo response (with "echo":true) arrives, then records
/// the round-trip + 4-leg measurements.
///
/// Synchronous design (only ONE in-flight message at a time) avoids the
/// last_send_tsc misattribution that plagued the original async design.

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "framework/bench_runner.hpp"
#include "framework/signal.hpp"
#include "framework/tsc_protocol.hpp"
#include "eph/utils/time.hpp"

namespace bench::scenario {

template <typename Transport>
class WsEchoScenario {
public:
    explicit WsEchoScenario(Transport& transport) : transport_(transport) {
        // Set on_message ONCE in ctor; lambda captures `this`.
        auto& tc = const_cast<eph::net::TransportConfig&>(transport_.config());
        tc.on_message = [this](const uint8_t* data, uint16_t len, uint8_t) {
            // Filter: only count "echo":true responses (defensive against
            // any future mock changes that might re-introduce tick pollution).
            std::string_view json(reinterpret_cast<const char*>(data), len);
            if (json.find("\"echo\":true") == std::string_view::npos) return;

            current_.client_recv_tsc = eph::utils::TSC::now();
            current_.server_send_tsc = tsc::parse_T(data, len);
            current_.server_recv_tsc = tsc::parse_T_recv(data, len);
            got_response_ = true;
        };
    }

    bool prepare(size_t payload) {
        // The JSON envelope is ~48 bytes ({"d":"...","T_send":<20 digits>}).
        // For payload >= 48, pad with `payload - 48` 'X' chars.
        // For smaller payloads, fall back to a single 'X' (envelope dominates).
        padding_.assign(payload > 48 ? payload - 48 : 1, 'X');
        return true;
    }

    bool do_one_round(RoundTrip& rt) {
        got_response_ = false;
        current_ = RoundTrip{};

        rt.client_send_tsc = eph::utils::TSC::now();
        current_.client_send_tsc = rt.client_send_tsc;

        char buf[2048];
        int n = std::snprintf(buf, sizeof(buf),
            R"({"d":"%.*s","T_send":%llu})",
            static_cast<int>(padding_.size()), padding_.c_str(),
            static_cast<unsigned long long>(rt.client_send_tsc));
        [[maybe_unused]] auto err = transport_.send_text(
            std::string_view(buf, static_cast<size_t>(n)));

        // Sync poll until echo arrives or shutdown
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
    std::string padding_;
    bool got_response_ = false;
    RoundTrip current_;
};

} // namespace bench::scenario
