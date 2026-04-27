#pragma once

/// @file proc_type.hpp
/// Minimal cross-cutting header for `ProcType` — the DPDK process role
/// enum used by both `platform.hpp` (full Platform contract) and
/// `eal.hpp` (EAL argv assembly).
///
/// Lives in its own header so the enum has a single source of truth: a
/// future addition (e.g. `Auto`) automatically reaches every consumer
/// without the dual-source serialization hazard that motivated this
/// extraction (M3 of the MP-review fix sweep).

#include <cstdint>
#include <string_view>

namespace eph::dpdk {

/// @brief DPDK process role for single-NIC multi-process (primary+secondary).
///
/// Primary: full port lifecycle — enumerate, create mempool, configure /
///          start port, own RSS/RETA. Default for all existing code paths
///          (matches current single-process behavior byte-for-byte).
/// Secondary: attaches to an already-running primary via shared hugepage
///            (DPDK `--proc-type=secondary --file-prefix=<same as primary>`).
///            Looks up the primary's mempool by name; MUST NOT call
///            `rte_eth_dev_configure/start/stop/close` or
///            `rte_mempool_free` — these would corrupt the primary's
///            port state. Has its own Poller(s), Stream(s), FlowRule(s),
///            and ICMP registry (all per-process). The primary must
///            start first and outlive every secondary.
///
/// See `docs/dpdk-multiprocess.md` for startup/teardown ordering,
/// queue/src_port segmentation rules, and PMD-specific caveats.
enum class ProcType : uint8_t { Primary, Secondary };

/// @brief Serialize a ProcType to the string DPDK expects after
///        `--proc-type=`. Single source of truth — both `EalConfig`
///        argv assembly and any logging path must call this rather than
///        recomputing the mapping.
[[nodiscard]] constexpr std::string_view
to_eal_string(ProcType p) noexcept {
    switch (p) {
        case ProcType::Primary:   return "primary";
        case ProcType::Secondary: return "secondary";
    }
    // Unreachable in well-formed code; returning "primary" is the
    // conservative default (DPDK's auto behavior). If a future enum
    // value is added without updating this switch, GCC's
    // -Wreturn-type / -Wswitch will flag it at compile time.
    return "primary";
}

} // namespace eph::dpdk
