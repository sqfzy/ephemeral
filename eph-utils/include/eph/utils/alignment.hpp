#pragma once
#include <cstddef>

#include "eph/base/cache.hpp"

namespace eph::utils {
template <typename T>
constexpr std::size_t Align =
    (alignof(T) > eph::base::CACHE_LINE_SIZE) ? alignof(T) : eph::base::CACHE_LINE_SIZE;
}  // namespace eph::utils
