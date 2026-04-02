#pragma once

/// @file concepts.hpp
/// @brief Core type constraints for eph lock-free containers.
///
/// Defines concepts that gate which types can be stored in the
/// lock-free queues and ring buffers of this library.

#include <concepts>
#include <type_traits>

namespace eph::containers {

/// @brief Constraint for types that can be stored in lock-free containers.
///
/// Only types satisfying TrivialData may be placed into the SPSC queues
/// and ring buffers. The requirements ensure that elements can be safely
/// copied via memcpy (trivially copyable), destroyed without side effects
/// (trivially destructible), and value-initialized into empty slots
/// (default initializable).
///
/// @tparam T The candidate element type.
template <typename T>
concept TrivialData =
    std::is_trivially_copyable_v<T> &&
    std::is_trivially_destructible_v<T> &&
    std::default_initializable<T>;

} // namespace eph::containers
