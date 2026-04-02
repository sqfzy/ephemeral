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

/// @brief x86-64 cache line size in bytes.
///
/// Used to pad or align structures so that independent fields accessed by
/// different threads do not share a cache line (false sharing). All modern
/// x86-64 processors use 64-byte cache lines.
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
