#pragma once
#include <cstddef>

#include "eph/base/cache.hpp"

using eph::base::CACHE_LINE_SIZE;

namespace eph::utils {
template <typename T>
constexpr std::size_t Align =
    (alignof(T) > CACHE_LINE_SIZE) ? alignof(T) : CACHE_LINE_SIZE;
}  // namespace eph::utils
