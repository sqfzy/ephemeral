#pragma once

/// @file platform_attach.hpp
/// v3 secondary-side attach config — minimal "how to find primary" surface.
///
/// **Zero-consensus principle**: secondary should never need to know what
/// primary chose. The only required input is `file_prefix` (locates the
/// primary's shared-hugepage registry); everything else (queue ranges,
/// port ranges, NIC physical state) is read from the registry or queried
/// live via DPDK's per-port `rte_eth_dev_info_get`.
///
/// Compare to v2 `PlatformConfig` (which secondary used to fill with
/// `nb_rx_queues / nb_tx_queues / mp_topology` — all of which are
/// primary-decided values that secondary had to keep in lockstep). v3
/// secondary's input set is two fields, both of which are "how to find
/// primary" rather than "what primary did".
///
/// Used by `Platform::attach()` and `Platform::attach_with_eal()`. The
/// autojoin path (`Platform::join_dynamic`) is internally routed to
/// `attach()` when EAL resolves the process to secondary.

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace eph::dpdk {

/// @brief Secondary-side attach config (v3).
///
/// The library reads NIC physical state (nb_rx_queues, queue ranges,
/// mempool name) from the shared-hugepage registry that primary
/// publishes; secondary never restates these.
struct PlatformAttachConfig {
    /// @brief Required: hugepage namespace identifier matching primary's
    /// `PlatformConfig::file_prefix`. Empty value rejected at validation.
    /// Held as `string_view` to keep the struct a literal type — caller
    /// owns the backing storage (typical: string literal or
    /// long-lived application-owned string).
    std::string_view file_prefix {};

    /// @brief DPDK port enumeration index. Default 0 is correct for the
    /// single-NIC case where both primary and secondary pass `-a <bdf>`
    /// in EAL argv (DPDK enumerates allowed devs in argv order, so the
    /// first allowed dev is port_id=0). Multi-NIC setups must specify.
    uint16_t port_id = 0;

    /// @brief Optional: bitmask of lcores this secondary owns, used for
    /// cross-process lcore conflict detection (published into the slot's
    /// `lcore_mask` field at registry attach time). 0 = opt out.
    uint64_t self_lcore_mask = 0;

    [[nodiscard]] friend bool operator==(const PlatformAttachConfig&,
                                         const PlatformAttachConfig&) = default;

    /// One-line dump for diagnostic logging.
    [[nodiscard]] std::string dump() const {
        return std::format(
            "PlatformAttachConfig{{file_prefix='{}', port_id={}, "
            "self_lcore_mask=0x{:x}}}",
            file_prefix, port_id, self_lcore_mask);
    }
};

} // namespace eph::dpdk
