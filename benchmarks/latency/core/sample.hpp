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

struct RttSample {
    uint64_t client_send_tsc{};
    uint64_t server_recv_tsc{}; ///< 0 = leg unavailable for this scenario
    uint64_t server_send_tsc{}; ///< 0 = leg unavailable for this scenario
    uint64_t client_recv_tsc{};
};

struct OneWaySample {
    uint64_t producer_tsc{}; ///< server stamp at send time
    uint64_t consumer_tsc{}; ///< client stamp at recv time
};

} // namespace bench
