/// @file scenario/market_rx.hpp
/// WS market data receive scenario — 1-leg pipeline latency.
///
/// Server pushes bookTicker JSON at `tick_interval`. Client measures
/// pipeline latency: time from server's TSC stamp ("T" field) to the
/// client's receive timestamp.
///
/// This is a 1-leg measurement; only the RTT histogram is populated.
/// TX/RX/Server legs remain empty (samples=0).
///
/// Special: do_one_round() does not "send" — it just polls for the next
/// pushed message and treats THAT as a complete round trip. The RoundTrip
/// is filled with fake send/recv tsc values that yield the correct
/// pipeline latency in the RTT leg only.

#pragma once

#include <cstdint>
#include <string_view>

#include "framework/bench_runner.hpp"
#include "framework/signal.hpp"
#include "framework/tsc_protocol.hpp"
#include "eph/utils/time.hpp"

namespace bench::scenario {

template <typename Transport>
class MarketRxScenario {
public:
    explicit MarketRxScenario(Transport& transport) : transport_(transport) {
        auto& tc = const_cast<eph::net::TransportConfig&>(transport_.config());
        tc.on_message = [this](const uint8_t* data, uint16_t len, uint8_t) {
            ++msg_count_;
            uint64_t t_send = tsc::parse_T(data, len);
            if (t_send == 0) return;
            uint64_t now = eph::utils::TSC::now();
            if (now <= t_send) return;
            // Stage the round-trip: client_send=t_send (server stamp),
            // client_recv=now → BenchRunner records RTT = now - t_send.
            staged_.client_send_tsc = t_send;
            staged_.client_recv_tsc = now;
            // server_recv/send remain 0 → tx/rx/srv legs not recorded.
            got_msg_ = true;
        };
    }

    bool prepare(size_t /*payload*/) {
        msg_count_ = 0;
        return true;
    }

    bool do_one_round(RoundTrip& rt) {
        got_msg_ = false;
        // Poll until the next pushed message arrives or shutdown.
        while (!got_msg_
               && g_running.load(std::memory_order_relaxed)
               && transport_.is_running()) {
            [[maybe_unused]] auto _ = transport_.poll();
        }
        if (!got_msg_) return false;
        rt = staged_;
        return true;
    }

    void cleanup() {}

    uint64_t msg_count() const noexcept { return msg_count_; }

private:
    Transport& transport_;
    bool got_msg_ = false;
    RoundTrip staged_{};
    uint64_t msg_count_ = 0;
};

} // namespace bench::scenario
