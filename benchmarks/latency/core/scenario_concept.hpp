/// @file core/scenario_concept.hpp
/// Compile-time concepts every bench scenario must satisfy.
///
/// Two distinct concepts replace the old "1-leg vs 2-leg via payload=0
/// hack" pattern: RTT scenarios push a sample by performing a full
/// request/response, OneWay scenarios just wait for the next inbound
/// message and stamp it.
///
/// Hot-path dispatch is via template parameter — `BenchRunner` member
/// templates fully inline `do_one_*()` into the measurement loop.
#pragma once

#include <concepts>
#include <cstddef>

#include "sample.hpp"

namespace bench {

template <typename T>
concept RttScenario = requires(T& s, size_t payload, RttSample& out) {
    { s.prepare(payload) } -> std::same_as<bool>;
    { s.do_one_rtt(out)  } -> std::same_as<bool>;
    { s.cleanup()        } -> std::same_as<void>;
};

template <typename T>
concept OneWayScenario = requires(T& s, OneWaySample& out) {
    { s.prepare()        } -> std::same_as<bool>;
    { s.do_one_recv(out) } -> std::same_as<bool>;
    { s.cleanup()        } -> std::same_as<void>;
};

} // namespace bench
