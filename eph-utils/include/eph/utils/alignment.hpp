#pragma once
#include <cstddef>

namespace eph::utils {

inline constexpr std::size_t CACHE_LINE_SIZE = 64;

template <typename T>
constexpr std::size_t Align =
    (alignof(T) > CACHE_LINE_SIZE) ? alignof(T) : CACHE_LINE_SIZE;

}  // namespace eph::utils
