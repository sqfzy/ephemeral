#pragma once

/// @file core/netns.hpp
/// Thin compat shim — the real implementation lives in
/// eph-utils/include/eph/utils/linux/netns.hpp.
///
/// New consumers should #include "eph/utils/linux/netns.hpp" directly.

#include "eph/utils/linux/netns.hpp"

namespace bench {

using ::eph::utils::linux_::enter_netns;

} // namespace bench
