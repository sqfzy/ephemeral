#pragma once

/// @file containers.hpp
/// @brief Convenience umbrella header for the eph-containers library.
///
/// Including this single header pulls in every public container and the
/// `TrivialData` concept:
///   - `eph::containers::TrivialData`
///   - `eph::containers::RingBuffer<T, Capacity>`
///   - `eph::containers::BoundedQueue<T, Capacity>`
///   - `eph::containers::BoundedQueueBytes<MaxDataSize, Capacity>`
///   - `eph::containers::EvictingQueue<T, Capacity>` (incl. Capacity=1 specialization)
///   - `eph::containers::EvictingQueueBytes<MaxDataSize, Capacity>`
///
/// Prefer the individual headers in translation units that only need one
/// container to keep compile times down; use this header for convenience
/// in application code or tests that touch multiple containers.

#include "eph/containers/concepts.hpp"
#include "eph/containers/ring_buffer.hpp"
#include "eph/containers/bounded_queue.hpp"
#include "eph/containers/bounded_queue_bytes.hpp"
#include "eph/containers/evicting_queue.hpp"
#include "eph/containers/evicting_queue_bytes.hpp"
