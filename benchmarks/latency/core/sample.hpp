/// @file core/sample.hpp
/// Latency measurement samples used by the bench runner.
///
/// `RttSample` carries 4 TSC stamps for 4-leg breakdown (TX / RX / RTT /
/// Server-leg). Server stamps are 0 when the scenario does not provide
/// server-side timing — the runner skips those legs without recording.
///
/// `OneWaySample` is for unidirectional pipelines (e.g. exchange market
/// data push) where the server stamps just before send and the client
/// stamps on receive.
#pragma once

#include <cstdint>

namespace bench {

/// Four TSC stamps captured around a single request/response:
///   client_send → server_recv → server_send → client_recv
/// The runner records 4 legs: RTT = c_recv − c_send, TX = s_recv − c_send,
/// RX = c_recv − s_send, SRV = s_send − s_recv. Server stamps are 0 when a
/// scenario cannot provide them; the runner skips those legs.
struct RttSample {
    uint64_t client_send_tsc{};
    uint64_t server_recv_tsc{}; ///< 0 = leg unavailable for this scenario
    uint64_t server_send_tsc{}; ///< 0 = leg unavailable for this scenario
    uint64_t client_recv_tsc{};
};

/// One-way latency pair — for the exchange market data push scenario,
/// where the server stamps just before send and the client stamps on
/// receive. The delta is what `BenchRunner::run_oneway` records.
struct OneWaySample {
    uint64_t producer_tsc{}; ///< server stamp at send time
    uint64_t consumer_tsc{}; ///< client stamp at recv time
};

} // namespace bench
