/// @file alignment.hpp
/// @brief Cache-line alignment constants and helpers for avoiding false sharing.
///
/// In HFT systems, false sharing between CPU cores on adjacent cache lines
/// is a significant source of latency jitter. This header provides the
/// standard cache line size and a convenience alias to over-align types
/// to cache-line boundaries.

#pragma once
#include <cstddef>

namespace eph::utils {

/// @brief Cache line size in bytes for false-sharing avoidance.
///
/// Used to pad or align structures so independent fields accessed by
/// different threads do not share a cache line (false sharing).
///
/// Value is 64 bytes — correct for all x86-64 CPUs and the AArch64 cores
/// this codebase targets (Cortex-A72/A76/A78, Neoverse N1/V1, AWS
/// Graviton 2/3/4). Apple Silicon (M-series, 128 B) and IBM POWER (128 B)
/// will see padding undersized; the data layout still works but
/// false-sharing avoidance is weaker on those uarches. If portability
/// to those targets becomes a goal, switch to
/// `std::hardware_destructive_interference_size` (C++17, but still
/// implementation-defined on libstdc++ at the time of writing).
inline constexpr std::size_t CACHE_LINE_SIZE = 64;

/// @brief Alignment value that ensures @p T is at least cache-line aligned.
///
/// Returns `alignof(T)` when the type's natural alignment already exceeds
/// `CACHE_LINE_SIZE`, otherwise returns `CACHE_LINE_SIZE`. Use with
/// `alignas(Align<T>)` to prevent false sharing on hot-path data structures.
///
/// @tparam T The type whose alignment to compute.
template <typename T>
constexpr std::size_t Align =
    (alignof(T) > CACHE_LINE_SIZE) ? alignof(T) : CACHE_LINE_SIZE;

}  // namespace eph::utils
