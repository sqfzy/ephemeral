/// @file sampler.hpp
/// Concept describing an inter-arrival-time sampler for push scenarios.
///
/// A `Sampler` is a stateful generator that, each time `next_interval_ns()`
/// is called, returns the number of nanoseconds until the *next* send
/// event. The mock scheduler busy-waits for that duration, sends one
/// frame from the payload pool, then asks the sampler for the next
/// interval. `size_hint_bytes()` is an optional companion that lets the
/// pool pick a sample close to the target distribution; samplers that
/// don't model size can return a sentinel (0).
///
/// The concept is compile-time only — runtime dispatch between
/// samplers (MMPP-2 vs Hawkes vs Poisson) happens via template
/// instantiation at the scenario handler, not virtual calls, keeping
/// the inner loop free of indirect branches.
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>

namespace mockex {

/// Any type that can produce a sequence of inter-arrival durations
/// (ns) for the push loop. `next_interval_ns()` is called exactly
/// once per frame; state evolution (e.g. MMPP regime switching) is
/// the sampler's own responsibility.
template <typename T>
concept Sampler = requires(T& s) {
    { s.next_interval_ns() }  -> std::convertible_to<uint64_t>;
    { s.size_hint_bytes() }   -> std::convertible_to<size_t>;
};

} // namespace mockex
